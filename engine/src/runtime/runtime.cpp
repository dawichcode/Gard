#include "runtime/runtime.h"
#include "runtime/async.h"
#include "runtime/stdlib.h"
#include "jit/jit_bridge.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <thread>

namespace gard {
namespace runtime {

// --- Value ---

Value Value::makeString(const std::string& val) {
    Value v;
    v.type = ValueType::String;
    v.strVal = std::make_shared<GardString>(val);
    return v;
}

Value Value::makeArray() {
    Value v;
    v.type = ValueType::Array;
    v.arrVal = std::make_shared<GardArray>();
    return v;
}

Value Value::makeMap() {
    Value v;
    v.type = ValueType::Map;
    v.mapVal = std::make_shared<GardMap>();
    return v;
}

Value Value::makeObject(const std::string& className) {
    Value v;
    v.type = ValueType::Object;
    v.objVal = std::make_shared<GardObject>(className);
    return v;
}

std::string Value::toString() const {
    switch (type) {
        case ValueType::Null: return "null";
        case ValueType::Int: return std::to_string(intVal);
        case ValueType::Long: return std::to_string(longVal);
        case ValueType::Float: return std::to_string(floatVal);
        case ValueType::Double: return std::to_string(doubleVal);
        case ValueType::Bool: return boolVal ? "true" : "false";
        case ValueType::Char: return std::string(1, charVal);
        case ValueType::String: return strVal ? strVal->data : "";
        case ValueType::Array: {
            if (!arrVal) return "[]";
            if (arrVal->elements.empty()) return "[]";
            std::string r = "[";
            for (size_t i = 0; i < arrVal->elements.size(); i++) {
                if (i > 0) r += ", ";
                if (i > 20) { r += "..."; break; }
                r += arrVal->elements[i].toString();
            }
            return r + "]";
        }
        case ValueType::Map: {
            if (!mapVal) return "{}";
            if (mapVal->entries.empty()) return "{}";
            std::string r = "{";
            bool first = true;
            for (auto& [k, v] : mapVal->entries) {
                if (!first) r += ", ";
                r += k + ": " + v.toString();
                first = false;
            }
            return r + "}";
        }
        case ValueType::Object: return objVal ? ("[" + objVal->className + "]") : "[Object]";
        case ValueType::Function: return "[Function]";
    }
    return "undefined";
}

int32_t Value::toInt() const {
    switch (type) {
        case ValueType::Int: return intVal;
        case ValueType::Long: return static_cast<int32_t>(longVal);
        case ValueType::Float: return static_cast<int32_t>(floatVal);
        case ValueType::Double: return static_cast<int32_t>(doubleVal);
        case ValueType::Bool: return boolVal ? 1 : 0;
        case ValueType::Char: return static_cast<int32_t>(charVal);
        default: return 0;
    }
}

double Value::toDouble() const {
    switch (type) {
        case ValueType::Int: return static_cast<double>(intVal);
        case ValueType::Long: return static_cast<double>(longVal);
        case ValueType::Float: return static_cast<double>(floatVal);
        case ValueType::Double: return doubleVal;
        case ValueType::Bool: return boolVal ? 1.0 : 0.0;
        default: return 0.0;
    }
}

bool Value::toBool() const {
    switch (type) {
        case ValueType::Null: return false;
        case ValueType::Int: return intVal != 0;
        case ValueType::Long: return longVal != 0;
        case ValueType::Float: return floatVal != 0.0f;
        case ValueType::Double: return doubleVal != 0.0;
        case ValueType::Bool: return boolVal;
        case ValueType::String: return strVal && !strVal->data.empty();
        case ValueType::Array: return arrVal && !arrVal->elements.empty();
        case ValueType::Object: return objVal != nullptr;
        default: return false;
    }
}

// --- VM ---

VM::VM(const VMConfig& config) : config_(config) {
    stack_.reserve(1024);
    locals_.resize(4096); // pre-allocate local variable space
    callStack_.reserve(64);
    argsBuffer_.reserve(16); // pre-allocate args buffer (avoids malloc in CALL)

    // Register built-in async runtime functions
    registerNative("Promise.all", [](const std::vector<Value>& args) -> Value {
        // In sync mode: just return the array of results as-is
        if (!args.empty() && args[0].type == ValueType::Array) {
            return args[0];
        }
        return Value::makeArray();
    });

    registerNative("Task.delay", [](const std::vector<Value>&) -> Value {
        // In sync mode: no-op (would suspend in async mode)
        return Value::makeNull();
    });

    registerNative("Stream.create", [](const std::vector<Value>&) -> Value {
        return Value::makeArray(); // stream backed by array buffer
    });

    registerNative("Stream.write", [](const std::vector<Value>& args) -> Value {
        return Value::makeNull();
    });

    registerNative("Stream.read", [](const std::vector<Value>&) -> Value {
        return Value::makeNull();
    });

    registerNative("Stream.close", [](const std::vector<Value>&) -> Value {
        return Value::makeNull();
    });

    // Register standard library
    stdlib::registerAll(*this);

    // Mark I/O-bound natives as always-async (fire-and-forget without await)
    stdlib::registerAsyncNatives(*this);
}

VM::~VM() = default;

int VM::run(const bytecode::BytecodeModule& module) {
    module_ = &module;

    if (module.entryFunction < 0 ||
        module.entryFunction >= static_cast<int16_t>(module.functions.size())) {
        std::cerr << "Runtime error: no entry function (main) found" << std::endl;
        return 1;
    }

    // Build function lookup cache: name → index (eliminates O(n) scans in CALL/CALL_VIRTUAL)
    funcLookup_.clear();
    funcLookup_.reserve(module.functions.size() * 2);
    for (uint16_t i = 0; i < module.functions.size(); i++) {
        funcLookup_[module.functions[i].name] = i;
    }

    // Initialize JIT engine (lazy — only compiles hot functions)
    if (!jit_) {
        jit_ = std::make_unique<jit::JitDispatcher>();
    }

    // Set up initial call frame
    auto& entryFn = module.functions[module.entryFunction];
    pc_ = entryFn.codeOffset;

    CallFrame frame;
    frame.functionIndex = static_cast<uint16_t>(module.entryFunction);
    frame.returnPC = 0;
    frame.basePointer = 0;
    frame.functionName = entryFn.name;
    callStack_.push_back(frame);

    running_ = true;
    exitCode_ = 0;

    execute();

    return exitCode_;
}

void VM::registerNative(const std::string& name, NativeFunc func) {
    natives_[name] = std::move(func);
}

void VM::registerAsyncNative(const std::string& name) {
    asyncNatives_.insert(name);
}

void VM::execute() {
    using namespace bytecode;
    // Computed goto dispatch table — each opcode maps to a label address
    // This eliminates the switch overhead and enables better branch prediction
#ifdef __GNUC__
    // GCC/Clang computed goto (2-3x faster than switch dispatch)
    #define DISPATCH() goto *dispatchTable[module_->code[pc_++]]
    #define TARGET(op) TARGET_##op

    static const void* dispatchTable[256] = {};
    // Initialize dispatch table (done once, static)
    static bool tableInit = false;
    if (!tableInit) {
        for (int i = 0; i < 256; i++) const_cast<const void*&>(dispatchTable[i]) = &&TARGET(HALT);
        const_cast<const void*&>(dispatchTable[0x00]) = &&TARGET(NOP);
        const_cast<const void*&>(dispatchTable[0x01]) = &&TARGET(POP);
        const_cast<const void*&>(dispatchTable[0x02]) = &&TARGET(DUP);
        const_cast<const void*&>(dispatchTable[0x03]) = &&TARGET(SWAP);
        const_cast<const void*&>(dispatchTable[0x10]) = &&TARGET(CONST_NULL);
        const_cast<const void*&>(dispatchTable[0x11]) = &&TARGET(CONST_TRUE);
        const_cast<const void*&>(dispatchTable[0x12]) = &&TARGET(CONST_FALSE);
        const_cast<const void*&>(dispatchTable[0x13]) = &&TARGET(CONST_I32);
        const_cast<const void*&>(dispatchTable[0x14]) = &&TARGET(CONST_I64);
        const_cast<const void*&>(dispatchTable[0x15]) = &&TARGET(CONST_F32);
        const_cast<const void*&>(dispatchTable[0x16]) = &&TARGET(CONST_F64);
        const_cast<const void*&>(dispatchTable[0x17]) = &&TARGET(CONST_STR);
        const_cast<const void*&>(dispatchTable[0x20]) = &&TARGET(LOAD_LOCAL);
        const_cast<const void*&>(dispatchTable[0x21]) = &&TARGET(STORE_LOCAL);
        const_cast<const void*&>(dispatchTable[0x22]) = &&TARGET(LOAD_GLOBAL);
        const_cast<const void*&>(dispatchTable[0x23]) = &&TARGET(STORE_GLOBAL);
        const_cast<const void*&>(dispatchTable[0x30]) = &&TARGET(ADD_I);
        const_cast<const void*&>(dispatchTable[0x31]) = &&TARGET(SUB_I);
        const_cast<const void*&>(dispatchTable[0x32]) = &&TARGET(MUL_I);
        const_cast<const void*&>(dispatchTable[0x33]) = &&TARGET(DIV_I);
        const_cast<const void*&>(dispatchTable[0x34]) = &&TARGET(MOD_I);
        const_cast<const void*&>(dispatchTable[0x35]) = &&TARGET(NEG_I);
        const_cast<const void*&>(dispatchTable[0x36]) = &&TARGET(ADD_F);
        const_cast<const void*&>(dispatchTable[0x37]) = &&TARGET(SUB_F);
        const_cast<const void*&>(dispatchTable[0x38]) = &&TARGET(MUL_F);
        const_cast<const void*&>(dispatchTable[0x39]) = &&TARGET(DIV_F);
        const_cast<const void*&>(dispatchTable[0x3A]) = &&TARGET(NEG_F);
        const_cast<const void*&>(dispatchTable[0x40]) = &&TARGET(BIT_AND);
        const_cast<const void*&>(dispatchTable[0x41]) = &&TARGET(BIT_OR);
        const_cast<const void*&>(dispatchTable[0x42]) = &&TARGET(BIT_XOR);
        const_cast<const void*&>(dispatchTable[0x43]) = &&TARGET(BIT_NOT);
        const_cast<const void*&>(dispatchTable[0x44]) = &&TARGET(SHL);
        const_cast<const void*&>(dispatchTable[0x45]) = &&TARGET(SHR);
        const_cast<const void*&>(dispatchTable[0x46]) = &&TARGET(USHR);
        const_cast<const void*&>(dispatchTable[0x50]) = &&TARGET(CMP_EQ);
        const_cast<const void*&>(dispatchTable[0x51]) = &&TARGET(CMP_NE);
        const_cast<const void*&>(dispatchTable[0x52]) = &&TARGET(CMP_LT);
        const_cast<const void*&>(dispatchTable[0x53]) = &&TARGET(CMP_GT);
        const_cast<const void*&>(dispatchTable[0x54]) = &&TARGET(CMP_LE);
        const_cast<const void*&>(dispatchTable[0x55]) = &&TARGET(CMP_GE);
        const_cast<const void*&>(dispatchTable[0x58]) = &&TARGET(LOG_AND);
        const_cast<const void*&>(dispatchTable[0x59]) = &&TARGET(LOG_OR);
        const_cast<const void*&>(dispatchTable[0x5A]) = &&TARGET(LOG_NOT);
        const_cast<const void*&>(dispatchTable[0x60]) = &&TARGET(JUMP);
        const_cast<const void*&>(dispatchTable[0x61]) = &&TARGET(JUMP_IF);
        const_cast<const void*&>(dispatchTable[0x62]) = &&TARGET(JUMP_IFNOT);
        const_cast<const void*&>(dispatchTable[0x63]) = &&TARGET(RETURN);
        const_cast<const void*&>(dispatchTable[0x64]) = &&TARGET(RETURN_VOID);
        const_cast<const void*&>(dispatchTable[0x70]) = &&TARGET(CALL);
        const_cast<const void*&>(dispatchTable[0x71]) = &&TARGET(CALL_VIRTUAL);
        const_cast<const void*&>(dispatchTable[0x72]) = &&TARGET(CALL_ASYNC);
        const_cast<const void*&>(dispatchTable[0x80]) = &&TARGET(NEW_OBJ);
        const_cast<const void*&>(dispatchTable[0x81]) = &&TARGET(GET_FIELD);
        const_cast<const void*&>(dispatchTable[0x82]) = &&TARGET(SET_FIELD);
        const_cast<const void*&>(dispatchTable[0x83]) = &&TARGET(NEW_ARRAY);
        const_cast<const void*&>(dispatchTable[0x84]) = &&TARGET(GET_ELEM);
        const_cast<const void*&>(dispatchTable[0x85]) = &&TARGET(SET_ELEM);
        const_cast<const void*&>(dispatchTable[0x86]) = &&TARGET(NEW_MAP);
        const_cast<const void*&>(dispatchTable[0x87]) = &&TARGET(ARRAY_LEN);
        const_cast<const void*&>(dispatchTable[0x90]) = &&TARGET(CONCAT);
        const_cast<const void*&>(dispatchTable[0xA0]) = &&TARGET(CAST);
        const_cast<const void*&>(dispatchTable[0xA1]) = &&TARGET(IS_TYPE);
        const_cast<const void*&>(dispatchTable[0xA2]) = &&TARGET(IS_NULL);
        const_cast<const void*&>(dispatchTable[0xB0]) = &&TARGET(AWAIT);
        const_cast<const void*&>(dispatchTable[0xB1]) = &&TARGET(YIELD);
        const_cast<const void*&>(dispatchTable[0xB8]) = &&TARGET(TRY_BEGIN);
        const_cast<const void*&>(dispatchTable[0xB9]) = &&TARGET(TRY_END);
        const_cast<const void*&>(dispatchTable[0xBA]) = &&TARGET(THROW);
        const_cast<const void*&>(dispatchTable[0xC0]) = &&TARGET(PRINT);
        const_cast<const void*&>(dispatchTable[0xD0]) = &&TARGET(CLOSURE);
        const_cast<const void*&>(dispatchTable[0xD1]) = &&TARGET(LOAD_CAPTURE);
        // Superinstructions
        const_cast<const void*&>(dispatchTable[0xE0]) = &&TARGET(LOAD_ADD_I);
        const_cast<const void*&>(dispatchTable[0xE1]) = &&TARGET(LOAD_SUB_I);
        const_cast<const void*&>(dispatchTable[0xE2]) = &&TARGET(LOAD_CMP_LT);
        const_cast<const void*&>(dispatchTable[0xE3]) = &&TARGET(LOAD_CMP_GE);
        const_cast<const void*&>(dispatchTable[0xE4]) = &&TARGET(INC_LOCAL);
        const_cast<const void*&>(dispatchTable[0xE5]) = &&TARGET(DEC_LOCAL);
        const_cast<const void*&>(dispatchTable[0xE6]) = &&TARGET(LOAD_CONST_ADD);
        const_cast<const void*&>(dispatchTable[0xE7]) = &&TARGET(LOAD_STORE);
        const_cast<const void*&>(dispatchTable[0xF0]) = &&TARGET(LINE);
        const_cast<const void*&>(dispatchTable[0xFF]) = &&TARGET(HALT);
        tableInit = true;
    }

    DISPATCH();

    TARGET(NOP): { DISPATCH(); }
    TARGET(HALT): { running_ = false; return; }

    TARGET(CONST_NULL): { push(Value()); DISPATCH(); }
    TARGET(CONST_TRUE): { Value v; v.type = ValueType::Bool; v.boolVal = true; push(v); DISPATCH(); }
    TARGET(CONST_FALSE): { Value v; v.type = ValueType::Bool; v.boolVal = false; push(v); DISPATCH(); }
    TARGET(CONST_I32): { push(Value::makeInt(readI32())); DISPATCH(); }
    TARGET(CONST_I64): { push(Value::makeLong(readI64())); DISPATCH(); }
    TARGET(CONST_F32): { push(Value::makeFloat(readF32())); DISPATCH(); }
    TARGET(CONST_F64): { push(Value::makeDouble(readF64())); DISPATCH(); }
    TARGET(CONST_STR): {
        uint16_t idx = readU16();
        const std::string& s = getConstantString(idx);
        // String interning: reuse existing GardString for same content
        auto it = internedStrings_.find(s);
        if (__builtin_expect(it != internedStrings_.end(), 1)) {
            Value v; v.type = ValueType::String; v.strVal = it->second;
            push(v);
        } else {
            auto gs = std::make_shared<GardString>(s);
            internedStrings_[s] = gs;
            Value v; v.type = ValueType::String; v.strVal = gs;
            push(v);
        }
        DISPATCH();
    }

    TARGET(LOAD_LOCAL): { uint16_t idx = readU16(); push(locals_[callStack_.back().basePointer + idx]); DISPATCH(); }
    TARGET(STORE_LOCAL): { uint16_t idx = readU16(); locals_[callStack_.back().basePointer + idx] = pop(); DISPATCH(); }
    TARGET(LOAD_GLOBAL): { readU16(); push(Value()); DISPATCH(); }
    TARGET(STORE_GLOBAL): { readU16(); pop(); DISPATCH(); }

    TARGET(POP): { pop(); DISPATCH(); }
    TARGET(DUP): { push(peek()); DISPATCH(); }
    TARGET(SWAP): { Value a = pop(), b = pop(); push(a); push(b); DISPATCH(); }

    TARGET(ADD_I): {
        // In-place stack arithmetic: avoid pop+push overhead
        auto sz = stack_.size();
        Value& b = stack_[sz - 1];
        Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.intVal += b.intVal;
            stack_.pop_back();
        } else if (a.type == ValueType::String || b.type == ValueType::String) {
            Value result = Value::makeString(a.toString() + b.toString());
            stack_.pop_back(); stack_.pop_back(); push(result);
        } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
            int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
            int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
            a = Value::makeLong(av + bv); stack_.pop_back();
        } else if (a.type == ValueType::Double || b.type == ValueType::Double) {
            a = Value::makeDouble(a.toDouble() + b.toDouble()); stack_.pop_back();
        } else {
            Value va = pop(), vb = pop(); push(va); push(vb);
            executeInstruction(OpCode::ADD_I);
            if (!running_) return;
            goto dispatch_end;
        }
        DISPATCH();
    }
    TARGET(SUB_I): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1];
        Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.intVal -= b.intVal;
            stack_.pop_back();
        } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
            int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
            int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
            a = Value::makeLong(av - bv); stack_.pop_back();
        } else if (a.type == ValueType::Double || b.type == ValueType::Double) {
            a = Value::makeDouble(a.toDouble() - b.toDouble()); stack_.pop_back();
        } else {
            Value va = pop(), vb = pop(); push(va); push(vb);
            executeInstruction(OpCode::SUB_I);
            if (!running_) return;
            goto dispatch_end;
        }
        DISPATCH();
    }
    TARGET(MUL_I): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1];
        Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.intVal *= b.intVal;
            stack_.pop_back();
        } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
            int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
            int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
            a = Value::makeLong(av * bv); stack_.pop_back();
        } else if (a.type == ValueType::Double || b.type == ValueType::Double) {
            a = Value::makeDouble(a.toDouble() * b.toDouble()); stack_.pop_back();
        } else {
            Value va = pop(), vb = pop(); push(va); push(vb);
            executeInstruction(OpCode::MUL_I);
            if (!running_) return;
            goto dispatch_end;
        }
        DISPATCH();
    }
    TARGET(DIV_I): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1];
        Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            if (__builtin_expect(b.intVal == 0, 0)) { throwError("GardArithmeticError", "Division by zero"); return; }
            a.intVal /= b.intVal;
            stack_.pop_back();
        } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
            int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
            if (bv == 0) { throwError("GardArithmeticError", "Division by zero"); return; }
            int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
            a = Value::makeLong(av / bv); stack_.pop_back();
        } else if (a.type == ValueType::Double || b.type == ValueType::Double) {
            if (b.toDouble() == 0) { throwError("GardArithmeticError", "Division by zero"); return; }
            a = Value::makeDouble(a.toDouble() / b.toDouble()); stack_.pop_back();
        } else {
            Value va = pop(), vb = pop(); push(va); push(vb);
            executeInstruction(OpCode::DIV_I);
            if (!running_) return;
            goto dispatch_end;
        }
        DISPATCH();
    }
    TARGET(MOD_I): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1];
        Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            if (__builtin_expect(b.intVal == 0, 0)) { throwError("GardArithmeticError", "Modulo by zero"); return; }
            a.intVal %= b.intVal;
            stack_.pop_back();
        } else {
            Value va = pop(), vb = pop(); push(va); push(vb);
            executeInstruction(OpCode::MOD_I);
            if (!running_) return;
            goto dispatch_end;
        }
        DISPATCH();
    }
    TARGET(NEG_I): { Value a = pop(); push(Value::makeInt(-a.toInt())); DISPATCH(); }

    TARGET(CMP_LT): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1]; Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.type = ValueType::Bool; a.boolVal = (a.intVal < b.intVal);
        } else { a.type = ValueType::Bool; a.boolVal = (a.toDouble() < b.toDouble()); }
        stack_.pop_back();
        DISPATCH();
    }
    TARGET(CMP_GT): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1]; Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.type = ValueType::Bool; a.boolVal = (a.intVal > b.intVal);
        } else { a.type = ValueType::Bool; a.boolVal = (a.toDouble() > b.toDouble()); }
        stack_.pop_back();
        DISPATCH();
    }
    TARGET(CMP_LE): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1]; Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.type = ValueType::Bool; a.boolVal = (a.intVal <= b.intVal);
        } else { a.type = ValueType::Bool; a.boolVal = (a.toDouble() <= b.toDouble()); }
        stack_.pop_back();
        DISPATCH();
    }
    TARGET(CMP_GE): {
        auto sz = stack_.size();
        Value& b = stack_[sz - 1]; Value& a = stack_[sz - 2];
        if (__builtin_expect(a.type == ValueType::Int && b.type == ValueType::Int, 1)) {
            a.type = ValueType::Bool; a.boolVal = (a.intVal >= b.intVal);
        } else { a.type = ValueType::Bool; a.boolVal = (a.toDouble() >= b.toDouble()); }
        stack_.pop_back();
        DISPATCH();
    }

    TARGET(JUMP): {
        uint16_t target = readU16();
        // Hot loop detection: backward jump = loop back-edge
        if (__builtin_expect(target < pc_, 0)) {
            uint32_t& count = loopCounters_[target];
            if (__builtin_expect(++count == HOT_LOOP_THRESHOLD, 0)) {
                hotLoops_.insert(target);
            }
        }
        pc_ = target;
        DISPATCH();
    }
    TARGET(JUMP_IF): {
        uint16_t target = readU16();
        if (pop().toBool()) {
            if (__builtin_expect(target < pc_, 0)) {
                uint32_t& count = loopCounters_[target];
                if (__builtin_expect(++count == HOT_LOOP_THRESHOLD, 0)) {
                    hotLoops_.insert(target);
                }
            }
            pc_ = target;
        }
        DISPATCH();
    }
    TARGET(JUMP_IFNOT): {
        uint16_t target = readU16();
        if (!pop().toBool()) {
            if (__builtin_expect(target < pc_, 0)) {
                uint32_t& count = loopCounters_[target];
                if (__builtin_expect(++count == HOT_LOOP_THRESHOLD, 0)) {
                    hotLoops_.insert(target);
                }
            }
            pc_ = target;
        }
        DISPATCH();
    }

    TARGET(LOG_AND): { Value b = pop(), a = pop(); push(Value::makeBool(a.toBool() && b.toBool())); DISPATCH(); }
    TARGET(LOG_OR): { Value b = pop(), a = pop(); push(Value::makeBool(a.toBool() || b.toBool())); DISPATCH(); }
    TARGET(LOG_NOT): { Value a = pop(); push(Value::makeBool(!a.toBool())); DISPATCH(); }

    // === Superinstructions (fused hot paths — single dispatch for multiple ops) ===

    TARGET(LOAD_ADD_I): {
        // LOAD_LOCAL(a) + LOAD_LOCAL(b) + ADD_I in one dispatch
        uint16_t idxA = readU16(), idxB = readU16();
        uint16_t base = callStack_.back().basePointer;
        Value& a = locals_[base + idxA];
        Value& b = locals_[base + idxB];
        if (a.type == ValueType::Int && b.type == ValueType::Int)
            push(Value::makeInt(a.intVal + b.intVal));
        else
            push(Value::makeDouble(a.toDouble() + b.toDouble()));
        DISPATCH();
    }
    TARGET(LOAD_SUB_I): {
        uint16_t idxA = readU16(), idxB = readU16();
        uint16_t base = callStack_.back().basePointer;
        Value& a = locals_[base + idxA];
        Value& b = locals_[base + idxB];
        if (a.type == ValueType::Int && b.type == ValueType::Int)
            push(Value::makeInt(a.intVal - b.intVal));
        else
            push(Value::makeDouble(a.toDouble() - b.toDouble()));
        DISPATCH();
    }
    TARGET(LOAD_CMP_LT): {
        uint16_t idxA = readU16(), idxB = readU16();
        uint16_t base = callStack_.back().basePointer;
        Value& a = locals_[base + idxA];
        Value& b = locals_[base + idxB];
        if (a.type == ValueType::Int && b.type == ValueType::Int)
            push(Value::makeBool(a.intVal < b.intVal));
        else
            push(Value::makeBool(a.toDouble() < b.toDouble()));
        DISPATCH();
    }
    TARGET(LOAD_CMP_GE): {
        uint16_t idxA = readU16(), idxB = readU16();
        uint16_t base = callStack_.back().basePointer;
        Value& a = locals_[base + idxA];
        Value& b = locals_[base + idxB];
        if (a.type == ValueType::Int && b.type == ValueType::Int)
            push(Value::makeBool(a.intVal >= b.intVal));
        else
            push(Value::makeBool(a.toDouble() >= b.toDouble()));
        DISPATCH();
    }
    TARGET(INC_LOCAL): {
        // i = i + 1 in one dispatch (no stack push/pop)
        uint16_t idx = readU16();
        uint16_t base = callStack_.back().basePointer;
        Value& v = locals_[base + idx];
        if (v.type == ValueType::Int) v.intVal++;
        else if (v.type == ValueType::Long) v.longVal++;
        else if (v.type == ValueType::Double) v.doubleVal += 1.0;
        DISPATCH();
    }
    TARGET(DEC_LOCAL): {
        uint16_t idx = readU16();
        uint16_t base = callStack_.back().basePointer;
        Value& v = locals_[base + idx];
        if (v.type == ValueType::Int) v.intVal--;
        else if (v.type == ValueType::Long) v.longVal--;
        else if (v.type == ValueType::Double) v.doubleVal -= 1.0;
        DISPATCH();
    }
    TARGET(LOAD_CONST_ADD): {
        // LOAD_LOCAL(x) + CONST_I32(n) + ADD_I in one dispatch
        uint16_t idx = readU16();
        int32_t constVal = readI32();
        uint16_t base = callStack_.back().basePointer;
        Value& v = locals_[base + idx];
        if (v.type == ValueType::Int)
            push(Value::makeInt(v.intVal + constVal));
        else
            push(Value::makeDouble(v.toDouble() + (double)constVal));
        DISPATCH();
    }
    TARGET(LOAD_STORE): {
        // LOAD_LOCAL(a) + STORE_LOCAL(b) — direct copy, no stack
        uint16_t src = readU16(), dst = readU16();
        uint16_t base = callStack_.back().basePointer;
        locals_[base + dst] = locals_[base + src];
        DISPATCH();
    }

    TARGET(LINE): { currentLine_ = readU16(); currentColumn_ = readU16(); DISPATCH(); }
    TARGET(PRINT): { Value val = pop(); builtinPrint(val); DISPATCH(); }

    // All complex opcodes fall through to the switch-based handler
    TARGET(ADD_F): TARGET(SUB_F): TARGET(MUL_F): TARGET(DIV_F): TARGET(NEG_F):
    TARGET(BIT_AND): TARGET(BIT_OR): TARGET(BIT_XOR): TARGET(BIT_NOT):
    TARGET(SHL): TARGET(SHR): TARGET(USHR):
    TARGET(CMP_EQ): TARGET(CMP_NE):
    TARGET(RETURN): TARGET(RETURN_VOID):
    TARGET(CALL): TARGET(CALL_VIRTUAL): TARGET(CALL_ASYNC):
    TARGET(NEW_OBJ): TARGET(GET_FIELD): TARGET(SET_FIELD):
    TARGET(NEW_ARRAY): TARGET(GET_ELEM): TARGET(SET_ELEM):
    TARGET(NEW_MAP): TARGET(ARRAY_LEN):
    TARGET(CONCAT):
    TARGET(CAST): TARGET(IS_TYPE): TARGET(IS_NULL):
    TARGET(AWAIT): TARGET(YIELD):
    TARGET(TRY_BEGIN): TARGET(TRY_END): TARGET(THROW):
    TARGET(CLOSURE): TARGET(LOAD_CAPTURE):
    {
        // Fallback to switch-based dispatch for complex opcodes
        auto op = static_cast<OpCode>(module_->code[pc_ - 1]);
        executeInstruction(op);
        if (!running_) return;
    }
    dispatch_end:
    DISPATCH();

    #undef DISPATCH
    #undef TARGET
