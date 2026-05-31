// GARDVM Tier 4 — Interpreter Implementation (non-dispatch parts)
#include "interpreter.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace gardvm {

using namespace gard::bytecode;

static constexpr size_t MAX_STACK = 65536;
static constexpr size_t MAX_FRAMES = 2048;
static constexpr size_t MAX_LOCALS = 131072;

Interpreter::Interpreter() {
    stack_ = new GValue[MAX_STACK];
    frames_ = new Frame[MAX_FRAMES];
    locals_ = new GValue[MAX_LOCALS];
    localsCapacity_ = MAX_LOCALS;
    std::memset(locals_, 0, MAX_LOCALS * sizeof(GValue));
    sp_ = stack_;
    frameTop_ = frames_;
}

Interpreter::~Interpreter() {
    delete[] stack_;
    delete[] frames_;
    delete[] locals_;
}

void Interpreter::load(const BytecodeModule& module) {
    module_ = &module;
    funcLookup_.clear();
    methodTable_.clear();
    methodByName_.clear();

    // First pass: register all functions and create shapes
    for (uint16_t i = 0; i < (uint16_t)module.functions.size(); i++) {
        const std::string& fullName = module.functions[i].name;
        funcLookup_[fullName] = i;
        size_t dot = fullName.find('.');
        if (dot != std::string::npos) {
            std::string className = fullName.substr(0, dot);
            if (shapes_.getIdByName(className) == 0) {
                shapes_.createShape(className, 16);
            }
        }
    }

    // Second pass: build method dispatch table keyed by (shapeId, cpIndex)
    // For each "ClassName.method" function, find all constant pool entries
    // that match "method" and register the dispatch pair.
    for (uint16_t i = 0; i < (uint16_t)module.functions.size(); i++) {
        const std::string& fullName = module.functions[i].name;
        size_t dot = fullName.find('.');
        if (dot != std::string::npos) {
            std::string className = fullName.substr(0, dot);
            std::string methodName = fullName.substr(dot + 1);
            uint32_t shapeId = shapes_.getIdByName(className);

            // Find all constant pool entries that match this method name
            for (uint16_t ci = 0; ci < (uint16_t)module.constantPool.size(); ci++) {
                if (module.constantPool[ci].tag == ConstantTag::String &&
                    module.constantPool[ci].strVal == methodName) {
                    uint64_t key = ((uint64_t)shapeId << 32) | (uint64_t)ci;
                    methodTable_[key] = i;
                }
            }
        }
    }
}

void Interpreter::registerNative(const std::string& name, NativeFn fn) {
    natives_[name] = fn;
}

int Interpreter::run() {
    if (!module_ || module_->entryFunction < 0) {
        fprintf(stderr, "gardvm: no entry function\n");
        return 1;
    }
    uint16_t entryIdx = (uint16_t)module_->entryFunction;
    const FunctionInfo& entry = module_->functions[entryIdx];

    frameTop_ = frames_;
    frameTop_->returnPC = nullptr;
    frameTop_->bp = locals_;
    frameTop_->funcIndex = entryIdx;
    frameTop_->localCount = entry.localCount;
    frameTop_->closure = nullptr;

    fp_ = locals_;
    pc_ = module_->code.data() + entry.codeOffset;
    sp_ = stack_;
    running_ = true;
    exitCode_ = 0;

    execute();
    return exitCode_;
}

void Interpreter::pushFrame(uint16_t funcIdx, uint8_t argc) {
    if (frameTop_ >= frames_ + MAX_FRAMES - 1) {
        throwError("StackOverflowError", "Maximum call depth exceeded");
        return;
    }
    const FunctionInfo& func = module_->functions[funcIdx];
    GValue* newBp = fp_ + frameTop_->localCount;
    if (newBp + func.localCount >= locals_ + localsCapacity_) {
        throwError("StackOverflowError", "Local variable space exhausted");
        return;
    }

    // Copy args from operand stack to new locals
    GValue* argBase = sp_ - argc;
    uint8_t copyCount = argc < func.paramCount ? argc : func.paramCount;
    for (uint8_t i = 0; i < copyCount; i++) newBp[i] = argBase[i];
    for (uint16_t i = copyCount; i < func.localCount; i++) newBp[i] = GVAL_NULL;

    // Handle rest params
    if (func.hasRestParam && argc > func.paramCount - 1) {
        uint8_t restStart = func.restParamIndex;
        int32_t restCount = argc - restStart;
        if (restCount > 0) {
            GArray* arr = allocArray(restCount);
            for (int32_t i = 0; i < restCount; i++) {
                arr->data[i] = argBase[restStart + i];
            }
            arr->length = restCount;
            newBp[restStart] = GValue::makePtr(arr);
        }
    }

    sp_ = argBase; // pop args from operand stack

    Frame* newFrame = ++frameTop_;
    newFrame->returnPC = pc_;
    newFrame->bp = fp_;
    newFrame->funcIndex = funcIdx;
    newFrame->localCount = func.localCount;
    newFrame->closure = nullptr;

    fp_ = newBp;
    pc_ = module_->code.data() + func.codeOffset;
    insCount_++;
}

void Interpreter::popFrame() {
    if (frameTop_ <= frames_) { running_ = false; return; }
    Frame* frame = frameTop_--;
    pc_ = frame->returnPC;
    fp_ = frame->bp;
}

GString* Interpreter::allocString(const char* data, uint32_t len) {
    GString* s = (GString*)gc_.allocNursery(sizeof(GString));
    std::memset(s, 0, sizeof(GString));
    s->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    s->length = len;
    if (len <= STRING_INLINE_CAP) {
        s->kind = StringKind::Inline;
        std::memcpy(s->inlineData, data, len);
        s->inlineData[len] = '\0';
    } else {
        s->kind = StringKind::Heap;
        s->heap.buf = (char*)gc_.allocNursery(len + 1);
        std::memcpy(s->heap.buf, data, len);
        s->heap.buf[len] = '\0';
        s->heap.capacity = len + 1;
    }
    return s;
}

GArray* Interpreter::allocArray(int32_t cap) {
    if (cap < 256) cap = 256;  // large default to avoid grow issues
    GArray* arr = (GArray*)gc_.allocNursery(sizeof(GArray));
    arr->header = ObjHeader::make(ObjType::Array, 0, sizeof(GArray));
    arr->length = 0;
    arr->capacity = cap;
    arr->data = (GValue*)gc_.allocNursery(cap * sizeof(GValue));
    arr->elemType = 0;
    std::memset(arr->data, 0, cap * sizeof(GValue));
    return arr;
}

GObject* Interpreter::allocObject(uint32_t shapeId) {
    const Shape* shape = shapes_.get(shapeId);
    uint32_t fieldCount = shape ? shape->fieldCount : 32;
    size_t totalSize = sizeof(ObjHeader) + fieldCount * sizeof(GValue);
    GObject* obj = (GObject*)gc_.allocNursery(totalSize);
    if (!obj) return nullptr;
    obj->header = ObjHeader::make(ObjType::Object, shapeId, (uint32_t)totalSize);
    std::memset(obj->fields, 0, fieldCount * sizeof(GValue));
    return obj;
}

GString* Interpreter::concatStrings(GString* a, GString* b) {
    const char* ad = a->data();
    const char* bd = b->data();
    if (!ad || !bd) return a; // shouldn't happen with flat strings
    uint32_t newLen = a->length + b->length;

    GString* r = (GString*)gc_.allocNursery(sizeof(GString));
    if (!r) return a;
    std::memset(r, 0, sizeof(GString));
    r->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    r->length = newLen;

    if (newLen <= STRING_INLINE_CAP) {
        r->kind = StringKind::Inline;
        std::memcpy(r->inlineData, ad, a->length);
        std::memcpy(r->inlineData + a->length, bd, b->length);
        r->inlineData[newLen] = '\0';
    } else {
        r->kind = StringKind::Heap;
        r->heap.buf = (char*)gc_.allocNursery(newLen + 1);
        if (!r->heap.buf) return a;
        std::memcpy(r->heap.buf, ad, a->length);
        std::memcpy(r->heap.buf + a->length, bd, b->length);
        r->heap.buf[newLen] = '\0';
        r->heap.capacity = newLen + 1;
    }
    return r;
}