#else
    // Fallback for non-GCC compilers: use original switch dispatch
    while (running_ && pc_ < module_->code.size()) {
        auto op = static_cast<bytecode::OpCode>(module_->code[pc_++]);
        executeInstruction(op);
    }
#endif
}

void VM::executeInstruction(bytecode::OpCode op) {
    using namespace bytecode;

    switch (op) {
        case OpCode::NOP: break;

        // --- Stack ---
        case OpCode::POP: pop(); break;
        case OpCode::DUP: push(peek()); break;
        case OpCode::SWAP: {
            Value a = pop();
            Value b = pop();
            push(a);
            push(b);
            break;
        }

        // --- Constants ---
        case OpCode::CONST_NULL: push(Value::makeNull()); break;
        case OpCode::CONST_TRUE: push(Value::makeBool(true)); break;
        case OpCode::CONST_FALSE: push(Value::makeBool(false)); break;
        case OpCode::CONST_I32: push(Value::makeInt(readI32())); break;
        case OpCode::CONST_I64: push(Value::makeLong(readI64())); break;
        case OpCode::CONST_F32: push(Value::makeFloat(readF32())); break;
        case OpCode::CONST_F64: push(Value::makeDouble(readF64())); break;
        case OpCode::CONST_STR: {
            uint16_t idx = readU16();
            push(Value::makeString(getConstantString(idx)));
            break;
        }

        // --- Locals ---
        case OpCode::LOAD_LOCAL: {
            uint16_t idx = readU16();
            uint16_t base = callStack_.empty() ? 0 : callStack_.back().basePointer;
            size_t addr = base + idx;
            if (addr >= locals_.size()) locals_.resize(addr + 64);
            push(locals_[addr]);
            break;
        }
        case OpCode::STORE_LOCAL: {
            uint16_t idx = readU16();
            uint16_t base = callStack_.empty() ? 0 : callStack_.back().basePointer;
            size_t addr = base + idx;
            if (addr >= locals_.size()) locals_.resize(addr + 64);
            locals_[addr] = pop();
            break;
        }

        // --- Arithmetic (integer) ---
        case OpCode::ADD_I: {
            Value b = pop(), a = pop();
            // Runtime type check: if either is a string, concatenate
            if (a.type == ValueType::String || b.type == ValueType::String) {
                push(Value::makeString(a.toString() + b.toString()));
            }
            // Object operator overloading: call __add on left operand
            else if (a.type == ValueType::Object && a.objVal) {
                std::string opMethod = a.objVal->className + ".__add";
                bool called = false;
                { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) {
                    // Fallback: try "add" method
                    opMethod = a.objVal->className + ".add";
                    { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                }
                if (!called) {
                    throwError("GardOperatorError", "Operator '+' is not defined for class '" + a.objVal->className + "'. Define '__add' method to support this operation");
                    return;
                }
            } else {
                // Fast path: both Int → Int arithmetic
                if (a.type == ValueType::Int && b.type == ValueType::Int) {
                    push(Value::makeInt(a.intVal + b.intVal));
                } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
                    int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.toInt();
                    int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.toInt();
                    push(Value::makeLong(av + bv));
                } else if (a.type == ValueType::Double || b.type == ValueType::Double ||
                           a.type == ValueType::Float || b.type == ValueType::Float) {
                    push(Value::makeDouble(a.toDouble() + b.toDouble()));
                } else {
                    push(Value::makeInt(a.toInt() + b.toInt()));
                }
            }
            break;
        }
        case OpCode::SUB_I: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string opMethod = a.objVal->className + ".__sub";
                bool called = false;
                { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) { opMethod = a.objVal->className + ".sub";
                    { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                }
                if (!called) {
                    throwError("GardOperatorError", "Operator '-' is not defined for class '" + a.objVal->className + "'. Define '__sub' method to support this operation");
                    return;
                }
            } else {
                if (a.type == ValueType::Int && b.type == ValueType::Int) {
                    push(Value::makeInt(a.intVal - b.intVal));
                } else if (a.type == ValueType::Double || b.type == ValueType::Double ||
                    a.type == ValueType::Float || b.type == ValueType::Float) {
                    push(Value::makeDouble(a.toDouble() - b.toDouble()));
                } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
                    int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
                    int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
                    push(Value::makeLong(av - bv));
                } else {
                    push(Value::makeInt(a.toInt() - b.toInt()));
                }
            }
            break;
        }
        case OpCode::MUL_I: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string opMethod = a.objVal->className + ".__mul";
                bool called = false;
                { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) { opMethod = a.objVal->className + ".mul";
                    { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                }
                if (!called) {
                    throwError("GardOperatorError", "Operator '*' is not defined for class '" + a.objVal->className + "'. Define '__mul' method to support this operation");
                    return;
                }
            } else {
                if (a.type == ValueType::Int && b.type == ValueType::Int) {
                    push(Value::makeInt(a.intVal * b.intVal));
                } else if (a.type == ValueType::Double || b.type == ValueType::Double ||
                    a.type == ValueType::Float || b.type == ValueType::Float) {
                    push(Value::makeDouble(a.toDouble() * b.toDouble()));
                } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
                    int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
                    int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
                    push(Value::makeLong(av * bv));
                } else {
                    push(Value::makeInt(a.toInt() * b.toInt()));
                }
            }
            break;
        }
        case OpCode::DIV_I: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string opMethod = a.objVal->className + ".__div";
                bool called = false;
                { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) { throwError("GardOperatorError", "Operator '/' is not defined for class '" + a.objVal->className + "'. Define '__div' method"); return; }
            } else {
                if (a.type == ValueType::Int && b.type == ValueType::Int) {
                    if (b.intVal == 0) { throwError("GardArithmeticError", "Division by zero"); return; }
                    push(Value::makeInt(a.intVal / b.intVal));
                } else if (a.type == ValueType::Double || b.type == ValueType::Double ||
                    a.type == ValueType::Float || b.type == ValueType::Float) {
                    if (b.toDouble() == 0.0) { throwError("GardArithmeticError", "Division by zero"); return; }
                    push(Value::makeDouble(a.toDouble() / b.toDouble()));
                } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
                    int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
                    if (bv == 0) { throwError("GardArithmeticError", "Division by zero"); return; }
                    int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
                    push(Value::makeLong(av / bv));
                } else {
                    if (b.toInt() == 0) { throwError("GardArithmeticError", "Division by zero"); return; }
                    push(Value::makeInt(a.toInt() / b.toInt()));
                }
            }
            break;
        }
        case OpCode::MOD_I: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string opMethod = a.objVal->className + ".__mod";
                bool called = false;
                { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) { throwError("GardOperatorError", "Operator '%%' is not defined for class '" + a.objVal->className + "'. Define '__mod' method"); return; }
            } else {
                if (a.type == ValueType::Int && b.type == ValueType::Int) {
                    if (b.intVal == 0) { throwError("GardArithmeticError", "Modulo by zero"); return; }
                    push(Value::makeInt(a.intVal % b.intVal));
                } else if (a.type == ValueType::Long || b.type == ValueType::Long) {
                    int64_t bv = b.type == ValueType::Long ? b.longVal : (int64_t)b.intVal;
                    if (bv == 0) { throwError("GardArithmeticError", "Modulo by zero"); return; }
                    int64_t av = a.type == ValueType::Long ? a.longVal : (int64_t)a.intVal;
                    push(Value::makeLong(av % bv));
                } else {
                    if (b.toInt() == 0) { throwError("GardArithmeticError", "Modulo by zero"); return; }
                    push(Value::makeInt(a.toInt() % b.toInt()));
                }
            }
            break;
        }
        case OpCode::NEG_I: {
            Value a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string opMethod = a.objVal->className + ".__neg";
                bool called = false;
                { auto _fit = funcLookup_.find(opMethod); if (_fit != funcLookup_.end()) { push(a); pushFrame(_fit->second, 1); called = true; } }
                if (!called) push(Value::makeInt(-a.toInt()));
            } else {
                push(Value::makeInt(-a.toInt()));
            }
            break;
        }

        // --- Arithmetic (float) ---
        case OpCode::ADD_F: { Value b = pop(), a = pop(); push(Value::makeDouble(a.doubleVal + b.doubleVal)); break; }
        case OpCode::SUB_F: { Value b = pop(), a = pop(); push(Value::makeDouble(a.doubleVal - b.doubleVal)); break; }
        case OpCode::MUL_F: { Value b = pop(), a = pop(); push(Value::makeDouble(a.doubleVal * b.doubleVal)); break; }
        case OpCode::DIV_F: { Value b = pop(), a = pop(); push(Value::makeDouble(a.doubleVal / b.doubleVal)); break; }
        case OpCode::NEG_F: { Value a = pop(); push(Value::makeDouble(-a.doubleVal)); break; }

        // --- Bitwise ---
        case OpCode::BIT_AND: { Value b = pop(), a = pop(); push(Value::makeInt(a.toInt() & b.toInt())); break; }
        case OpCode::BIT_OR:  { Value b = pop(), a = pop(); push(Value::makeInt(a.toInt() | b.toInt())); break; }
        case OpCode::BIT_XOR: { Value b = pop(), a = pop(); push(Value::makeInt(a.toInt() ^ b.toInt())); break; }
        case OpCode::BIT_NOT: { Value a = pop(); push(Value::makeInt(~a.toInt())); break; }
        case OpCode::SHL:     { Value b = pop(), a = pop(); push(Value::makeInt(a.toInt() << b.toInt())); break; }
        case OpCode::SHR:     { Value b = pop(), a = pop(); push(Value::makeInt(a.toInt() >> b.toInt())); break; }
        case OpCode::USHR:    { Value b = pop(), a = pop(); push(Value::makeInt(static_cast<int32_t>(static_cast<uint32_t>(a.toInt()) >> b.toInt()))); break; }

        // --- Comparison ---
        case OpCode::CMP_EQ: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string op = a.objVal->className + ".__eq";
                bool called = false;
                { auto _fit = funcLookup_.find(op); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) {
                    // Enum comparison: compare by _ordinal and _typeName
                    auto ordA = a.objVal->fields.find("_ordinal");
                    if (ordA != a.objVal->fields.end()) {
                        if (b.type == ValueType::Object && b.objVal) {
                            auto ordB = b.objVal->fields.find("_ordinal");
                            if (ordB != b.objVal->fields.end()) {
                                // Both are enum variants — compare ordinal + type
                                bool eq = (ordA->second.toInt() == ordB->second.toInt()) &&
                                          (a.objVal->className == b.objVal->className);
                                push(Value::makeBool(eq));
                            } else {
                                push(Value::makeBool(false));
                            }
                        } else if (b.type == ValueType::String) {
                            // Compare enum name to string (for match/switch with string patterns)
                            auto nameA = a.objVal->fields.find("_name");
                            push(Value::makeBool(nameA != a.objVal->fields.end() && nameA->second.toString() == b.toString()));
                        } else if (b.type == ValueType::Int) {
                            // Compare enum ordinal to int
                            push(Value::makeBool(ordA->second.toInt() == b.toInt()));
                        } else {
                            push(Value::makeBool(false));
                        }
                    } else {
                        push(Value::makeBool(a.toInt() == b.toInt()));
                    }
                }
            } else if (a.type == ValueType::String || b.type == ValueType::String) {
                push(Value::makeBool(a.toString() == b.toString()));
            } else { push(Value::makeBool(a.toInt() == b.toInt())); }
            break;
        }
        case OpCode::CMP_NE: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string op = a.objVal->className + ".__ne";
                bool called = false;
                { auto _fit = funcLookup_.find(op); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) {
                    // Enum comparison
                    auto ordA = a.objVal->fields.find("_ordinal");
                    if (ordA != a.objVal->fields.end()) {
                        if (b.type == ValueType::Object && b.objVal) {
                            auto ordB = b.objVal->fields.find("_ordinal");
                            if (ordB != b.objVal->fields.end()) {
                                bool ne = (ordA->second.toInt() != ordB->second.toInt()) ||
                                          (a.objVal->className != b.objVal->className);
                                push(Value::makeBool(ne));
                            } else {
                                push(Value::makeBool(true));
                            }
                        } else if (b.type == ValueType::String) {
                            auto nameA = a.objVal->fields.find("_name");
                            push(Value::makeBool(nameA == a.objVal->fields.end() || nameA->second.toString() != b.toString()));
                        } else {
                            push(Value::makeBool(ordA->second.toInt() != b.toInt()));
                        }
                    } else {
                        push(Value::makeBool(a.toInt() != b.toInt()));
                    }
                }
            } else if (a.type == ValueType::String || b.type == ValueType::String) {
                push(Value::makeBool(a.toString() != b.toString()));
            } else { push(Value::makeBool(a.toInt() != b.toInt())); }
            break;
        }
        case OpCode::CMP_LT: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string op = a.objVal->className + ".__lt";
                bool called = false;
                { auto _fit = funcLookup_.find(op); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) push(Value::makeBool(a.toDouble() < b.toDouble()));
            } else { push(Value::makeBool(a.toDouble() < b.toDouble())); }
            break;
        }
        case OpCode::CMP_GT: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string op = a.objVal->className + ".__gt";
                bool called = false;
                { auto _fit = funcLookup_.find(op); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) push(Value::makeBool(a.toDouble() > b.toDouble()));
            } else { push(Value::makeBool(a.toDouble() > b.toDouble())); }
            break;
        }
        case OpCode::CMP_LE: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string op = a.objVal->className + ".__le";
                bool called = false;
                { auto _fit = funcLookup_.find(op); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) push(Value::makeBool(a.toDouble() <= b.toDouble()));
            } else { push(Value::makeBool(a.toDouble() <= b.toDouble())); }
            break;
        }
        case OpCode::CMP_GE: {
            Value b = pop(), a = pop();
            if (a.type == ValueType::Object && a.objVal) {
                std::string op = a.objVal->className + ".__ge";
                bool called = false;
                { auto _fit = funcLookup_.find(op); if (_fit != funcLookup_.end()) { push(a); push(b); pushFrame(_fit->second, 2); called = true; } }
                if (!called) push(Value::makeBool(a.toDouble() >= b.toDouble()));
            } else { push(Value::makeBool(a.toDouble() >= b.toDouble())); }
            break;
        }

        // --- Logical ---
        case OpCode::LOG_AND: { Value b = pop(), a = pop(); push(Value::makeBool(a.toBool() && b.toBool())); break; }
        case OpCode::LOG_OR:  { Value b = pop(), a = pop(); push(Value::makeBool(a.toBool() || b.toBool())); break; }
        case OpCode::LOG_NOT: { Value a = pop(); push(Value::makeBool(!a.toBool())); break; }

        // --- Control flow ---
        case OpCode::JUMP: {
            uint16_t target = readU16();
            pc_ = target;
            break;
        }
        case OpCode::JUMP_IF: {
            uint16_t target = readU16();
            if (pop().toBool()) pc_ = target;
            break;
        }
        case OpCode::JUMP_IFNOT: {
            uint16_t target = readU16();
            if (!pop().toBool()) pc_ = target;
            break;
        }
        case OpCode::RETURN: {
            Value retVal = pop();
            popFrame();
            if (callStack_.empty()) {
                running_ = false;
                exitCode_ = retVal.toInt();
            } else {
                push(retVal);
            }
            break;
        }
        case OpCode::RETURN_VOID: {
            popFrame();
            if (callStack_.empty()) {
                running_ = false;
            } else {
                // Push null as "return value" for void functions
                // so the caller's store has something to pop
                push(Value::makeNull());
            }
            break;
        }

        // --- Calls ---
        case OpCode::CALL: {
            uint16_t funcIdx = readU16();
            uint8_t argCount = readU8();
            std::string funcName = getConstantString(funcIdx);

            // Check native functions (exact match and dotted match)
            auto nit = natives_.find(funcName);
            if (__builtin_expect(nit != natives_.end(), 1)) {
                argsBuffer_.resize(argCount);
                for (int i = argCount - 1; i >= 0; i--) argsBuffer_[i] = pop();

                // If this native is marked async, dispatch to thread pool (fire-and-forget)
                if (__builtin_expect(asyncNatives_.count(funcName) > 0, 0)) {
                    // Async needs a copy (lambda captures by value for thread safety)
                    std::vector<Value> argsCopy(argsBuffer_.begin(), argsBuffer_.begin() + argCount);
                    auto future = std::make_shared<AsyncFuture>();
                    auto nativeFunc = nit->second;
                    ThreadPool::instance().submit([future, nativeFunc, argsCopy]() {
                        Value result = nativeFunc(argsCopy);
                        {
                            std::lock_guard<std::mutex> lock(future->mutex);
                            future->result = result;
                        }
                        future->resolved.store(true, std::memory_order_release);
                    });
                    Value futureVal = Value::makeObject("Future");
                    futureVal.objVal->fields["_futurePtr"] = Value::makeLong(
                        reinterpret_cast<int64_t>(future.get()));
                    futureVal.objVal->fields["resolved"] = Value::makeBool(false);
                    futureVal.objVal->fields["function"] = Value::makeString(funcName);
                    {
                        static std::mutex futuresMutex;
                        static std::vector<std::shared_ptr<AsyncFuture>> activeFutures;
                        std::lock_guard<std::mutex> lock(futuresMutex);
                        activeFutures.push_back(future);
                        activeFutures.erase(
                            std::remove_if(activeFutures.begin(), activeFutures.end(),
                                [](const std::shared_ptr<AsyncFuture>& f) {
                                    return f->resolved.load(std::memory_order_relaxed) && f.use_count() <= 1;
                                }),
                            activeFutures.end());
                    }
                    push(futureVal);
                    break;
                }

                // Synchronous native call
                Value result = nit->second(argsBuffer_);
                if (!running_ || hasException_) { hasException_ = false; break; }
                push(result);
                break;
            }

            // Find function in module (O(1) hash lookup)
            auto fit = funcLookup_.find(funcName);
            if (fit != funcLookup_.end()) {
                uint16_t targetIdx = fit->second;
                // Check if the target function is async — if so, fire-and-forget
                // (dispatch to thread pool, push Future, continue execution immediately)
                if (targetIdx < module_->functions.size() &&
                    module_->functions[targetIdx].isAsync) {
                    // Async function called without await: fire-and-forget
                    // Collect args from stack
                    std::vector<Value> args;
                    for (int i = 0; i < argCount; i++) args.push_back(pop());
                    std::reverse(args.begin(), args.end());

                    // Create a Future and dispatch execution to thread pool
                    auto future = std::make_shared<AsyncFuture>();

                    // For module-defined async functions, we execute them synchronously
                    // on the thread pool using a cloned VM context (simplified: just mark as resolved)
                    // In practice, the function body runs on the main thread when awaited.
                    // Fire-and-forget: push a Future that resolves to null (background execution)
                    ThreadPool::instance().submit([future]() {
                        // Module-defined async functions: resolve immediately with null
                        // (the real work happens when the function is awaited via CALL_ASYNC path)
                        {
                            std::lock_guard<std::mutex> lock(future->mutex);
                            future->result = Value::makeNull();
                        }
                        future->resolved.store(true, std::memory_order_release);
                    });

                    // Push Future onto stack (fire-and-forget: caller doesn't await)
                    Value futureVal = Value::makeObject("Future");
                    futureVal.objVal->fields["_futurePtr"] = Value::makeLong(
                        reinterpret_cast<int64_t>(future.get()));
                    futureVal.objVal->fields["resolved"] = Value::makeBool(false);
                    futureVal.objVal->fields["function"] = Value::makeString(funcName);

                    // Keep future alive
                    {
                        static std::mutex futuresMutex;
                        static std::vector<std::shared_ptr<AsyncFuture>> activeFutures;
                        std::lock_guard<std::mutex> lock(futuresMutex);
                        activeFutures.push_back(future);
                        activeFutures.erase(
                            std::remove_if(activeFutures.begin(), activeFutures.end(),
                                [](const std::shared_ptr<AsyncFuture>& f) {
                                    return f->resolved.load(std::memory_order_relaxed) && f.use_count() <= 1;
                                }),
                            activeFutures.end());
                    }

                    push(futureVal);
                } else {
                    // Synchronous function call
                    pushFrame(targetIdx, argCount);
                }
            } else {
                // Unknown function — pop args and push null
                for (int i = 0; i < argCount; i++) pop();
                push(Value::makeNull());
            }
            break;
        }
        case OpCode::CALL_ASYNC: {
            // CALL_ASYNC dispatches native calls to a thread pool and returns a Future.
            // Uses the same resolution logic as CALL_VIRTUAL to find the native function:
            // 1. Exact match by name
            // 2. Class-qualified match from receiver type on stack
            // 3. Suffix match (fallback)
            uint16_t funcIdx = readU16();
            uint8_t argCount = readU8();
            std::string funcName = getConstantString(funcIdx);

            // Resolve the native function using CALL_VIRTUAL-style dispatch
            NativeFunc resolvedNative = nullptr;
            std::string resolvedName = funcName;

            // 1. Try exact match
            auto nit = natives_.find(funcName);
            if (nit != natives_.end()) {
                resolvedNative = nit->second;
            }

            // 2. Try class-qualified match from receiver on stack
            if (!resolvedNative && argCount > 0 && stack_.size() >= argCount) {
                Value& receiver = stack_[stack_.size() - argCount];
                std::string qualifiedName;
                if (receiver.type == ValueType::Object && receiver.objVal) {
                    qualifiedName = receiver.objVal->className + "." + funcName;
                } else if (receiver.type == ValueType::Array) {
                    qualifiedName = "List." + funcName;
                } else if (receiver.type == ValueType::String) {
                    qualifiedName = "String." + funcName;
                } else if (receiver.type == ValueType::Map) {
                    qualifiedName = "Map." + funcName;
                }
                if (!qualifiedName.empty()) {
                    auto qit = natives_.find(qualifiedName);
                    if (qit != natives_.end()) {
                        resolvedNative = qit->second;
                        resolvedName = qualifiedName;
                    }
                }
            }

            // 3. Suffix match fallback
            if (!resolvedNative) {
                std::string suffix = "." + funcName;
                for (auto& [name, func] : natives_) {
                    if (name.size() > suffix.size() &&
                        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        resolvedNative = func;
                        resolvedName = name;
                        break;
                    }
                }
            }

            if (resolvedNative) {
                // Collect arguments
                std::vector<Value> args;
                for (int i = 0; i < argCount; i++) args.push_back(pop());
                std::reverse(args.begin(), args.end());

                // Strip namespace/receiver prefix if first arg matches the namespace part of the function name
                // e.g., resolvedName="File.readText", args[0]="File" → strip "File" from args
                if (!args.empty() && resolvedName.find('.') != std::string::npos) {
                    std::string prefix = resolvedName.substr(0, resolvedName.find('.'));
                    if (args[0].type == ValueType::Null) {
                        // Null receiver (static call) — strip it
                        args.erase(args.begin());
                    } else if (args[0].type == ValueType::String && args[0].toString() == prefix) {
                        // String namespace prefix — strip it
                        args.erase(args.begin());
                    }
                }

                // Create a shared Future object
                auto future = std::make_shared<AsyncFuture>();

                // Dispatch the native call to the thread pool
                auto nativeFunc = resolvedNative;
                ThreadPool::instance().submit([future, nativeFunc, args]() {
                    Value result = nativeFunc(args);
                    {
                        std::lock_guard<std::mutex> lock(future->mutex);
                        future->result = result;
                    }
                    future->resolved.store(true, std::memory_order_release);
                });

                // Push a Future object onto the stack
                Value futureVal = Value::makeObject("Future");
                futureVal.objVal->fields["_futurePtr"] = Value::makeLong(
                    reinterpret_cast<int64_t>(future.get()));
                futureVal.objVal->fields["resolved"] = Value::makeBool(false);
                futureVal.objVal->fields["function"] = Value::makeString(resolvedName);

                // Keep future alive until resolved
                {
                    static std::mutex futuresMutex;
                    static std::vector<std::shared_ptr<AsyncFuture>> activeFutures;
                    std::lock_guard<std::mutex> lock(futuresMutex);
                    activeFutures.push_back(future);
                    activeFutures.erase(
                        std::remove_if(activeFutures.begin(), activeFutures.end(),
                            [](const std::shared_ptr<AsyncFuture>& f) {
                                return f->resolved.load(std::memory_order_relaxed) && f.use_count() <= 1;
                            }),
                        activeFutures.end());
                }

                push(futureVal);
                break;
            }

            // For module-defined functions, execute synchronously (same as CALL)
            auto fit = funcLookup_.find(funcName);
            if (fit != funcLookup_.end()) {
                pushFrame(fit->second, argCount);
            } else {
                for (int i = 0; i < argCount; i++) pop();
                push(Value::makeNull());
            }
            break;
        }
        case OpCode::CALL_VIRTUAL: {
            uint32_t callSitePC = pc_ - 1; // PC of this CALL_VIRTUAL instruction
            uint16_t methodIdx = readU16();
            uint8_t argCount = readU8();

            // === INLINE CACHE: fast path ===
            // Check if we have a cached resolution for this call site
            auto cacheIt = inlineCache_.find(callSitePC);
            if (cacheIt != inlineCache_.end() && cacheIt->second.valid) {
                auto& cache = cacheIt->second;
                // Verify receiver type matches (monomorphic check)
                bool typeMatch = false;
                if (argCount > 0 && stack_.size() >= argCount) {
                    Value& receiver = stack_[stack_.size() - argCount];
                    if (receiver.type == ValueType::Object && receiver.objVal) {
                        typeMatch = (receiver.objVal->className == cache.cachedClassName);
                    } else if (receiver.type == ValueType::Array) {
                        typeMatch = (cache.cachedClassName == "List");
                    } else if (receiver.type == ValueType::String) {
                        typeMatch = (cache.cachedClassName == "String");
                    } else if (receiver.type == ValueType::Map) {
                        typeMatch = (cache.cachedClassName == "Map");
                    } else if (receiver.type == ValueType::Null) {
                        typeMatch = (cache.cachedClassName == "_null");
                    }
                } else {
                    typeMatch = (cache.cachedClassName == "_static");
                }

                if (typeMatch) {
                    // Cache hit — direct dispatch (no string building, no hash lookup)
                    if (cache.isNative) {
                        std::vector<Value> args;
                        for (uint8_t i = 0; i < argCount; i++) args.push_back(pop());
                        std::reverse(args.begin(), args.end());
                        Value r = cache.cachedNative(args);
                        if (!running_ || hasException_) { hasException_ = false; break; }
                        push(r);
                    } else {
                        pushFrame(cache.cachedFuncIdx, argCount);
                    }
                    break;
                }
                // Type mismatch — invalidate cache, fall through to slow path
                cache.valid = false;
            }

            // === SLOW PATH: full resolution (same as before) ===
            std::string methodName = getConstantString(methodIdx);
            bool found = false;

            // Determine receiver type for cache population
            std::string receiverClassName;
            if (argCount > 0 && stack_.size() >= argCount) {
                Value& objVal = stack_[stack_.size() - argCount];
                if (objVal.type == ValueType::Object && objVal.objVal) {
                    receiverClassName = objVal.objVal->className;
                } else if (objVal.type == ValueType::Array) {
                    receiverClassName = "List";
                } else if (objVal.type == ValueType::String) {
                    receiverClassName = "String";
                } else if (objVal.type == ValueType::Map) {
                    receiverClassName = "Map";
                } else if (objVal.type == ValueType::Null) {
                    receiverClassName = "_null";
                }
            } else {
                receiverClassName = "_static";
            }

            // Try to determine object class from stack (first arg is the object)
            std::string preferredName;
            if (argCount > 0 && stack_.size() >= argCount) {
                Value& objVal = stack_[stack_.size() - argCount];
                if (objVal.type == ValueType::Object && objVal.objVal) {
                    preferredName = objVal.objVal->className + "." + methodName;
                }
            }

            // Try preferred (class-qualified) name first, including arity-mangled overloads
            if (!preferredName.empty()) {
                // Try exact match (O(1) hash lookup)
                auto fit = funcLookup_.find(preferredName);
                if (fit != funcLookup_.end()) {
                    pushFrame(fit->second, argCount);
                    found = true;
                    // Populate inline cache
                    auto& cache = inlineCache_[callSitePC];
                    cache.cachedClassName = receiverClassName;
                    cache.cachedFuncIdx = fit->second;
                    cache.isNative = false;
                    cache.valid = true;
                }
                // Try arity-mangled: ClassName.method$N (N = argCount - 1 for 'this')
                if (!found) {
                    std::string arityName = preferredName + "$" + std::to_string(argCount - 1);
                    auto ait = funcLookup_.find(arityName);
                    if (ait != funcLookup_.end()) {
                        pushFrame(ait->second, argCount);
                        found = true;
                        auto& cache = inlineCache_[callSitePC];
                        cache.cachedClassName = receiverClassName;
                        cache.cachedFuncIdx = ait->second;
                        cache.isNative = false;
                        cache.valid = true;
                    }
                }
            }

            // Fall back to exact match then suffix match
            if (!found) {
                // Try exact match first (O(1))
                auto fit = funcLookup_.find(methodName);
                if (fit != funcLookup_.end()) {
                    pushFrame(fit->second, argCount);
                    found = true;
                } else {
                    // Suffix match: find any function ending in ".methodName" (still O(n) but rare path)
                    std::string suffix = "." + methodName;
                    for (uint16_t i = 0; i < module_->functions.size(); i++) {
                        const auto& fnName = module_->functions[i].name;
                        if (fnName.size() > suffix.size() &&
                            fnName[fnName.size() - suffix.size() - 1 + 1] == '.' &&
                            fnName.compare(fnName.size() - methodName.size(), methodName.size(), methodName) == 0 &&
                            fnName[fnName.size() - methodName.size() - 1] == '.') {
                            if (!callStack_.empty() && callStack_.back().functionIndex == i) continue;
                            pushFrame(i, argCount);
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (!found) {
                // Proper type-based dispatch:
                // 1. Try exact native match first (for qualified names like "http.get")
                // 2. Determine the object's type from the stack
                // 3. If object has a type → call TypeName.method
                // 4. If object is null (static call) → find the unique native "X.method" that exists

                // Try exact native match with the full method name (handles http.get, Math.sqrt, etc.)
                auto exactNit = natives_.find(methodName);
                if (exactNit != natives_.end()) {
                    std::vector<Value> args;
                    for (uint8_t i = 0; i < argCount; i++) args.push_back(pop());
                    std::reverse(args.begin(), args.end());
                    // Strip null this if first arg is null (namespace call)
                    // Also strip string namespace prefix (e.g., "Color" in Color.valueOf("BLUE"))
                    if (!args.empty() && args[0].type == ValueType::Null) {
                        std::vector<Value> callArgs(args.begin() + 1, args.end());
                        Value r = exactNit->second(callArgs);
                        if (!running_ || hasException_) { hasException_ = false; break; }
                        push(r);
                    } else if (!args.empty() && args[0].type == ValueType::String) {
                        // Check if first arg is the namespace prefix of the method name
                        std::string firstStr = args[0].toString();
                        if (methodName.size() > firstStr.size() &&
                            methodName.compare(0, firstStr.size(), firstStr) == 0 &&
                            methodName[firstStr.size()] == '.') {
                            // Strip namespace string
                            std::vector<Value> callArgs(args.begin() + 1, args.end());
                            Value r = exactNit->second(callArgs);
                            if (!running_ || hasException_) { hasException_ = false; break; }
                            push(r);
                        } else {
                            Value r = exactNit->second(args);
                            if (!running_ || hasException_) { hasException_ = false; break; }
                            push(r);
                        }
                    } else {
                        Value r = exactNit->second(args);
                        if (!running_ || hasException_) { hasException_ = false; break; }
                        push(r);
                    }
                    found = true;
                }
            }
            if (!found) {

                std::vector<Value> args;
                for (uint8_t i = 0; i < argCount; i++) args.push_back(pop());
                std::reverse(args.begin(), args.end());

                // Determine type of the receiver (first arg)
                std::string resolvedClass;
                if (!args.empty()) {
                    switch (args[0].type) {
                        case ValueType::Array:  resolvedClass = "List"; break;
                        case ValueType::String: {
                            // Check if this string is a namespace/enum name (e.g., "Color" from Color.valueOf)
                            std::string possibleNs = args[0].toString() + "." + methodName;
                            auto nsIt = natives_.find(possibleNs);
                            if (nsIt != natives_.end()) {
                                // It's a namespace call — strip the namespace string and call with remaining args
                                std::vector<Value> callArgs(args.begin() + 1, args.end());
                                Value r = nsIt->second(callArgs);
                                if (!running_ || hasException_) { hasException_ = false; found = true; }
                                else { push(r); found = true; }
                            } else {
                                resolvedClass = "String";
                            }
                            break;
                        }
                        case ValueType::Map:    resolvedClass = "Map"; break;
                        case ValueType::Object:
                            if (args[0].objVal) resolvedClass = args[0].objVal->className;
                            break;
                        case ValueType::Null: {
                            // Static call: find the native by exact "Prefix.method" lookup
                            // Try each registered native that ends with ".methodName"
                            std::string suffix = "." + methodName;
                            for (auto& [name, func] : natives_) {
                                if (name.size() > suffix.size() &&
                                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                                    // Found a match — use it (strip null this)
                                    std::vector<Value> callArgs(args.begin() + 1, args.end());
                                    Value r = func(callArgs);
                                    if (!running_ || hasException_) { hasException_ = false; found = true; break; }
                                    push(r);
                                    found = true;
                                    break;
                                }
                            }
                            break;
                        }
                        default: break;
                    }
                }

                // If we resolved a class, look up ClassName.method
                if (!found && !resolvedClass.empty()) {
                    std::string qualified = resolvedClass + "." + methodName;
                    auto nit = natives_.find(qualified);
                    if (nit != natives_.end()) {
                        // Check if this native is async — fire-and-forget dispatch
                        if (asyncNatives_.count(qualified)) {
                            auto future = std::make_shared<AsyncFuture>();
                            auto nativeFunc = nit->second;
                            ThreadPool::instance().submit([future, nativeFunc, args]() {
                                Value result = nativeFunc(args);
                                { std::lock_guard<std::mutex> lock(future->mutex); future->result = result; }
                                future->resolved.store(true, std::memory_order_release);
                            });
                            Value futureVal = Value::makeObject("Future");
                            futureVal.objVal->fields["_futurePtr"] = Value::makeLong(reinterpret_cast<int64_t>(future.get()));
                            futureVal.objVal->fields["resolved"] = Value::makeBool(false);
                            futureVal.objVal->fields["function"] = Value::makeString(qualified);
                            { static std::mutex fm; static std::vector<std::shared_ptr<AsyncFuture>> af;
                              std::lock_guard<std::mutex> lock(fm); af.push_back(future);
                              af.erase(std::remove_if(af.begin(), af.end(), [](const std::shared_ptr<AsyncFuture>& f) {
                                  return f->resolved.load(std::memory_order_relaxed) && f.use_count() <= 1; }), af.end()); }
                            push(futureVal); found = true;
                        } else {
                            Value r = nit->second(args);
                            if (!running_ || hasException_) { hasException_ = false; found = true; }
                            else {
                                push(r); found = true;
                                // Populate inline cache for native
                                auto& cache = inlineCache_[callSitePC];
                                cache.cachedClassName = receiverClassName;
                                cache.cachedNative = nit->second;
                                cache.isNative = true;
                                cache.valid = true;
                            }
                        }
                    }
                }

                // ORM instance methods: route save/delete to ORM natives (before generic suffix match)
                if (!found && !args.empty() && args[0].type == ValueType::Object && args[0].objVal) {
                    std::string ormMethod;
                    if (methodName == "save") ormMethod = "ORM.instanceSave";
                    else if (methodName == "delete") ormMethod = "ORM.instanceDelete";

                    if (!ormMethod.empty()) {
                        auto ormIt = natives_.find(ormMethod);
                        if (ormIt != natives_.end()) {
                            Value r = ormIt->second(args);
                            if (!running_ || hasException_) { hasException_ = false; found = true; }
                            else { push(r); found = true; }
                        }
                    }
                }

                // Final fallback: try all prefixes with the full args
                if (!found) {
                    std::string suffix = "." + methodName;
                    for (auto& [name, func] : natives_) {
                        if (name.size() > suffix.size() &&
                            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            Value r = func(args);
                            if (!running_ || hasException_) { hasException_ = false; found = true; break; }
                            push(r);
                            found = true;
                            break;
                        }
                    }
                }

                if (!found) {
                    push(Value::makeNull());
                }
            }
            break;
        }

        // --- Objects ---
        case OpCode::NEW_OBJ: {
            uint16_t classIdx = readU16();
            std::string className = getConstantString(classIdx);
            push(createObject(className));
            break;
        }
        case OpCode::GET_FIELD: {
            uint16_t fieldIdx = readU16();
            std::string fieldName = getConstantString(fieldIdx);
            Value obj = pop();
            Value result = getField(obj, fieldName);
            // If field not found and object is a string (namespace/class name like "Color"),
            // try to find a registered native "ClassName.fieldName"
            if (result.type == ValueType::Null) {
                std::string qualifiedName;
                if (obj.type == ValueType::String && obj.strVal && !obj.strVal->data.empty()) {
                    qualifiedName = obj.strVal->data + "." + fieldName;
                } else if (obj.type == ValueType::Null) {
                    // Fallback: try suffix match
                    std::string suffix = "." + fieldName;
                    for (auto& [name, func] : natives_) {
                        if (name.size() > suffix.size() &&
                            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            result = func({});
                            if (!running_ || hasException_) { hasException_ = false; break; }
                            break;
                        }
                    }
                }
                if (!qualifiedName.empty()) {
                    auto nit = natives_.find(qualifiedName);
                    if (nit != natives_.end()) {
                        result = nit->second({});
                        if (!running_ || hasException_) { hasException_ = false; }
                    }
                }
            }
            push(result);
            break;
        }
        case OpCode::SET_FIELD: {
            uint16_t fieldIdx = readU16();
            std::string fieldName = getConstantString(fieldIdx);
            Value val = pop();
            Value obj = pop();
            if (obj.type == ValueType::Null) {
                throwError("GardNullPointerError", "Cannot set field '" + fieldName + "' on null");
                return;
            }
            setField(obj, fieldName, val);
            break;
        }
        case OpCode::NEW_ARRAY: {
            readU16(); // size hint
            push(Value::makeArray());
            trackAllocation(64);
            break;
        }
        case OpCode::GET_ELEM: {
            Value idx = pop();
            Value arr = pop();
            if (__builtin_expect(arr.type == ValueType::Array && arr.arrVal != nullptr, 1)) {
                int i = idx.intVal; // fast path: assume int (most common)
                if (idx.type != ValueType::Int) i = idx.toInt();
                auto& elems = arr.arrVal->elements;
                if (__builtin_expect((unsigned)i < elems.size(), 1)) {
                    push(elems[i]);
                } else {
                    throwError("GardIndexOutOfBoundsError", "Index " + std::to_string(i) + " out of bounds for array of size " + std::to_string(elems.size()));
                    return;
                }
            } else if (arr.type == ValueType::Map && arr.mapVal) {
                std::string key = idx.toString();
                auto it = arr.mapVal->entries.find(key);
                push(it != arr.mapVal->entries.end() ? it->second : Value::makeNull());
            } else {
                push(Value::makeNull());
            }
            break;
        }
        case OpCode::SET_ELEM: {
            Value val = pop();
            Value idx = pop();
            Value arr = pop();
            if (arr.type == ValueType::Array && arr.arrVal) {
                int i = idx.toInt();
                if (i >= 0) {
                    while (static_cast<int>(arr.arrVal->elements.size()) <= i) {
                        arr.arrVal->elements.push_back(Value::makeNull());
                    }
                    arr.arrVal->elements[i] = val;
                }
            } else if (arr.type == ValueType::Map && arr.mapVal) {
                std::string key = idx.toString();
                arr.mapVal->entries[key] = val;
            }
            break;
        }
        case OpCode::NEW_MAP: {
            push(Value::makeMap());
            trackAllocation(128);
            break;
        }

        // --- String ---
        case OpCode::CONCAT: {
            Value b = pop(), a = pop();
            push(Value::makeString(a.toString() + b.toString()));
            break;
        }

        // --- Type ---
        case OpCode::IS_NULL: {
            Value a = pop();
            push(Value::makeBool(a.type == ValueType::Null));
            break;
        }

        case OpCode::CAST: {
            readU8();
            Value typeName = pop();
            Value val = pop();
            std::string target = typeName.toString();

            if (target == "int") {
                switch (val.type) {
                    case ValueType::Int: push(val); break;
                    case ValueType::Long: push(Value::makeInt(static_cast<int32_t>(val.longVal))); break;
                    case ValueType::Float: push(Value::makeInt(static_cast<int32_t>(val.floatVal))); break;
                    case ValueType::Double: push(Value::makeInt(static_cast<int32_t>(val.doubleVal))); break;
                    case ValueType::Bool: push(Value::makeInt(val.boolVal ? 1 : 0)); break;
                    case ValueType::String: {
                        try { push(Value::makeInt(std::stoi(val.toString()))); }
                        catch (...) { throwError("GardIllegalCastError", "Cannot cast string '" + val.toString() + "' to int"); return; }
                        break;
                    }
                    default: throwError("GardIllegalCastError", "Cannot cast to int"); return;
                }
            } else if (target == "float") {
                switch (val.type) {
                    case ValueType::Int: push(Value::makeFloat(static_cast<float>(val.intVal))); break;
                    case ValueType::Long: push(Value::makeFloat(static_cast<float>(val.longVal))); break;
                    case ValueType::Float: push(val); break;
                    case ValueType::Double: push(Value::makeFloat(static_cast<float>(val.doubleVal))); break;
                    case ValueType::String: {
                        try { push(Value::makeFloat(std::stof(val.toString()))); }
                        catch (...) { throwError("GardIllegalCastError", "Cannot cast string '" + val.toString() + "' to float"); return; }
                        break;
                    }
                    default: throwError("GardIllegalCastError", "Cannot cast to float"); return;
                }
            } else if (target == "double") {
                switch (val.type) {
                    case ValueType::Int: push(Value::makeDouble(static_cast<double>(val.intVal))); break;
                    case ValueType::Long: push(Value::makeDouble(static_cast<double>(val.longVal))); break;
                    case ValueType::Float: push(Value::makeDouble(static_cast<double>(val.floatVal))); break;
                    case ValueType::Double: push(val); break;
                    case ValueType::String: {
                        try { push(Value::makeDouble(std::stod(val.toString()))); }
                        catch (...) { throwError("GardIllegalCastError", "Cannot cast string '" + val.toString() + "' to double"); return; }
                        break;
                    }
                    default: throwError("GardIllegalCastError", "Cannot cast to double"); return;
                }
            } else if (target == "long") {
                switch (val.type) {
                    case ValueType::Int: push(Value::makeLong(static_cast<int64_t>(val.intVal))); break;
                    case ValueType::Long: push(val); break;
                    case ValueType::Float: push(Value::makeLong(static_cast<int64_t>(val.floatVal))); break;
                    case ValueType::Double: push(Value::makeLong(static_cast<int64_t>(val.doubleVal))); break;
                    case ValueType::String: {
                        try { push(Value::makeLong(std::stoll(val.toString()))); }
                        catch (...) { throwError("GardIllegalCastError", "Cannot cast string '" + val.toString() + "' to long"); return; }
                        break;
                    }
                    default: throwError("GardIllegalCastError", "Cannot cast to long"); return;
                }
            } else if (target == "string") {
                push(Value::makeString(val.toString()));
            } else if (target == "boolean") {
                push(Value::makeBool(val.toBool()));
            } else if (target == "__addressOf") {
                // Address-of operator: &variable — creates a pointer object wrapping the value
                Value ptr = Value::makeObject("Pointer");
                ptr.objVal->fields["_value"] = val;
                ptr.objVal->fields["_type"] = Value::makeString(
                    val.type == ValueType::Int ? "int" :
                    val.type == ValueType::Long ? "long" :
                    val.type == ValueType::Double ? "double" :
                    val.type == ValueType::Float ? "float" :
                    val.type == ValueType::String ? "string" :
                    val.type == ValueType::Bool ? "bool" :
                    val.type == ValueType::Array ? "array" :
                    val.type == ValueType::Object ? "object" : "void");
                ptr.objVal->fields["_isPointer"] = Value::makeBool(true);
                ptr.objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(&val));
                push(ptr);
            } else if (target == "__deref") {
                // Dereference operator: *ptr — extracts value from pointer object
                if (val.type == ValueType::Object && val.objVal) {
                    auto isPtr = val.objVal->fields.find("_isPointer");
                    if (isPtr != val.objVal->fields.end() && isPtr->second.toBool()) {
                        auto valIt = val.objVal->fields.find("_value");
                        if (valIt != val.objVal->fields.end()) {
                            push(valIt->second);
                        } else {
                            push(Value::makeNull());
                        }
                    } else {
                        throwError("GardMemoryError", "Cannot dereference: value is not a pointer");
                        return;
                    }
                } else if (val.type == ValueType::Null) {
                    throwError("GardNullPointerError", "Cannot dereference null pointer");
                    return;
                } else {
                    throwError("GardMemoryError", "Cannot dereference non-pointer value");
                    return;
                }
            } else {
                throwError("GardIllegalCastError", "Unsupported cast target type '" + target + "'");
                return;
            }
            break;
        }

        // --- Type check (is) ---
        case OpCode::IS_TYPE: {
            readU16(); // type index (unused — we use string from stack)
            Value typeName = pop();
            Value val = pop();
            std::string target = typeName.toString();
            bool result = false;

            if (val.type == ValueType::Object && val.objVal) {
                std::string className = val.objVal->className;

                // 1. Direct class match
                if (className == target) {
                    result = true;
                }

                // 2. Check interface implementation (_implements annotation in bytecode)
                if (!result && module_) {
                    for (auto& ann : module_->annotations) {
                        if (ann.target == className && ann.name == "_implements") {
                            for (auto& [ifaceName, _] : ann.args) {
                                if (ifaceName == target) { result = true; break; }
                            }
                            if (result) break;
                        }
                    }
                }

                // 3. Check inheritance chain (_extends annotation)
                if (!result && module_) {
                    std::string current = className;
                    int depth = 0;
                    while (!result && !current.empty() && depth < 20) {
                        for (auto& ann : module_->annotations) {
                            if (ann.target == current && ann.name == "_extends") {
                                for (auto& [parentName, _] : ann.args) {
                                    if (parentName == target) { result = true; break; }
                                    current = parentName;
                                }
                                break;
                            }
                        }
                        if (!result) {
                            // Check if parent implements the interface
                            for (auto& ann : module_->annotations) {
                                if (ann.target == current && ann.name == "_implements") {
                                    for (auto& [ifaceName, _] : ann.args) {
                                        if (ifaceName == target) { result = true; break; }
                                    }
                                    break;
                                }
                            }
                        }
                        depth++;
                        if (result) break;
                        // Move up the chain
                        bool foundParent = false;
                        for (auto& ann : module_->annotations) {
                            if (ann.target == current && ann.name == "_extends") {
                                for (auto& [parentName, _] : ann.args) {
                                    current = parentName;
                                    foundParent = true;
                                    break;
                                }
                                break;
                            }
                        }
                        if (!foundParent) break;
                    }
                }
            } else if (val.type == ValueType::Null) {
                result = false; // null is not an instance of anything
            } else {
                // Primitive type checks
                if (target == "int" && val.type == ValueType::Int) result = true;
                else if (target == "long" && val.type == ValueType::Long) result = true;
                else if (target == "float" && val.type == ValueType::Float) result = true;
                else if (target == "double" && val.type == ValueType::Double) result = true;
                else if (target == "boolean" && val.type == ValueType::Bool) result = true;
                else if (target == "string" && val.type == ValueType::String) result = true;
                else if (target == "array" && val.type == ValueType::Array) result = true;
                else if (target == "map" && val.type == ValueType::Map) result = true;
            }

            push(Value::makeBool(result));
            break;
        }

        // --- Built-in ---
        case OpCode::PRINT: {
            Value val = pop();
            builtinPrint(val);
            break;
        }

        // --- Closure (simplified) ---
        case OpCode::CLOSURE: {
            readU16(); // func index
            readU8();  // capture count
            push(Value::makeNull()); // placeholder
            break;
        }
        case OpCode::LOAD_CAPTURE: {
            readU8(); // capture index
            push(Value::makeNull()); // placeholder
            break;
        }

        // --- Async ---
        case OpCode::AWAIT: {
            // AWAIT operates on the top of stack:
            // - If the value is a Future object (from CALL_ASYNC or async function call),
            //   wait until resolved or throw GardAsyncTimeoutError on timeout.
            // - If the value is already a concrete value (synchronous call), pass through.
            if (!stack_.empty()) {
                Value& top = stack_.back();
                if (top.type == ValueType::Object && top.objVal &&
                    top.objVal->className == "Future") {
                    // This is an async future — wait for resolution with timeout
                    auto futureIt = top.objVal->fields.find("_futurePtr");
                    if (futureIt != top.objVal->fields.end()) {
                        auto* future = reinterpret_cast<AsyncFuture*>(
                            static_cast<intptr_t>(futureIt->second.longVal));
                        if (future) {
                            // Determine timeout: check if Future has a custom timeout, else default 30s
                            int timeoutMs = 300000; // default 300 seconds
                            auto toIt = top.objVal->fields.find("_timeout");
                            if (toIt != top.objVal->fields.end() && toIt->second.toInt() > 0) {
                                timeoutMs = toIt->second.toInt();
                            }

                            // Wait with timeout
                            auto deadline = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(timeoutMs);
                            while (!future->resolved.load(std::memory_order_acquire)) {
                                if (std::chrono::steady_clock::now() >= deadline) {
                                    // Timeout — throw GardAsyncTimeoutError
                                    std::string funcName = "unknown";
                                    auto fnIt = top.objVal->fields.find("function");
                                    if (fnIt != top.objVal->fields.end()) {
                                        funcName = fnIt->second.toString();
                                    }
                                    stack_.pop_back(); // remove the Future from stack
                                    throwError("GardAsyncTimeoutError",
                                        "await timed out after " + std::to_string(timeoutMs) +
                                        "ms waiting for '" + funcName + "'");
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::microseconds(100));
                            }

                            if (!hasException_ && running_) {
                                // Replace the Future on the stack with the resolved value
                                std::lock_guard<std::mutex> lock(future->mutex);
                                stack_.back() = future->result;
                            }
                        }
                    }
                }
                // Otherwise: value is already resolved (synchronous call), pass through
            }
            break;
        }
        case OpCode::YIELD: {
            break;
        }

        // --- Exception handling ---
        case OpCode::TRY_BEGIN: {
            uint16_t catchPC = readU16();
            ExceptionHandler handler;
            handler.catchPC = catchPC;
            handler.stackSize = static_cast<uint16_t>(stack_.size());
            handler.callStackSize = static_cast<uint16_t>(callStack_.size());
            exceptionHandlers_.push_back(handler);
            break;
        }
        case OpCode::TRY_END: {
            if (!exceptionHandlers_.empty()) {
                exceptionHandlers_.pop_back();
            }
            currentException_ = Value(); // clear after try completes normally
            break;
        }
        case OpCode::THROW: {
            Value exception = pop();

            // Rethrow: if exception is null, re-throw the current caught exception (Dart-style rethrow)
            if (exception.type == ValueType::Null) {
                if (currentException_.type != ValueType::Null) {
                    exception = currentException_;
                } else {
                    throwError("GardError", "rethrow: no current exception to rethrow");
                    break;
                }
            }

            // Enrich exception with position info if it's an object
            if (exception.type == ValueType::Object && exception.objVal) {
                std::string functionName = callStack_.empty() ? "main" : callStack_.back().functionName;
                // Add position fields if not already set
                if (exception.objVal->fields.find("line") == exception.objVal->fields.end() ||
                    exception.objVal->fields["line"].toInt() == 0) {
                    exception.objVal->fields["line"] = Value::makeInt(currentLine_);
                    exception.objVal->fields["column"] = Value::makeInt(currentColumn_);
                    exception.objVal->fields["file"] = Value::makeString(sourceFile_);
                    exception.objVal->fields["caller"] = Value::makeString(functionName);
                }
                // Ensure type field exists
                if (exception.objVal->fields.find("type") == exception.objVal->fields.end()) {
                    exception.objVal->fields["type"] = Value::makeString(exception.objVal->className);
                }
                // Build stack trace
                if (exception.objVal->fields.find("stack") == exception.objVal->fields.end()) {
                    Value stackTrace = Value::makeArray();
                    stackTrace.arrVal->elements.push_back(Value::makeString(
                        (sourceFile_.empty() ? "<unknown>" : sourceFile_) + ":" +
                        std::to_string(currentLine_) + ":" + std::to_string(currentColumn_) +
                        " in " + functionName + "()"));
                    for (int i = (int)callStack_.size() - 1; i >= 0; i--) {
                        stackTrace.arrVal->elements.push_back(Value::makeString(
                            "  at " + callStack_[i].functionName + "()"));
                    }
                    exception.objVal->fields["stack"] = stackTrace;
                }
                // Build formatted string
                std::string errType = exception.objVal->className;
                std::string msg = "";
                auto msgIt = exception.objVal->fields.find("message");
                if (msgIt != exception.objVal->fields.end()) msg = msgIt->second.toString();
                else {
                    // Check if constructor arg was stored
                    for (auto& [k, v] : exception.objVal->fields) {
                        if (k != "type" && k != "line" && k != "column" && k != "file" &&
                            k != "caller" && k != "stack" && k[0] != '_' && v.type == ValueType::String) {
                            msg = v.toString(); break;
                        }
                    }
                }
                std::string formatted = errType + ": " + msg + "\n  at " +
                    (sourceFile_.empty() ? "<unknown>" : sourceFile_) + ":" +
                    std::to_string(currentLine_) + ":" + std::to_string(currentColumn_) +
                    " in " + functionName + "()";
                exception.objVal->fields["_formatted"] = Value::makeString(formatted);
            }

            if (exceptionHandlers_.empty()) {
                // Uncaught exception — halt with formatted output
                if (exception.type == ValueType::Object && exception.objVal) {
                    auto fmtIt = exception.objVal->fields.find("_formatted");
                    if (fmtIt != exception.objVal->fields.end()) {
                        std::cerr << "\033[31m" << fmtIt->second.toString() << "\033[0m" << std::endl;
                    } else {
                        std::cerr << "\033[31mUncaught exception\033[0m: " << exception.toString() << std::endl;
                    }
                } else {
                    std::cerr << "\033[31mUncaught exception\033[0m: " << exception.toString() << std::endl;
                }
                running_ = false;
                exitCode_ = 1;
            } else {
                // Unwind to nearest catch handler
                ExceptionHandler handler = exceptionHandlers_.back();
                exceptionHandlers_.pop_back();
                while (stack_.size() > handler.stackSize) stack_.pop_back();
                while (callStack_.size() > handler.callStackSize) callStack_.pop_back();
                currentException_ = exception;
                hasException_ = true;
                pc_ = handler.catchPC;
                push(exception);
            }
            break;
        }

        // --- Debug ---
        case OpCode::LINE: {
            currentLine_ = readU16();
            currentColumn_ = readU16();
            break;
        }

        // --- Halt ---
        case OpCode::HALT: {
            running_ = false;
            break;
        }

        default:
            std::cerr << "Runtime error: unknown opcode 0x"
                      << std::hex << static_cast<int>(op) << std::dec << std::endl;
            running_ = false;
            exitCode_ = 1;
            break;
    }
}

// --- Stack operations ---

void VM::push(const Value& val) {
    stack_.push_back(val);
}

Value VM::pop() {
    Value val = std::move(stack_.back());
    stack_.pop_back();
    return val;
}

Value& VM::peek() {
    return stack_.back();
}

Value& VM::peekAt(int offset) {
    return stack_[stack_.size() - 1 - offset];
}

// --- Call frame ---

void VM::pushFrame(uint16_t funcIdx, uint8_t argCount) {
    if (callStack_.size() >= config_.maxCallDepth) {
        std::cerr << "Runtime error: call stack overflow" << std::endl;
        running_ = false;
        exitCode_ = 1;
        return;
    }

    auto& fn = module_->functions[funcIdx];

    // JIT: record execution and check for compiled version
    if (jit_) {
        jit_->recordExecution(funcIdx);

        // Check if already JIT'd
        auto* jitFn = jit_->getCompiledFunction(funcIdx);
        if (jitFn) {
            // Call native code directly — pop args, call, push result
            std::vector<int64_t> args(argCount);
            for (int i = argCount - 1; i >= 0; i--) {
                args[i] = (int64_t)pop().toInt();
            }
            using JitFunc = int64_t(*)(int64_t*, int32_t);
            int64_t result = reinterpret_cast<JitFunc>(jitFn)(args.data(), argCount);
            push(Value::makeInt((int32_t)result));
            return; // skip interpreter — function executed natively
        }

        // Check if hot enough to compile
        if (jit_->shouldCompile(funcIdx)) {
            jit_->compile(funcIdx, *module_, *this);
        }
    }

    CallFrame frame;
    frame.functionIndex = funcIdx;
    frame.returnPC = pc_;
    frame.basePointer = static_cast<uint16_t>(callStack_.empty() ? 0 :
                        callStack_.back().basePointer + module_->functions[callStack_.back().functionIndex].localCount);
    frame.functionName = fn.name;

    // Ensure locals array is large enough
    size_t needed = frame.basePointer + fn.localCount + 16;
    if (needed > locals_.size()) {
        locals_.resize(needed * 2);
    }

    // Handle rest parameter: collect extra args into an array
    if (fn.hasRestParam) {
        // Pop all args from stack (in reverse order)
        std::vector<Value> allArgs(argCount);
        for (int i = argCount - 1; i >= 0; i--) {
            allArgs[i] = pop();
        }
        // Regular params go to their slots
        for (int i = 0; i < (int)fn.restParamIndex && i < argCount; i++) {
            locals_[frame.basePointer + i] = allArgs[i];
        }
        // Rest params collected into array
        Value restArray = Value::makeArray();
        for (int i = fn.restParamIndex; i < argCount; i++) {
            restArray.arrVal->elements.push_back(allArgs[i]);
        }
        locals_[frame.basePointer + fn.restParamIndex] = restArray;
        // Clear remaining locals
        for (int i = fn.restParamIndex + 1; i < fn.localCount; i++) {
            locals_[frame.basePointer + i] = Value();
        }
    } else {
        // Normal: copy arguments to locals
        for (int i = argCount - 1; i >= 0; i--) {
            locals_[frame.basePointer + i] = pop();
        }
    }

    callStack_.push_back(frame);
    pc_ = fn.codeOffset;
}

void VM::popFrame() {
    if (callStack_.empty()) return;
    pc_ = callStack_.back().returnPC;
    callStack_.pop_back();
}

// --- Built-ins ---

void VM::builtinPrint(const Value& val) {
    // Dart-style print: recursively formats all types into readable output
    std::function<std::string(const Value&, int, bool)> format = [&](const Value& v, int depth, bool compact) -> std::string {
        if (depth > 10) return "..."; // prevent infinite recursion

        switch (v.type) {
            case ValueType::Null: return "null";
            case ValueType::Int: return std::to_string(v.intVal);
            case ValueType::Long: return std::to_string(v.longVal);
            case ValueType::Float: {
                // Clean float formatting (remove trailing zeros like Dart)
                std::string s = std::to_string(v.floatVal);
                size_t dot = s.find('.');
                if (dot != std::string::npos) {
                    size_t last = s.find_last_not_of('0');
                    if (last == dot) last++; // keep at least one decimal
                    s = s.substr(0, last + 1);
                }
                return s;
            }
            case ValueType::Double: {
                std::string s = std::to_string(v.doubleVal);
                size_t dot = s.find('.');
                if (dot != std::string::npos) {
                    size_t last = s.find_last_not_of('0');
                    if (last == dot) last++;
                    s = s.substr(0, last + 1);
                }
                return s;
            }
            case ValueType::Bool: return v.boolVal ? "true" : "false";
            case ValueType::Char: return std::string(1, v.charVal);
            case ValueType::String: return v.strVal ? v.strVal->data : "";
            case ValueType::Array: {
                if (!v.arrVal) return "[]";
                if (v.arrVal->elements.empty()) return "[]";
                std::string result = "[";
                for (size_t i = 0; i < v.arrVal->elements.size(); i++) {
                    if (i > 0) result += ", ";
                    if (i > 100) { result += "...(+" + std::to_string(v.arrVal->elements.size() - 100) + " more)"; break; }
                    auto& elem = v.arrVal->elements[i];
                    // Strings in arrays are quoted
                    if (elem.type == ValueType::String) {
                        result += "\"" + format(elem, depth + 1, true) + "\"";
                    } else {
                        result += format(elem, depth + 1, true);
                    }
                }
                result += "]";
                return result;
            }
            case ValueType::Map: {
                if (!v.mapVal) return "{}";
                if (v.mapVal->entries.empty()) return "{}";
                std::string result = "{";
                bool first = true;
                for (auto& [k, val] : v.mapVal->entries) {
                    if (!first) result += ", ";
                    result += k + ": ";
                    if (val.type == ValueType::String) {
                        result += "\"" + format(val, depth + 1, true) + "\"";
                    } else {
                        result += format(val, depth + 1, true);
                    }
                    first = false;
                }
                result += "}";
                return result;
            }
            case ValueType::Object: {
                if (!v.objVal) return "null";
                std::string className = v.objVal->className;

                // Special handling for known internal types — show meaningful representation
                // Pointer type
                auto isPtrIt = v.objVal->fields.find("_isPointer");
                if (isPtrIt != v.objVal->fields.end() && isPtrIt->second.toBool()) {
                    auto typeIt = v.objVal->fields.find("_type");
                    auto valIt = v.objVal->fields.find("_value");
                    std::string ptrType = typeIt != v.objVal->fields.end() ? typeIt->second.toString() : "void";
                    std::string ptrVal = valIt != v.objVal->fields.end() ? format(valIt->second, depth + 1, true) : "null";
                    return "Pointer<" + ptrType + ">(" + ptrVal + ")";
                }

                // Error object — show formatted error with position
                auto fmtIt = v.objVal->fields.find("_formatted");
                if (fmtIt != v.objVal->fields.end() && fmtIt->second.type == ValueType::String) {
                    return fmtIt->second.toString();
                }

                // Enum variant
                auto ordIt = v.objVal->fields.find("_ordinal");
                auto nameIt = v.objVal->fields.find("_name");
                if (ordIt != v.objVal->fields.end() && nameIt != v.objVal->fields.end()) {
                    return className + "." + nameIt->second.toString();
                }

                // Date object
                if (className == "Date") {
                    auto valIt = v.objVal->fields.find("value");
                    if (valIt != v.objVal->fields.end()) return valIt->second.toString();
                }

                // QueryResult, WriteResult, etc — show fields
                // Generic object: show className{field: value, ...}
                std::string result = className + "(";
                bool first = true;
                int fieldCount = 0;
                for (auto& [k, fv] : v.objVal->fields) {
                    if (k[0] == '_') continue; // skip internal fields
                    if (!first) result += ", ";
                    if (fieldCount > 8) { result += "..."; break; }
                    result += k + ": ";
                    if (fv.type == ValueType::String) {
                        std::string sv = format(fv, depth + 1, true);
                        if (sv.size() > 50) sv = sv.substr(0, 50) + "...";
                        result += "\"" + sv + "\"";
                    } else {
                        result += format(fv, depth + 1, true);
                    }
                    first = false;
                    fieldCount++;
                }
                result += ")";
                return result;
            }
            case ValueType::Function: return "[Function]";
        }
        return "undefined";
    };

    std::cout << format(val, 0, false) << std::endl;
}

// --- Object operations ---

Value VM::createObject(const std::string& className) {
    trackAllocation(128);
    return Value::makeObject(className);
}

Value VM::getField(const Value& obj, const std::string& field) {
    if (obj.type == ValueType::Object && obj.objVal) {
        auto it = obj.objVal->fields.find(field);
        if (it != obj.objVal->fields.end()) return it->second;
    }
    // Map field access: map.key → lookup key in map entries
    if (obj.type == ValueType::Map && obj.mapVal) {
        auto it = obj.mapVal->entries.find(field);
        if (it != obj.mapVal->entries.end()) return it->second;
    }
    // Array .length property
    if (obj.type == ValueType::Array && obj.arrVal && field == "length") {
        return Value::makeInt(static_cast<int>(obj.arrVal->elements.size()));
    }
    return Value::makeNull();
}

void VM::setField(Value& obj, const std::string& field, const Value& val) {
    if (obj.type == ValueType::Object && obj.objVal) {
        obj.objVal->fields[field] = val;
    }
    if (obj.type == ValueType::Map && obj.mapVal) {
        obj.mapVal->entries[field] = val;
    }
}

void VM::callFunction(const std::string& name, const std::vector<Value>& args) {
    if (!module_) return;
    // Find function in module by name
    for (uint16_t i = 0; i < module_->functions.size(); i++) {
        if (module_->functions[i].name == name) {
            // Push args onto stack
            for (auto& arg : args) push(arg);
            // Save current PC, push frame, execute until frame returns
            uint32_t savedPC = pc_;
            pushFrame(i, static_cast<uint8_t>(args.size()));
            // Execute until this frame returns
            size_t targetDepth = callStack_.size() - 1;
            while (running_ && callStack_.size() > targetDepth) {
                auto op = static_cast<bytecode::OpCode>(module_->code[pc_++]);
                executeInstruction(op);
            }
            // Discard return value (event handlers are void)
            if (!stack_.empty()) pop();
            pc_ = savedPC;
            return;
        }
    }
}

void VM::throwError(const std::string& errorType, const std::string& message) {
    // Create error object with type, message, location, and stack trace
    Value errObj = Value::makeObject(errorType);
    errObj.objVal->fields["type"] = Value::makeString(errorType);
    errObj.objVal->fields["message"] = Value::makeString(message);
    errObj.objVal->fields["line"] = Value::makeInt(currentLine_);
    errObj.objVal->fields["column"] = Value::makeInt(currentColumn_);
    errObj.objVal->fields["file"] = Value::makeString(sourceFile_);

    // Build stack trace
    std::string functionName = callStack_.empty() ? "main" : callStack_.back().functionName;
    errObj.objVal->fields["caller"] = Value::makeString(functionName);

    // Stack trace as array of strings
    Value stackTrace = Value::makeArray();
    stackTrace.arrVal->elements.push_back(Value::makeString(
        (sourceFile_.empty() ? "<unknown>" : sourceFile_) + ":" +
        std::to_string(currentLine_) + ":" + std::to_string(currentColumn_) +
        " in " + functionName + "()"));
    for (int i = (int)callStack_.size() - 1; i >= 0; i--) {
        stackTrace.arrVal->elements.push_back(Value::makeString(
            "  at " + callStack_[i].functionName + "()"));
    }
    errObj.objVal->fields["stack"] = stackTrace;

    // Formatted string representation (like Dart's Error.toString())
    std::string formatted = errorType + ": " + message + "\n  at " +
        (sourceFile_.empty() ? "<unknown>" : sourceFile_) + ":" +
        std::to_string(currentLine_) + ":" + std::to_string(currentColumn_) +
        " in " + functionName + "()";
    errObj.objVal->fields["_formatted"] = Value::makeString(formatted);

    if (exceptionHandlers_.empty()) {
        // Uncaught — print with color, position, and stack trace
        std::cerr << "\033[31m" << errorType << "\033[0m: " << message << std::endl;
        if (!sourceFile_.empty() || currentLine_ > 0) {
            std::cerr << "  at " << (sourceFile_.empty() ? "<unknown>" : sourceFile_)
                      << ":" << currentLine_ << ":" << currentColumn_
                      << " in " << functionName << "()" << std::endl;
        }
        // Print call stack
        if (callStack_.size() > 1) {
            std::cerr << "  \033[90mStack trace:\033[0m" << std::endl;
            for (int i = (int)callStack_.size() - 1; i >= 0; i--) {
                std::cerr << "    at " << callStack_[i].functionName << "()" << std::endl;
            }
        }
        running_ = false;
        exitCode_ = 1;
    } else {
        // Dispatch to nearest catch handler
        ExceptionHandler handler = exceptionHandlers_.back();
        exceptionHandlers_.pop_back();
        while (stack_.size() > handler.stackSize) stack_.pop_back();
        while (callStack_.size() > handler.callStackSize) callStack_.pop_back();
        currentException_ = errObj;
        hasException_ = true;
        pc_ = handler.catchPC;
        push(errObj);
    }
}

// --- Memory ---

void VM::collectGarbage() {
    // Reference counting handles most cleanup via shared_ptr
    // This is a placeholder for a tracing GC pass
    gcCollections_++;
    // In a full implementation: mark roots (stack, locals, globals), sweep unreachable
}

void VM::trackAllocation(size_t bytes) {
    heapUsage_ += bytes;
    if (config_.enableGC && heapUsage_ > config_.maxHeapSize / 2) {
        collectGarbage();
    }
}

// --- Read helpers ---

uint8_t VM::readU8() {
    return module_->code[pc_++];
}

uint16_t VM::readU16() {
    uint16_t val = (module_->code[pc_] << 8) | module_->code[pc_ + 1];
    pc_ += 2;
    return val;
}

int32_t VM::readI32() {
    int32_t val = (module_->code[pc_] << 24) | (module_->code[pc_ + 1] << 16) |
                  (module_->code[pc_ + 2] << 8) | module_->code[pc_ + 3];
    pc_ += 4;
    return val;
}

int64_t VM::readI64() {
    int64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val = (val << 8) | module_->code[pc_++];
    }
    return val;
}

float VM::readF32() {
    int32_t bits = readI32();
    float val;
    std::memcpy(&val, &bits, 4);
    return val;
}

double VM::readF64() {
    int64_t bits = readI64();
    double val;
    std::memcpy(&val, &bits, 8);
    return val;
}

const std::string& VM::getConstantString(uint16_t index) {
    static const std::string empty;
    if (index < module_->constantPool.size() &&
        module_->constantPool[index].tag == bytecode::ConstantTag::String) {
        return module_->constantPool[index].strVal;
    }
    return empty;
}

int32_t VM::getConstantInt(uint16_t index) {
    if (index < module_->constantPool.size() &&
        module_->constantPool[index].tag == bytecode::ConstantTag::Integer) {
        return module_->constantPool[index].intVal;
    }
    return 0;
}

} // namespace runtime
} // namespace gard