GValue Interpreter::getField(GObject* obj, uint16_t nameIdx) {
    if (nameIdx < 32) return obj->fields[nameIdx];
    return GVAL_NULL;
}

void Interpreter::setField(GObject* obj, uint16_t nameIdx, GValue val) {
    if (nameIdx < 32) obj->fields[nameIdx] = val;
}

void Interpreter::throwError(const char* type, const char* msg) {
    // Create exception as a GObject with fields at slots matching their nameIdx
    // The bytecode uses GET_FIELD with nameIdx, and our fallback uses nameIdx as slot directly.
    // So we store fields at their nameIdx positions.
    GObject* err = allocObject(0);
    if (err) {
        // Find the constant pool indices for "type", "message", "line", "column", "caller"
        int typeSlot = -1, msgSlot = -1, lineSlot = -1, colSlot = -1, callerSlot = -1;
        for (uint16_t ci = 0; ci < (uint16_t)module_->constantPool.size(); ci++) {
            if (module_->constantPool[ci].tag == gard::bytecode::ConstantTag::String) {
                const std::string& s = module_->constantPool[ci].strVal;
                if (s == "type") typeSlot = ci;
                else if (s == "message") msgSlot = ci;
                else if (s == "line") lineSlot = ci;
                else if (s == "column") colSlot = ci;
                else if (s == "caller") callerSlot = ci;
            }
        }
        // Store at the nameIdx slot (which GET_FIELD will use directly)
        if (typeSlot >= 0 && typeSlot < 32)
            err->fields[typeSlot] = GValue::makePtr(allocString(type, (uint32_t)strlen(type)));
        if (msgSlot >= 0 && msgSlot < 32)
            err->fields[msgSlot] = GValue::makePtr(allocString(msg, (uint32_t)strlen(msg)));
        if (lineSlot >= 0 && lineSlot < 32)
            err->fields[lineSlot] = GValue::makeInt((int32_t)currentLine_);
        if (colSlot >= 0 && colSlot < 32)
            err->fields[colSlot] = GValue::makeInt((int32_t)currentCol_);
        if (callerSlot >= 0 && callerSlot < 32 && frameTop_ >= frames_) {
            const std::string& fn = module_->functions[frameTop_->funcIndex].name;
            err->fields[callerSlot] = GValue::makePtr(allocString(fn.c_str(), (uint32_t)fn.size()));
        }
        currentException_ = GValue::makePtr(err);
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s: %s", type, msg);
        currentException_ = GValue::makePtr(allocString(buf, (uint32_t)strlen(buf)));
    }

    if (!unwindToHandler()) {
        fprintf(stderr, "\n\033[1;31m%s\033[0m: %s\n", type, msg);
        if (currentLine_ > 0) fprintf(stderr, "    at line %d:%d\n", currentLine_, currentCol_);
        running_ = false;
        exitCode_ = 1;
    }
}

bool Interpreter::unwindToHandler() {
    if (handlers_.empty()) return false;
    ExHandler& h = handlers_.back();
    pc_ = h.catchPC;
    sp_ = h.savedSP;
    frameTop_ = h.savedFrame;
    fp_ = frameTop_->bp;
    push(currentException_);
    currentException_ = GVAL_NULL;
    handlers_.pop_back();
    return true;
}

// ============================================================================
// Standard Library Registration
// ============================================================================

static GValue native_print(GValue* args, int32_t argc) {
    for (int32_t i = 0; i < argc; i++) {
        GValue v = args[i];
        if (v.isInt()) printf("%d", v.asInt());
        else if (v.isDouble()) printf("%g", v.asDouble());
        else if (v.isBool()) printf("%s", v.asBool() ? "true" : "false");
        else if (v.isNull()) printf("null");
        else if (v.isPtr()) {
            void* p = v.asPtr();
            if (p) {
                ObjHeader* h = (ObjHeader*)p;
                if (h->type() == ObjType::String) {
                    GString* s = (GString*)p;
                    const char* d = s->data();
                    if (d) printf("%.*s", (int)s->length, d);
                } else {
                    printf("<object>");
                }
            } else {
                printf("null");
            }
        }
        if (i < argc - 1) printf(" ");
    }
    printf("\n");
    return GVAL_NULL;
}

void Interpreter::registerStdlib() {
    registerNative("print", native_print);

    // Error constructors — create GObject with type/message/line/column fields
    auto makeErrorCtor = [](const char* typeName) -> NativeFn {
        // We can't capture typeName in a plain function pointer, so use a static dispatch
        return nullptr; // handled below
    };
    (void)makeErrorCtor;

    // Register all Gard error types as natives that create error objects
    // They take 1 arg (message string) and return an object with fields at nameIdx slots
    // Since we can't easily create objects from a static function (need GC access),
    // we handle error constructors specially in CALL_VIRTUAL: if the name starts with "Gard"
    // and ends with "Error", treat it as an error constructor.

    // List operations
    registerNative("List.add", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 2 || !args[0].isPtr()) return GVAL_NULL;
        GArray* arr = (GArray*)args[0].asPtr();
        if (arr->length < arr->capacity) {
            arr->data[arr->length++] = args[1];
        }
        return GVAL_NULL;
    });

    registerNative("List.get", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 2 || !args[0].isPtr()) return GVAL_NULL;
        GArray* arr = (GArray*)args[0].asPtr();
        int32_t idx = args[1].asInt();
        if (idx < 0 || idx >= arr->length) return GVAL_NULL;
        return arr->data[idx];
    });

    registerNative("List.set", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 3 || !args[0].isPtr()) return GVAL_NULL;
        GArray* arr = (GArray*)args[0].asPtr();
        int32_t idx = args[1].asInt();
        if (idx >= 0 && idx < arr->length) arr->data[idx] = args[2];
        return GVAL_NULL;
    });

    registerNative("List.length", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 1 || !args[0].isPtr()) return GVAL_ZERO;
        GArray* arr = (GArray*)args[0].asPtr();
        return GValue::makeInt(arr->length);
    });

    registerNative("List.pop", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 1 || !args[0].isPtr()) return GVAL_NULL;
        GArray* arr = (GArray*)args[0].asPtr();
        if (arr->length == 0) return GVAL_NULL;
        return arr->data[--arr->length];
    });

    registerNative("List.contains", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 2 || !args[0].isPtr()) return GVAL_FALSE;
        GArray* arr = (GArray*)args[0].asPtr();
        for (int32_t i = 0; i < arr->length; i++) {
            if (arr->data[i] == args[1]) return GVAL_TRUE;
        }
        return GVAL_FALSE;
    });

    registerNative("List.removeAt", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 2 || !args[0].isPtr()) return GVAL_NULL;
        GArray* arr = (GArray*)args[0].asPtr();
        int32_t idx = args[1].asInt();
        if (idx < 0 || idx >= arr->length) return GVAL_NULL;
        for (int32_t i = idx; i < arr->length - 1; i++) arr->data[i] = arr->data[i+1];
        arr->length--;
        return GVAL_NULL;
    });

    registerNative("List.indexOf", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 2 || !args[0].isPtr()) return GValue::makeInt(-1);
        GArray* arr = (GArray*)args[0].asPtr();
        for (int32_t i = 0; i < arr->length; i++) {
            if (arr->data[i] == args[1]) return GValue::makeInt(i);
        }
        return GValue::makeInt(-1);
    });

    registerNative("List.reverse", [](GValue* args, int32_t argc) -> GValue {
        if (argc < 1 || !args[0].isPtr()) return GVAL_NULL;
        GArray* arr = (GArray*)args[0].asPtr();
        for (int32_t i = 0, j = arr->length - 1; i < j; i++, j--) {
            GValue tmp = arr->data[i]; arr->data[i] = arr->data[j]; arr->data[j] = tmp;
        }
        return GVAL_NULL;
    });
}

} // namespace gardvm
