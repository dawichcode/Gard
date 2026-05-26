#include "runtime/stdlib_web.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <functional>
#include <cmath>
#include <set>

namespace gard {
namespace runtime {
namespace stdlib {

// ===== WebAssembly Binary Format Constants =====
// Wasm spec: https://webassembly.github.io/spec/core/

static const uint8_t WASM_MAGIC[] = {0x00, 0x61, 0x73, 0x6D}; // \0asm
static const uint8_t WASM_VERSION[] = {0x01, 0x00, 0x00, 0x00}; // version 1

// Section IDs
enum WasmSection : uint8_t {
    SEC_CUSTOM   = 0,
    SEC_TYPE     = 1,
    SEC_IMPORT   = 2,
    SEC_FUNCTION = 3,
    SEC_TABLE    = 4,
    SEC_MEMORY   = 5,
    SEC_GLOBAL   = 6,
    SEC_EXPORT   = 7,
    SEC_START    = 8,
    SEC_ELEMENT  = 9,
    SEC_CODE     = 10,
    SEC_DATA     = 11,
    SEC_DATACOUNT = 12
};

// Value types
enum WasmValType : uint8_t {
    WASM_I32    = 0x7F,
    WASM_I64    = 0x7E,
    WASM_F32    = 0x7D,
    WASM_F64    = 0x7C,
    WASM_FUNCREF = 0x70,
    WASM_EXTERNREF = 0x6F
};

// Export kinds
enum WasmExportKind : uint8_t {
    EXPORT_FUNC   = 0x00,
    EXPORT_TABLE  = 0x01,
    EXPORT_MEMORY = 0x02,
    EXPORT_GLOBAL = 0x03
};

// Opcodes
enum WasmOp : uint8_t {
    OP_UNREACHABLE  = 0x00,
    OP_NOP          = 0x01,
    OP_BLOCK        = 0x02,
    OP_LOOP         = 0x03,
    OP_IF           = 0x04,
    OP_ELSE         = 0x05,
    OP_END          = 0x0B,
    OP_BR           = 0x0C,
    OP_BR_IF        = 0x0D,
    OP_BR_TABLE     = 0x0E,
    OP_RETURN       = 0x0F,
    OP_CALL         = 0x10,
    OP_CALL_INDIRECT = 0x11,
    OP_DROP         = 0x1A,
    OP_SELECT       = 0x1B,
    OP_LOCAL_GET    = 0x20,
    OP_LOCAL_SET    = 0x21,
    OP_LOCAL_TEE    = 0x22,
    OP_GLOBAL_GET   = 0x23,
    OP_GLOBAL_SET   = 0x24,
    OP_I32_LOAD     = 0x28,
    OP_I64_LOAD     = 0x29,
    OP_F32_LOAD     = 0x2A,
    OP_F64_LOAD     = 0x2B,
    OP_I32_STORE    = 0x36,
    OP_I64_STORE    = 0x37,
    OP_F32_STORE    = 0x38,
    OP_F64_STORE    = 0x39,
    OP_MEMORY_SIZE  = 0x3F,
    OP_MEMORY_GROW  = 0x40,
    OP_I32_CONST    = 0x41,
    OP_I64_CONST    = 0x42,
    OP_F32_CONST    = 0x43,
    OP_F64_CONST    = 0x44,
    OP_I32_EQZ      = 0x45,
    OP_I32_EQ       = 0x46,
    OP_I32_NE       = 0x47,
    OP_I32_LT_S     = 0x48,
    OP_I32_LT_U     = 0x49,
    OP_I32_GT_S     = 0x4A,
    OP_I32_GT_U     = 0x4B,
    OP_I32_LE_S     = 0x4C,
    OP_I32_LE_U     = 0x4D,
    OP_I32_GE_S     = 0x4E,
    OP_I32_GE_U     = 0x4F,
    OP_I64_EQZ      = 0x50,
    OP_I64_EQ       = 0x51,
    OP_I64_NE       = 0x52,
    OP_I64_LT_S     = 0x53,
    OP_I64_GT_S     = 0x55,
    OP_I64_LE_S     = 0x57,
    OP_I64_GE_S     = 0x59,
    OP_F32_EQ       = 0x5B,
    OP_F32_NE       = 0x5C,
    OP_F32_LT       = 0x5D,
    OP_F32_GT       = 0x5E,
    OP_F32_LE       = 0x5F,
    OP_F32_GE       = 0x60,
    OP_F64_EQ       = 0x61,
    OP_F64_NE       = 0x62,
    OP_F64_LT       = 0x63,
    OP_F64_GT       = 0x64,
    OP_F64_LE       = 0x65,
    OP_F64_GE       = 0x66,
    OP_I32_ADD      = 0x6A,
    OP_I32_SUB      = 0x6B,
    OP_I32_MUL      = 0x6C,
    OP_I32_DIV_S    = 0x6D,
    OP_I32_REM_S    = 0x6F,
    OP_I32_AND      = 0x71,
    OP_I32_OR       = 0x72,
    OP_I32_XOR      = 0x73,
    OP_I32_SHL      = 0x74,
    OP_I32_SHR_S    = 0x75,
    OP_I32_SHR_U    = 0x76,
    OP_I64_ADD      = 0x7C,
    OP_I64_SUB      = 0x7D,
    OP_I64_MUL      = 0x7E,
    OP_I64_DIV_S    = 0x7F,
    OP_I64_REM_S    = 0x81,
    OP_F32_ADD      = 0x92,
    OP_F32_SUB      = 0x93,
    OP_F32_MUL      = 0x94,
    OP_F32_DIV      = 0x95,
    OP_F64_ADD      = 0xA0,
    OP_F64_SUB      = 0xA1,
    OP_F64_MUL      = 0xA2,
    OP_F64_DIV      = 0xA3,
    OP_I32_WRAP_I64 = 0xA7,
    OP_I64_EXTEND_I32_S = 0xAC,
    OP_F32_CONVERT_I32_S = 0xB2,
    OP_F64_CONVERT_I32_S = 0xB7,
    OP_I32_TRUNC_F64_S = 0xAA,
    OP_F64_PROMOTE_F32 = 0xBB,
    OP_F32_DEMOTE_F64 = 0xB6
};

// ===== Wasm Module Builder (internal) =====

struct WasmFuncType {
    std::vector<WasmValType> params;
    std::vector<WasmValType> results;
};

struct WasmImport {
    std::string module;
    std::string name;
    uint8_t kind; // 0=func, 1=table, 2=memory, 3=global
    uint32_t typeIdx; // for functions
};

struct WasmExport {
    std::string name;
    WasmExportKind kind;
    uint32_t index;
};

struct WasmGlobal {
    WasmValType type;
    bool mutable_;
    std::vector<uint8_t> initExpr; // init expression bytecode
};

struct WasmDataSegment {
    uint32_t memoryIdx;
    std::vector<uint8_t> offsetExpr; // i32.const offset
    std::vector<uint8_t> data;
};

struct WasmLocal {
    uint32_t count;
    WasmValType type;
};

struct WasmFunc {
    uint32_t typeIdx;
    std::vector<WasmLocal> locals;
    std::vector<uint8_t> body; // bytecode (without trailing END)
};

struct WasmModule {
    std::vector<WasmFuncType> types;
    std::vector<WasmImport> imports;
    std::vector<uint32_t> functions; // type indices for defined functions
    bool hasMemory = false;
    uint32_t memoryMin = 1; // pages (64KB each)
    uint32_t memoryMax = 256; // max pages (16MB)
    std::vector<WasmGlobal> globals;
    std::vector<WasmExport> exports;
    int32_t startFunc = -1;
    std::vector<WasmFunc> code;
    std::vector<WasmDataSegment> dataSegments;
    uint32_t heapPtr = 0; // current heap pointer for linear memory allocation

    // String table for data segments
    std::unordered_map<std::string, uint32_t> stringTable; // string -> offset in memory
    uint32_t nextStringOffset = 1024; // start strings after 1KB reserved for stack/heap ptr
};

// ===== LEB128 encoding =====

static void encodeLEB128_u(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

static void encodeLEB128_s(std::vector<uint8_t>& out, int32_t value) {
    bool more = true;
    while (more) {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40))) {
            more = false;
        } else {
            byte |= 0x80;
        }
        out.push_back(byte);
    }
}

static void encodeLEB128_s64(std::vector<uint8_t>& out, int64_t value) {
    bool more = true;
    while (more) {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40))) {
            more = false;
        } else {
            byte |= 0x80;
        }
        out.push_back(byte);
    }
}

static void encodeString(std::vector<uint8_t>& out, const std::string& s) {
    encodeLEB128_u(out, (uint32_t)s.size());
    out.insert(out.end(), s.begin(), s.end());
}

static void encodeSection(std::vector<uint8_t>& out, uint8_t sectionId, const std::vector<uint8_t>& content) {
    out.push_back(sectionId);
    encodeLEB128_u(out, (uint32_t)content.size());
    out.insert(out.end(), content.begin(), content.end());
}

// ===== Binary Wasm Emitter =====

static std::vector<uint8_t> emitWasmBinary(const WasmModule& mod) {
    std::vector<uint8_t> binary;

    // Magic + version
    binary.insert(binary.end(), WASM_MAGIC, WASM_MAGIC + 4);
    binary.insert(binary.end(), WASM_VERSION, WASM_VERSION + 4);

    // Type section
    if (!mod.types.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.types.size());
        for (auto& ft : mod.types) {
            sec.push_back(0x60); // functype
            encodeLEB128_u(sec, (uint32_t)ft.params.size());
            for (auto p : ft.params) sec.push_back((uint8_t)p);
            encodeLEB128_u(sec, (uint32_t)ft.results.size());
            for (auto r : ft.results) sec.push_back((uint8_t)r);
        }
        encodeSection(binary, SEC_TYPE, sec);
    }

    // Import section
    if (!mod.imports.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.imports.size());
        for (auto& imp : mod.imports) {
            encodeString(sec, imp.module);
            encodeString(sec, imp.name);
            sec.push_back(imp.kind);
            if (imp.kind == 0) encodeLEB128_u(sec, imp.typeIdx);
        }
        encodeSection(binary, SEC_IMPORT, sec);
    }

    // Function section (type indices for defined functions)
    if (!mod.functions.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.functions.size());
        for (auto idx : mod.functions) encodeLEB128_u(sec, idx);
        encodeSection(binary, SEC_FUNCTION, sec);
    }

    // Memory section
    if (mod.hasMemory) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, 1); // 1 memory
        sec.push_back(0x01); // has max
        encodeLEB128_u(sec, mod.memoryMin);
        encodeLEB128_u(sec, mod.memoryMax);
        encodeSection(binary, SEC_MEMORY, sec);
    }

    // Global section
    if (!mod.globals.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.globals.size());
        for (auto& g : mod.globals) {
            sec.push_back((uint8_t)g.type);
            sec.push_back(g.mutable_ ? 0x01 : 0x00);
            sec.insert(sec.end(), g.initExpr.begin(), g.initExpr.end());
            sec.push_back(OP_END);
        }
        encodeSection(binary, SEC_GLOBAL, sec);
    }

    // Export section
    if (!mod.exports.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.exports.size());
        for (auto& exp : mod.exports) {
            encodeString(sec, exp.name);
            sec.push_back((uint8_t)exp.kind);
            encodeLEB128_u(sec, exp.index);
        }
        encodeSection(binary, SEC_EXPORT, sec);
    }

    // Start section
    if (mod.startFunc >= 0) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.startFunc);
        encodeSection(binary, SEC_START, sec);
    }

    // Code section
    if (!mod.code.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.code.size());
        for (auto& fn : mod.code) {
            std::vector<uint8_t> funcBody;
            // Locals
            encodeLEB128_u(funcBody, (uint32_t)fn.locals.size());
            for (auto& loc : fn.locals) {
                encodeLEB128_u(funcBody, loc.count);
                funcBody.push_back((uint8_t)loc.type);
            }
            // Body
            funcBody.insert(funcBody.end(), fn.body.begin(), fn.body.end());
            funcBody.push_back(OP_END);
            // Encode function size + body
            encodeLEB128_u(sec, (uint32_t)funcBody.size());
            sec.insert(sec.end(), funcBody.begin(), funcBody.end());
        }
        encodeSection(binary, SEC_CODE, sec);
    }

    // Data section
    if (!mod.dataSegments.empty()) {
        std::vector<uint8_t> sec;
        encodeLEB128_u(sec, (uint32_t)mod.dataSegments.size());
        for (auto& seg : mod.dataSegments) {
            sec.push_back(0x00); // active segment, memory 0
            sec.insert(sec.end(), seg.offsetExpr.begin(), seg.offsetExpr.end());
            sec.push_back(OP_END);
            encodeLEB128_u(sec, (uint32_t)seg.data.size());
            sec.insert(sec.end(), seg.data.begin(), seg.data.end());
        }
        encodeSection(binary, SEC_DATA, sec);
    }

    return binary;
}

// ===== WAT (Text Format) Emitter =====

static std::string valTypeToWat(WasmValType t) {
    switch (t) {
        case WASM_I32: return "i32";
        case WASM_I64: return "i64";
        case WASM_F32: return "f32";
        case WASM_F64: return "f64";
        default: return "i32";
    }
}

static std::string emitWat(const WasmModule& mod,
                           const std::vector<std::string>& funcNames,
                           const std::vector<std::vector<std::string>>& funcParamNames) {
    std::ostringstream wat;
    wat << "(module\n";

    // Memory
    if (mod.hasMemory) {
        wat << "  (memory (export \"memory\") " << mod.memoryMin << " " << mod.memoryMax << ")\n";
    }

    // Globals
    for (size_t i = 0; i < mod.globals.size(); i++) {
        auto& g = mod.globals[i];
        wat << "  (global $g" << i << " (";
        if (g.mutable_) wat << "mut ";
        wat << valTypeToWat(g.type) << ") (";
        wat << valTypeToWat(g.type) << ".const 0))\n";
    }

    // Imports
    for (auto& imp : mod.imports) {
        wat << "  (import \"" << imp.module << "\" \"" << imp.name << "\" (func";
        if (imp.kind == 0 && imp.typeIdx < mod.types.size()) {
            auto& ft = mod.types[imp.typeIdx];
            for (auto p : ft.params) wat << " (param " << valTypeToWat(p) << ")";
            for (auto r : ft.results) wat << " (result " << valTypeToWat(r) << ")";
        }
        wat << "))\n";
    }

    // Functions
    uint32_t importFuncCount = 0;
    for (auto& imp : mod.imports) { if (imp.kind == 0) importFuncCount++; }

    for (size_t i = 0; i < mod.code.size(); i++) {
        uint32_t typeIdx = mod.functions[i];
        auto& ft = mod.types[typeIdx];
        std::string name = (i < funcNames.size()) ? funcNames[i] : ("func_" + std::to_string(i));

        wat << "  (func $" << name;

        // Check if exported
        for (auto& exp : mod.exports) {
            if (exp.kind == EXPORT_FUNC && exp.index == (importFuncCount + i)) {
                wat << " (export \"" << exp.name << "\")";
                break;
            }
        }

        // Params
        for (size_t p = 0; p < ft.params.size(); p++) {
            std::string pname = (i < funcParamNames.size() && p < funcParamNames[i].size())
                ? funcParamNames[i][p] : ("p" + std::to_string(p));
            wat << " (param $" << pname << " " << valTypeToWat(ft.params[p]) << ")";
        }
        // Results
        for (auto r : ft.results) {
            wat << " (result " << valTypeToWat(r) << ")";
        }
        wat << "\n";

        // Locals
        for (auto& loc : mod.code[i].locals) {
            for (uint32_t l = 0; l < loc.count; l++) {
                wat << "    (local " << valTypeToWat(loc.type) << ")\n";
            }
        }

        // We emit a placeholder body comment (full disassembly would be very complex)
        wat << "    ;; body: " << mod.code[i].body.size() << " bytes\n";
        wat << "  )\n";
    }

    // Data segments
    for (auto& seg : mod.dataSegments) {
        wat << "  (data (i32.const ";
        // Parse offset from offsetExpr (i32.const N)
        if (seg.offsetExpr.size() >= 2 && seg.offsetExpr[0] == OP_I32_CONST) {
            // Decode LEB128
            int32_t offset = 0;
            int shift = 0;
            for (size_t b = 1; b < seg.offsetExpr.size(); b++) {
                offset |= ((int32_t)(seg.offsetExpr[b] & 0x7F)) << shift;
                shift += 7;
                if (!(seg.offsetExpr[b] & 0x80)) break;
            }
            wat << offset;
        } else {
            wat << "0";
        }
        wat << ") \"";
        for (uint8_t byte : seg.data) {
            if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') {
                wat << (char)byte;
            } else {
                char hex[5];
                snprintf(hex, sizeof(hex), "\\%02x", byte);
                wat << hex;
            }
        }
        wat << "\")\n";
    }

    wat << ")\n";
    return wat.str();
}

// ===== Gard Type → Wasm Type mapping =====

static WasmValType gardTypeToWasm(const std::string& type) {
    if (type == "int" || type == "i32" || type == "bool") return WASM_I32;
    if (type == "long" || type == "i64") return WASM_I64;
    if (type == "float" || type == "f32") return WASM_F32;
    if (type == "double" || type == "f64") return WASM_F64;
    // Strings and objects are passed as i32 pointers in linear memory
    return WASM_I32;
}

// ===== Module storage =====

static std::unordered_map<int, std::shared_ptr<WasmModule>> g_wasmModules;
static int g_nextWasmId = 1;
static std::unordered_map<int, std::vector<std::string>> g_wasmFuncNames;
static std::unordered_map<int, std::vector<std::vector<std::string>>> g_wasmParamNames;

// ===== Register Web Module =====

void registerWebModule(VM& vm) {

    // Wasm.createModule(options?) — create a new WebAssembly module builder
    // options: { memory: { min: 1, max: 256 } }
    vm.registerNative("Wasm.createModule", [&vm](const std::vector<Value>& a) -> Value {
        auto mod = std::make_shared<WasmModule>();
        mod->hasMemory = true;
        mod->memoryMin = 1;
        mod->memoryMax = 256;

        // Parse options
        if (!a.empty() && a[0].type == ValueType::Object && a[0].objVal) {
            auto memIt = a[0].objVal->fields.find("memory");
            if (memIt != a[0].objVal->fields.end() && memIt->second.objVal) {
                auto minIt = memIt->second.objVal->fields.find("min");
                auto maxIt = memIt->second.objVal->fields.find("max");
                if (minIt != memIt->second.objVal->fields.end()) mod->memoryMin = minIt->second.toInt();
                if (maxIt != memIt->second.objVal->fields.end()) mod->memoryMax = maxIt->second.toInt();
            }
            auto noMemIt = a[0].objVal->fields.find("noMemory");
            if (noMemIt != a[0].objVal->fields.end() && noMemIt->second.toBool()) mod->hasMemory = false;
        } else if (!a.empty() && a[0].type == ValueType::Map && a[0].mapVal) {
            auto memIt = a[0].mapVal->entries.find("memory");
            if (memIt != a[0].mapVal->entries.end() && memIt->second.mapVal) {
                auto minIt = memIt->second.mapVal->entries.find("min");
                auto maxIt = memIt->second.mapVal->entries.find("max");
                if (minIt != memIt->second.mapVal->entries.end()) mod->memoryMin = minIt->second.toInt();
                if (maxIt != memIt->second.mapVal->entries.end()) mod->memoryMax = maxIt->second.toInt();
            }
        }

        if (mod->memoryMin < 0 || mod->memoryMax < 1 || mod->memoryMin > mod->memoryMax) {
            vm.throwError("GardWasmError", "Wasm.createModule: invalid memory bounds (min=" + std::to_string(mod->memoryMin) + ", max=" + std::to_string(mod->memoryMax) + ")");
            return Value::makeNull();
        }

        // Add heap pointer global (mutable i32, starts at nextStringOffset)
        WasmGlobal heapPtrGlobal;
        heapPtrGlobal.type = WASM_I32;
        heapPtrGlobal.mutable_ = true;
        heapPtrGlobal.initExpr.push_back(OP_I32_CONST);
        encodeLEB128_s(heapPtrGlobal.initExpr, (int32_t)mod->nextStringOffset);
        mod->globals.push_back(heapPtrGlobal);

        int id = g_nextWasmId++;
        g_wasmModules[id] = mod;
        g_wasmFuncNames[id] = {};
        g_wasmParamNames[id] = {};

        Value modObj = Value::makeObject("WasmModule");
        modObj.objVal->fields["_wasmId"] = Value::makeInt(id);
        modObj.objVal->fields["memoryMin"] = Value::makeInt(mod->memoryMin);
        modObj.objVal->fields["memoryMax"] = Value::makeInt(mod->memoryMax);
        return modObj;
    });

    // Wasm.addFunction(module, name, params, results, body) — add a function to the module
    // params: [{name: "x", type: "i32"}, ...] or ["i32", "i64", ...]
    // results: ["i32"] or ["f64"]
    // body: array of wasm instructions (opcode objects or raw bytes)
    vm.registerNative("Wasm.addFunction", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.addFunction: requires module, name, params, results, body"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.addFunction: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        std::string funcName = a[1].toString();

        // Parse params
        WasmFuncType ft;
        std::vector<std::string> paramNames;
        if (a[2].type == ValueType::Array && a[2].arrVal) {
            for (auto& p : a[2].arrVal->elements) {
                if (p.type == ValueType::Object && p.objVal) {
                    auto typeIt = p.objVal->fields.find("type");
                    auto nameIt = p.objVal->fields.find("name");
                    std::string typeName = typeIt != p.objVal->fields.end() ? typeIt->second.toString() : "i32";
                    std::string pName = nameIt != p.objVal->fields.end() ? nameIt->second.toString() : "";
                    ft.params.push_back(gardTypeToWasm(typeName));
                    paramNames.push_back(pName.empty() ? ("p" + std::to_string(paramNames.size())) : pName);
                } else if (p.type == ValueType::Map && p.mapVal) {
                    auto typeIt = p.mapVal->entries.find("type");
                    auto nameIt = p.mapVal->entries.find("name");
                    std::string typeName = typeIt != p.mapVal->entries.end() ? typeIt->second.toString() : "i32";
                    std::string pName = nameIt != p.mapVal->entries.end() ? nameIt->second.toString() : "";
                    ft.params.push_back(gardTypeToWasm(typeName));
                    paramNames.push_back(pName.empty() ? ("p" + std::to_string(paramNames.size())) : pName);
                } else {
                    ft.params.push_back(gardTypeToWasm(p.toString()));
                    paramNames.push_back("p" + std::to_string(paramNames.size()));
                }
            }
        }

        // Parse results
        if (a[3].type == ValueType::Array && a[3].arrVal) {
            for (auto& r : a[3].arrVal->elements) {
                ft.results.push_back(gardTypeToWasm(r.toString()));
            }
        } else if (a[3].type == ValueType::String) {
            std::string rs = a[3].toString();
            if (rs != "void" && !rs.empty()) ft.results.push_back(gardTypeToWasm(rs));
        }

        // Register type (dedup)
        uint32_t typeIdx = (uint32_t)mod->types.size();
        for (size_t ti = 0; ti < mod->types.size(); ti++) {
            if (mod->types[ti].params == ft.params && mod->types[ti].results == ft.results) {
                typeIdx = (uint32_t)ti;
                goto type_found;
            }
        }
        mod->types.push_back(ft);
        type_found:

        mod->functions.push_back(typeIdx);

        // Parse body — array of instruction objects or raw byte arrays
        WasmFunc func;
        func.typeIdx = typeIdx;
        if (a[4].type == ValueType::Array && a[4].arrVal) {
            for (auto& instr : a[4].arrVal->elements) {
                if (instr.type == ValueType::Int) {
                    func.body.push_back((uint8_t)instr.toInt());
                } else if (instr.type == ValueType::Array && instr.arrVal) {
                    // Raw byte array
                    for (auto& b : instr.arrVal->elements) {
                        func.body.push_back((uint8_t)b.toInt());
                    }
                } else if (instr.type == ValueType::Object && instr.objVal) {
                    // Instruction object: { op: "i32.add" } or { op: "i32.const", value: 42 }
                    auto opIt = instr.objVal->fields.find("op");
                    if (opIt == instr.objVal->fields.end()) continue;
                    std::string op = opIt->second.toString();
                    auto valIt = instr.objVal->fields.find("value");

                    // Map string opcodes to bytes
                    if (op == "i32.const") { func.body.push_back(OP_I32_CONST); encodeLEB128_s(func.body, valIt != instr.objVal->fields.end() ? valIt->second.toInt() : 0); }
                    else if (op == "i64.const") { func.body.push_back(OP_I64_CONST); encodeLEB128_s64(func.body, valIt != instr.objVal->fields.end() ? (int64_t)valIt->second.toInt() : 0); }
                    else if (op == "f32.const") { func.body.push_back(OP_F32_CONST); float f = valIt != instr.objVal->fields.end() ? (float)valIt->second.toDouble() : 0.0f; uint8_t* fb = (uint8_t*)&f; func.body.insert(func.body.end(), fb, fb+4); }
                    else if (op == "f64.const") { func.body.push_back(OP_F64_CONST); double d = valIt != instr.objVal->fields.end() ? valIt->second.toDouble() : 0.0; uint8_t* db = (uint8_t*)&d; func.body.insert(func.body.end(), db, db+8); }
                    else if (op == "local.get") { func.body.push_back(OP_LOCAL_GET); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "local.set") { func.body.push_back(OP_LOCAL_SET); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "local.tee") { func.body.push_back(OP_LOCAL_TEE); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "global.get") { func.body.push_back(OP_GLOBAL_GET); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "global.set") { func.body.push_back(OP_GLOBAL_SET); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "call") { func.body.push_back(OP_CALL); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "br") { func.body.push_back(OP_BR); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "br_if") { func.body.push_back(OP_BR_IF); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "i32.add") func.body.push_back(OP_I32_ADD);
                    else if (op == "i32.sub") func.body.push_back(OP_I32_SUB);
                    else if (op == "i32.mul") func.body.push_back(OP_I32_MUL);
                    else if (op == "i32.div_s") func.body.push_back(OP_I32_DIV_S);
                    else if (op == "i32.rem_s") func.body.push_back(OP_I32_REM_S);
                    else if (op == "i32.and") func.body.push_back(OP_I32_AND);
                    else if (op == "i32.or") func.body.push_back(OP_I32_OR);
                    else if (op == "i32.xor") func.body.push_back(OP_I32_XOR);
                    else if (op == "i32.shl") func.body.push_back(OP_I32_SHL);
                    else if (op == "i32.shr_s") func.body.push_back(OP_I32_SHR_S);
                    else if (op == "i32.shr_u") func.body.push_back(OP_I32_SHR_U);
                    else if (op == "i32.eq") func.body.push_back(OP_I32_EQ);
                    else if (op == "i32.ne") func.body.push_back(OP_I32_NE);
                    else if (op == "i32.lt_s") func.body.push_back(OP_I32_LT_S);
                    else if (op == "i32.gt_s") func.body.push_back(OP_I32_GT_S);
                    else if (op == "i32.le_s") func.body.push_back(OP_I32_LE_S);
                    else if (op == "i32.ge_s") func.body.push_back(OP_I32_GE_S);
                    else if (op == "i32.eqz") func.body.push_back(OP_I32_EQZ);
                    else if (op == "i64.add") func.body.push_back(OP_I64_ADD);
                    else if (op == "i64.sub") func.body.push_back(OP_I64_SUB);
                    else if (op == "i64.mul") func.body.push_back(OP_I64_MUL);
                    else if (op == "i64.div_s") func.body.push_back(OP_I64_DIV_S);
                    else if (op == "i64.eq") func.body.push_back(OP_I64_EQ);
                    else if (op == "i64.ne") func.body.push_back(OP_I64_NE);
                    else if (op == "i64.lt_s") func.body.push_back(OP_I64_LT_S);
                    else if (op == "i64.eqz") func.body.push_back(OP_I64_EQZ);
                    else if (op == "f32.add") func.body.push_back(OP_F32_ADD);
                    else if (op == "f32.sub") func.body.push_back(OP_F32_SUB);
                    else if (op == "f32.mul") func.body.push_back(OP_F32_MUL);
                    else if (op == "f32.div") func.body.push_back(OP_F32_DIV);
                    else if (op == "f32.eq") func.body.push_back(OP_F32_EQ);
                    else if (op == "f32.ne") func.body.push_back(OP_F32_NE);
                    else if (op == "f32.lt") func.body.push_back(OP_F32_LT);
                    else if (op == "f32.gt") func.body.push_back(OP_F32_GT);
                    else if (op == "f64.add") func.body.push_back(OP_F64_ADD);
                    else if (op == "f64.sub") func.body.push_back(OP_F64_SUB);
                    else if (op == "f64.mul") func.body.push_back(OP_F64_MUL);
                    else if (op == "f64.div") func.body.push_back(OP_F64_DIV);
                    else if (op == "f64.eq") func.body.push_back(OP_F64_EQ);
                    else if (op == "f64.ne") func.body.push_back(OP_F64_NE);
                    else if (op == "f64.lt") func.body.push_back(OP_F64_LT);
                    else if (op == "f64.gt") func.body.push_back(OP_F64_GT);
                    else if (op == "drop") func.body.push_back(OP_DROP);
                    else if (op == "select") func.body.push_back(OP_SELECT);
                    else if (op == "return") func.body.push_back(OP_RETURN);
                    else if (op == "nop") func.body.push_back(OP_NOP);
                    else if (op == "unreachable") func.body.push_back(OP_UNREACHABLE);
                    else if (op == "block") { func.body.push_back(OP_BLOCK); func.body.push_back(0x40); } // void block
                    else if (op == "loop") { func.body.push_back(OP_LOOP); func.body.push_back(0x40); }
                    else if (op == "if") { func.body.push_back(OP_IF); func.body.push_back(0x40); }
                    else if (op == "else") func.body.push_back(OP_ELSE);
                    else if (op == "end") func.body.push_back(OP_END);
                    else if (op == "memory.size") { func.body.push_back(OP_MEMORY_SIZE); func.body.push_back(0x00); }
                    else if (op == "memory.grow") { func.body.push_back(OP_MEMORY_GROW); func.body.push_back(0x00); }
                    else if (op == "i32.load") { func.body.push_back(OP_I32_LOAD); encodeLEB128_u(func.body, 2); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "i32.store") { func.body.push_back(OP_I32_STORE); encodeLEB128_u(func.body, 2); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "i64.load") { func.body.push_back(OP_I64_LOAD); encodeLEB128_u(func.body, 3); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "i64.store") { func.body.push_back(OP_I64_STORE); encodeLEB128_u(func.body, 3); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "f64.load") { func.body.push_back(OP_F64_LOAD); encodeLEB128_u(func.body, 3); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "f64.store") { func.body.push_back(OP_F64_STORE); encodeLEB128_u(func.body, 3); encodeLEB128_u(func.body, valIt != instr.objVal->fields.end() ? (uint32_t)valIt->second.toInt() : 0); }
                    else if (op == "i32.wrap_i64") func.body.push_back(OP_I32_WRAP_I64);
                    else if (op == "i64.extend_i32_s") func.body.push_back(OP_I64_EXTEND_I32_S);
                    else if (op == "f32.convert_i32_s") func.body.push_back(OP_F32_CONVERT_I32_S);
                    else if (op == "f64.convert_i32_s") func.body.push_back(OP_F64_CONVERT_I32_S);
                    else if (op == "i32.trunc_f64_s") func.body.push_back(OP_I32_TRUNC_F64_S);
                    else if (op == "f64.promote_f32") func.body.push_back(OP_F64_PROMOTE_F32);
                    else if (op == "f32.demote_f64") func.body.push_back(OP_F32_DEMOTE_F64);
                    else {
                        vm.throwError("GardWasmError", "Wasm.addFunction: unknown opcode '" + op + "'");
                        return Value::makeNull();
                    }
                }
            }
        }

        mod->code.push_back(func);
        g_wasmFuncNames[id].push_back(funcName);
        g_wasmParamNames[id].push_back(paramNames);

        Value result = Value::makeObject("WasmFunction");
        result.objVal->fields["name"] = Value::makeString(funcName);
        result.objVal->fields["index"] = Value::makeInt((int)(mod->code.size() - 1));
        result.objVal->fields["typeIndex"] = Value::makeInt((int)typeIdx);
        return result;
    });

    // Wasm.addLocal(module, funcIndex, type) — add a local variable to a function
    vm.registerNative("Wasm.addLocal", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.addLocal: requires module, funcIndex, type"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.addLocal: module not found"); return Value::makeNull(); }
        auto& mod = it->second;
        int funcIdx = a[1].toInt();
        if (funcIdx < 0 || funcIdx >= (int)mod->code.size()) { vm.throwError("GardWasmError", "Wasm.addLocal: function index out of range"); return Value::makeNull(); }
        WasmValType type = gardTypeToWasm(a[2].toString());
        // Merge with existing locals of same type or add new
        auto& locals = mod->code[funcIdx].locals;
        if (!locals.empty() && locals.back().type == type) {
            locals.back().count++;
        } else {
            locals.push_back({1, type});
        }
        return Value::makeBool(true);
    });

    // Wasm.exportFunction(module, funcIndex, exportName) — export a function
    vm.registerNative("Wasm.exportFunction", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.exportFunction: requires module, funcIndex, exportName"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.exportFunction: module not found"); return Value::makeNull(); }
        auto& mod = it->second;
        int funcIdx = a[1].toInt();
        std::string exportName = a[2].toString();

        // Calculate actual function index (imports + defined)
        uint32_t importFuncCount = 0;
        for (auto& imp : mod->imports) { if (imp.kind == 0) importFuncCount++; }

        WasmExport exp;
        exp.name = exportName;
        exp.kind = EXPORT_FUNC;
        exp.index = importFuncCount + (uint32_t)funcIdx;
        mod->exports.push_back(exp);
        return Value::makeBool(true);
    });

    // Wasm.exportMemory(module, exportName?) — export the linear memory
    vm.registerNative("Wasm.exportMemory", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.exportMemory: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.exportMemory: module not found"); return Value::makeNull(); }
        auto& mod = it->second;
        if (!mod->hasMemory) { vm.throwError("GardWasmError", "Wasm.exportMemory: module has no memory"); return Value::makeNull(); }
        std::string name = a.size() >= 2 ? a[1].toString() : "memory";
        WasmExport exp;
        exp.name = name;
        exp.kind = EXPORT_MEMORY;
        exp.index = 0;
        mod->exports.push_back(exp);
        return Value::makeBool(true);
    });

    // Wasm.addImport(module, moduleName, funcName, params, results) — import a function
    vm.registerNative("Wasm.addImport", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.addImport: requires module, moduleName, funcName, params, results"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.addImport: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        WasmFuncType ft;
        if (a[3].type == ValueType::Array && a[3].arrVal) {
            for (auto& p : a[3].arrVal->elements) ft.params.push_back(gardTypeToWasm(p.toString()));
        }
        if (a[4].type == ValueType::Array && a[4].arrVal) {
            for (auto& r : a[4].arrVal->elements) ft.results.push_back(gardTypeToWasm(r.toString()));
        }

        // Register type
        uint32_t typeIdx = (uint32_t)mod->types.size();
        for (size_t ti = 0; ti < mod->types.size(); ti++) {
            if (mod->types[ti].params == ft.params && mod->types[ti].results == ft.results) { typeIdx = (uint32_t)ti; goto imp_type_found; }
        }
        mod->types.push_back(ft);
        imp_type_found:

        WasmImport imp;
        imp.module = a[1].toString();
        imp.name = a[2].toString();
        imp.kind = 0; // function
        imp.typeIdx = typeIdx;
        mod->imports.push_back(imp);

        Value result = Value::makeObject("WasmImport");
        result.objVal->fields["module"] = Value::makeString(imp.module);
        result.objVal->fields["name"] = Value::makeString(imp.name);
        result.objVal->fields["index"] = Value::makeInt((int)(mod->imports.size() - 1));
        return result;
    });

    // Wasm.addGlobal(module, type, mutable, initValue) — add a global variable
    vm.registerNative("Wasm.addGlobal", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.addGlobal: requires module, type, mutable, initValue"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.addGlobal: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        WasmGlobal g;
        g.type = gardTypeToWasm(a[1].toString());
        g.mutable_ = a[2].toBool();
        // Init expression
        switch (g.type) {
            case WASM_I32: g.initExpr.push_back(OP_I32_CONST); encodeLEB128_s(g.initExpr, a[3].toInt()); break;
            case WASM_I64: g.initExpr.push_back(OP_I64_CONST); encodeLEB128_s64(g.initExpr, (int64_t)a[3].toInt()); break;
            case WASM_F32: { g.initExpr.push_back(OP_F32_CONST); float f = (float)a[3].toDouble(); uint8_t* fb = (uint8_t*)&f; g.initExpr.insert(g.initExpr.end(), fb, fb+4); break; }
            case WASM_F64: { g.initExpr.push_back(OP_F64_CONST); double d = a[3].toDouble(); uint8_t* db = (uint8_t*)&d; g.initExpr.insert(g.initExpr.end(), db, db+8); break; }
            default: g.initExpr.push_back(OP_I32_CONST); encodeLEB128_s(g.initExpr, 0); break;
        }
        mod->globals.push_back(g);
        return Value::makeInt((int)(mod->globals.size() - 1));
    });

    // Wasm.addDataSegment(module, offset, data) — add a data segment to linear memory
    // data: string or array of bytes
    vm.registerNative("Wasm.addDataSegment", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.addDataSegment: requires module, offset, data"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.addDataSegment: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        int32_t offset = a[1].toInt();
        WasmDataSegment seg;
        seg.memoryIdx = 0;
        seg.offsetExpr.push_back(OP_I32_CONST);
        encodeLEB128_s(seg.offsetExpr, offset);

        if (a[2].type == ValueType::String) {
            std::string s = a[2].toString();
            seg.data.insert(seg.data.end(), s.begin(), s.end());
            seg.data.push_back(0); // null terminator
        } else if (a[2].type == ValueType::Array && a[2].arrVal) {
            for (auto& b : a[2].arrVal->elements) seg.data.push_back((uint8_t)b.toInt());
        }

        mod->dataSegments.push_back(seg);
        Value result = Value::makeObject("DataSegment");
        result.objVal->fields["offset"] = Value::makeInt(offset);
        result.objVal->fields["size"] = Value::makeInt((int)seg.data.size());
        return result;
    });

    // Wasm.addString(module, str) — add a string to the data section, returns {ptr, len}
    vm.registerNative("Wasm.addString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.addString: requires module and string"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.addString: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        std::string str = a[1].toString();
        // Check if already in string table
        auto stIt = mod->stringTable.find(str);
        if (stIt != mod->stringTable.end()) {
            Value result = Value::makeObject("WasmString");
            result.objVal->fields["ptr"] = Value::makeInt((int)stIt->second);
            result.objVal->fields["len"] = Value::makeInt((int)str.size());
            return result;
        }

        uint32_t offset = mod->nextStringOffset;
        mod->stringTable[str] = offset;

        WasmDataSegment seg;
        seg.memoryIdx = 0;
        seg.offsetExpr.push_back(OP_I32_CONST);
        encodeLEB128_s(seg.offsetExpr, (int32_t)offset);
        seg.data.insert(seg.data.end(), str.begin(), str.end());
        // No null terminator — use ptr+len convention
        mod->dataSegments.push_back(seg);
        mod->nextStringOffset += (uint32_t)str.size();
        // Align to 4 bytes
        while (mod->nextStringOffset % 4 != 0) mod->nextStringOffset++;

        Value result = Value::makeObject("WasmString");
        result.objVal->fields["ptr"] = Value::makeInt((int)offset);
        result.objVal->fields["len"] = Value::makeInt((int)str.size());
        return result;
    });

    // Wasm.compile(module) — compile module to binary .wasm format, returns byte array
    vm.registerNative("Wasm.compile", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.compile: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.compile: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        if (mod->functions.size() != mod->code.size()) {
            vm.throwError("GardWasmError", "Wasm.compile: function/code count mismatch (functions=" + std::to_string(mod->functions.size()) + ", code=" + std::to_string(mod->code.size()) + ")");
            return Value::makeNull();
        }

        std::vector<uint8_t> binary = emitWasmBinary(*mod);

        Value result = Value::makeArray();
        for (uint8_t b : binary) result.arrVal->elements.push_back(Value::makeInt(b));
        return result;
    });

    // Wasm.toWat(module) — compile module to WAT text format
    vm.registerNative("Wasm.toWat", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.toWat: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.toWat: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        auto& names = g_wasmFuncNames[id];
        auto& paramNames = g_wasmParamNames[id];
        std::string wat = emitWat(*mod, names, paramNames);
        return Value::makeString(wat);
    });

    // Wasm.writeFile(module, path) — write compiled .wasm binary to file
    vm.registerNative("Wasm.writeFile", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.writeFile: requires module and file path"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.writeFile: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        std::string path = a[1].toString();
        std::vector<uint8_t> binary = emitWasmBinary(*mod);

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            vm.throwError("GardWasmError", "Wasm.writeFile: cannot open '" + path + "' for writing");
            return Value::makeNull();
        }
        file.write(reinterpret_cast<const char*>(binary.data()), binary.size());
        file.close();

        Value result = Value::makeObject("WriteResult");
        result.objVal->fields["path"] = Value::makeString(path);
        result.objVal->fields["size"] = Value::makeInt((int)binary.size());
        return result;
    });

    // Wasm.writeWat(module, path) — write WAT text format to file
    vm.registerNative("Wasm.writeWat", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.writeWat: requires module and file path"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.writeWat: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        std::string path = a[1].toString();
        auto& names = g_wasmFuncNames[id];
        auto& paramNames = g_wasmParamNames[id];
        std::string wat = emitWat(*mod, names, paramNames);

        std::ofstream file(path);
        if (!file.is_open()) {
            vm.throwError("GardWasmError", "Wasm.writeWat: cannot open '" + path + "' for writing");
            return Value::makeNull();
        }
        file << wat;
        file.close();

        Value result = Value::makeObject("WriteResult");
        result.objVal->fields["path"] = Value::makeString(path);
        result.objVal->fields["size"] = Value::makeInt((int)wat.size());
        return result;
    });

    // Wasm.validate(module) — validate the module structure
    vm.registerNative("Wasm.validate", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.validate: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.validate: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        Value errors = Value::makeArray();

        // Check function/code count match
        if (mod->functions.size() != mod->code.size()) {
            errors.arrVal->elements.push_back(Value::makeString("function count (" + std::to_string(mod->functions.size()) + ") != code count (" + std::to_string(mod->code.size()) + ")"));
        }

        // Check type indices are valid
        for (size_t i = 0; i < mod->functions.size(); i++) {
            if (mod->functions[i] >= mod->types.size()) {
                errors.arrVal->elements.push_back(Value::makeString("function " + std::to_string(i) + " references invalid type index " + std::to_string(mod->functions[i])));
            }
        }

        // Check import type indices
        for (auto& imp : mod->imports) {
            if (imp.kind == 0 && imp.typeIdx >= mod->types.size()) {
                errors.arrVal->elements.push_back(Value::makeString("import '" + imp.module + "." + imp.name + "' references invalid type index"));
            }
        }

        // Check export indices
        uint32_t totalFuncs = 0;
        for (auto& imp : mod->imports) { if (imp.kind == 0) totalFuncs++; }
        totalFuncs += (uint32_t)mod->code.size();
        for (auto& exp : mod->exports) {
            if (exp.kind == EXPORT_FUNC && exp.index >= totalFuncs) {
                errors.arrVal->elements.push_back(Value::makeString("export '" + exp.name + "' references invalid function index " + std::to_string(exp.index)));
            }
        }

        // Check memory bounds
        if (mod->hasMemory && mod->memoryMin > mod->memoryMax) {
            errors.arrVal->elements.push_back(Value::makeString("memory min (" + std::to_string(mod->memoryMin) + ") > max (" + std::to_string(mod->memoryMax) + ")"));
        }

        Value result = Value::makeObject("ValidationResult");
        result.objVal->fields["valid"] = Value::makeBool(errors.arrVal->elements.empty());
        result.objVal->fields["errors"] = errors;
        return result;
    });

    // Wasm.setStartFunction(module, funcIndex) — set the start function (auto-called on instantiation)
    vm.registerNative("Wasm.setStartFunction", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.setStartFunction: requires module and funcIndex"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.setStartFunction: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        int funcIdx = a[1].toInt();
        uint32_t importFuncCount = 0;
        for (auto& imp : mod->imports) { if (imp.kind == 0) importFuncCount++; }
        mod->startFunc = importFuncCount + funcIdx;

        // Validate: start function must have no params and no results
        if (funcIdx >= 0 && funcIdx < (int)mod->functions.size()) {
            uint32_t typeIdx = mod->functions[funcIdx];
            if (typeIdx < mod->types.size()) {
                auto& ft = mod->types[typeIdx];
                if (!ft.params.empty() || !ft.results.empty()) {
                    vm.throwError("GardWasmError", "Wasm.setStartFunction: start function must have no parameters and no return value");
                    return Value::makeNull();
                }
            }
        }
        return Value::makeBool(true);
    });

    // Wasm.getSize(module) — get the compiled binary size without writing
    vm.registerNative("Wasm.getSize", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.getSize: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.getSize: module not found"); return Value::makeNull(); }
        auto& mod = it->second;
        std::vector<uint8_t> binary = emitWasmBinary(*mod);
        return Value::makeInt((int)binary.size());
    });

    // Wasm.info(module) — get module metadata
    vm.registerNative("Wasm.info", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.info: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.info: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        Value info = Value::makeObject("WasmInfo");
        info.objVal->fields["types"] = Value::makeInt((int)mod->types.size());
        info.objVal->fields["imports"] = Value::makeInt((int)mod->imports.size());
        info.objVal->fields["functions"] = Value::makeInt((int)mod->code.size());
        info.objVal->fields["exports"] = Value::makeInt((int)mod->exports.size());
        info.objVal->fields["globals"] = Value::makeInt((int)mod->globals.size());
        info.objVal->fields["dataSegments"] = Value::makeInt((int)mod->dataSegments.size());
        info.objVal->fields["hasMemory"] = Value::makeBool(mod->hasMemory);
        info.objVal->fields["memoryPages"] = Value::makeInt(mod->memoryMin);
        info.objVal->fields["memoryMaxPages"] = Value::makeInt(mod->memoryMax);
        info.objVal->fields["hasStart"] = Value::makeBool(mod->startFunc >= 0);
        return info;
    });

    // Wasm.allocate(module, size) — allocate bytes in linear memory, returns pointer
    vm.registerNative("Wasm.allocate", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.allocate: requires module and size"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "Wasm.allocate: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        int size = a[1].toInt();
        if (size <= 0) { vm.throwError("GardWasmError", "Wasm.allocate: size must be > 0"); return Value::makeNull(); }

        uint32_t ptr = mod->nextStringOffset;
        mod->nextStringOffset += (uint32_t)size;
        // Align to 4 bytes
        while (mod->nextStringOffset % 4 != 0) mod->nextStringOffset++;

        // Check if we exceed memory
        uint32_t maxBytes = mod->memoryMax * 65536;
        if (mod->nextStringOffset > maxBytes) {
            vm.throwError("GardWasmMemoryError", "Wasm.allocate: out of linear memory (requested " + std::to_string(size) + " bytes, limit=" + std::to_string(maxBytes) + ")");
            return Value::makeNull();
        }

        return Value::makeInt((int)ptr);
    });

    // Wasm.destroy(module) — free module resources
    vm.registerNative("Wasm.destroy", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "Wasm.destroy: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        g_wasmModules.erase(id);
        g_wasmFuncNames.erase(id);
        g_wasmParamNames.erase(id);
        return Value::makeBool(true);
    });

    // ===== 7.8 WebAssembly Module System =====

    // WasmMemory.create(module, initial, maximum) — create a memory instance for read/write ops
    // Returns a WasmMemory object backed by a real byte buffer for host-side memory operations
    vm.registerNative("WasmMemory.create", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.create: requires module and initial pages"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmMemoryError", "WasmMemory.create: module not found"); return Value::makeNull(); }

        int initial = a[1].toInt();
        int maximum = a.size() >= 3 ? a[2].toInt() : initial * 4;
        if (initial < 1) { vm.throwError("GardWasmMemoryError", "WasmMemory.create: initial pages must be >= 1"); return Value::makeNull(); }
        if (maximum < initial) { vm.throwError("GardWasmMemoryError", "WasmMemory.create: maximum cannot be less than initial"); return Value::makeNull(); }

        // Allocate real memory buffer (initial pages * 64KB)
        int bufSize = initial * 65536;
        Value mem = Value::makeObject("WasmMemory");
        mem.objVal->fields["_wasmId"] = Value::makeInt(id);
        mem.objVal->fields["_pages"] = Value::makeInt(initial);
        mem.objVal->fields["_maxPages"] = Value::makeInt(maximum);
        mem.objVal->fields["_size"] = Value::makeInt(bufSize);
        // Store buffer as byte array
        Value buf = Value::makeArray();
        buf.arrVal->elements.resize(bufSize, Value::makeInt(0));
        mem.objVal->fields["_buffer"] = buf;
        mem.objVal->fields["pages"] = Value::makeInt(initial);
        mem.objVal->fields["maxPages"] = Value::makeInt(maximum);
        mem.objVal->fields["byteLength"] = Value::makeInt(bufSize);
        return mem;
    });

    // WasmMemory.grow(memory, deltaPages) — grow memory by delta pages
    vm.registerNative("WasmMemory.grow", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.grow: requires memory and delta pages"); return Value::makeNull(); }
        int currentPages = a[0].objVal->fields["_pages"].toInt();
        int maxPages = a[0].objVal->fields["_maxPages"].toInt();
        int delta = a[1].toInt();
        if (delta < 0) { vm.throwError("GardWasmMemoryError", "WasmMemory.grow: delta must be >= 0"); return Value::makeNull(); }
        int newPages = currentPages + delta;
        if (newPages > maxPages) {
            vm.throwError("GardWasmMemoryError", "WasmMemory.grow: cannot grow beyond maximum (" + std::to_string(maxPages) + " pages)");
            return Value::makeNull();
        }
        int newSize = newPages * 65536;
        auto& buf = a[0].objVal->fields["_buffer"];
        if (buf.arrVal) buf.arrVal->elements.resize(newSize, Value::makeInt(0));
        a[0].objVal->fields["_pages"] = Value::makeInt(newPages);
        a[0].objVal->fields["_size"] = Value::makeInt(newSize);
        a[0].objVal->fields["pages"] = Value::makeInt(newPages);
        a[0].objVal->fields["byteLength"] = Value::makeInt(newSize);
        return Value::makeInt(currentPages); // returns previous page count (wasm spec)
    });

    // WasmMemory.readInt32(memory, ptr) — read a 32-bit integer from memory at byte offset
    vm.registerNative("WasmMemory.readInt32", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.readInt32: requires memory and pointer"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.readInt32: out of bounds (ptr=" + std::to_string(ptr) + ", size=" + std::to_string(size) + ")"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeInt(0);
        int32_t val = 0;
        val |= (buf.arrVal->elements[ptr].toInt() & 0xFF);
        val |= (buf.arrVal->elements[ptr+1].toInt() & 0xFF) << 8;
        val |= (buf.arrVal->elements[ptr+2].toInt() & 0xFF) << 16;
        val |= (buf.arrVal->elements[ptr+3].toInt() & 0xFF) << 24;
        return Value::makeInt(val);
    });

    // WasmMemory.writeInt32(memory, ptr, value) — write a 32-bit integer to memory
    vm.registerNative("WasmMemory.writeInt32", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeInt32: requires memory, pointer, value"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeInt32: out of bounds (ptr=" + std::to_string(ptr) + ", size=" + std::to_string(size) + ")"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        int32_t val = a[2].toInt();
        buf.arrVal->elements[ptr]   = Value::makeInt(val & 0xFF);
        buf.arrVal->elements[ptr+1] = Value::makeInt((val >> 8) & 0xFF);
        buf.arrVal->elements[ptr+2] = Value::makeInt((val >> 16) & 0xFF);
        buf.arrVal->elements[ptr+3] = Value::makeInt((val >> 24) & 0xFF);
        return Value::makeBool(true);
    });

    // WasmMemory.readFloat64(memory, ptr) — read a 64-bit float from memory
    vm.registerNative("WasmMemory.readFloat64", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.readFloat64: requires memory and pointer"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 8 > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.readFloat64: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeDouble(0);
        uint8_t bytes[8];
        for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)buf.arrVal->elements[ptr+i].toInt();
        double val; std::memcpy(&val, bytes, 8);
        return Value::makeDouble(val);
    });

    // WasmMemory.writeFloat64(memory, ptr, value) — write a 64-bit float to memory
    vm.registerNative("WasmMemory.writeFloat64", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeFloat64: requires memory, pointer, value"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 8 > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeFloat64: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        double val = a[2].toDouble();
        uint8_t bytes[8]; std::memcpy(bytes, &val, 8);
        for (int i = 0; i < 8; i++) buf.arrVal->elements[ptr+i] = Value::makeInt(bytes[i]);
        return Value::makeBool(true);
    });

    // WasmMemory.readByte(memory, ptr) — read a single byte
    vm.registerNative("WasmMemory.readByte", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.readByte: requires memory and pointer"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr >= size) { vm.throwError("GardWasmMemoryError", "WasmMemory.readByte: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        return (buf.arrVal) ? buf.arrVal->elements[ptr] : Value::makeInt(0);
    });

    // WasmMemory.writeByte(memory, ptr, value) — write a single byte
    vm.registerNative("WasmMemory.writeByte", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeByte: requires memory, pointer, value"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr >= size) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeByte: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (buf.arrVal) buf.arrVal->elements[ptr] = Value::makeInt(a[2].toInt() & 0xFF);
        return Value::makeBool(true);
    });

    // WasmMemory.writeArray(memory, ptr, data) — write an array of bytes/ints to memory
    vm.registerNative("WasmMemory.writeArray", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeArray: requires memory, pointer, data array"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (!a[2].arrVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeArray: data must be an array"); return Value::makeNull(); }
        int dataLen = (int)a[2].arrVal->elements.size();
        if (ptr < 0 || ptr + dataLen > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeArray: out of bounds (ptr=" + std::to_string(ptr) + ", len=" + std::to_string(dataLen) + ", memSize=" + std::to_string(size) + ")"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        for (int i = 0; i < dataLen; i++) {
            buf.arrVal->elements[ptr + i] = Value::makeInt(a[2].arrVal->elements[i].toInt() & 0xFF);
        }
        return Value::makeInt(dataLen);
    });

    // WasmMemory.readArray(memory, ptr, length) — read an array of bytes from memory
    vm.registerNative("WasmMemory.readArray", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.readArray: requires memory, pointer, length"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int length = a[2].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + length > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.readArray: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        Value result = Value::makeArray();
        if (buf.arrVal) {
            for (int i = 0; i < length; i++) {
                result.arrVal->elements.push_back(buf.arrVal->elements[ptr + i]);
            }
        }
        return result;
    });

    // WasmMemory.writeString(memory, ptr, str) — write a string to memory (UTF-8 bytes)
    vm.registerNative("WasmMemory.writeString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeString: requires memory, pointer, string"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        std::string str = a[2].toString();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + (int)str.size() > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.writeString: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        for (size_t i = 0; i < str.size(); i++) {
            buf.arrVal->elements[ptr + i] = Value::makeInt((uint8_t)str[i]);
        }
        Value result = Value::makeObject("WasmString");
        result.objVal->fields["ptr"] = Value::makeInt(ptr);
        result.objVal->fields["len"] = Value::makeInt((int)str.size());
        return result;
    });

    // WasmMemory.readString(memory, ptr, length) — read a string from memory
    vm.registerNative("WasmMemory.readString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.readString: requires memory, pointer, length"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int length = a[2].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + length > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.readString: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeString("");
        std::string result;
        for (int i = 0; i < length; i++) {
            result += (char)buf.arrVal->elements[ptr + i].toInt();
        }
        return Value::makeString(result);
    });

    // WasmMemory.allocate(memory, size) — bump-allocate from the memory's heap pointer
    vm.registerNative("WasmMemory.allocate", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.allocate: requires memory and size"); return Value::makeNull(); }
        int allocSize = a[1].toInt();
        if (allocSize <= 0) { vm.throwError("GardWasmMemoryError", "WasmMemory.allocate: size must be > 0"); return Value::makeNull(); }
        auto heapIt = a[0].objVal->fields.find("_heapPtr");
        int heapPtr = (heapIt != a[0].objVal->fields.end()) ? heapIt->second.toInt() : 0;
        int memSize = a[0].objVal->fields["_size"].toInt();
        // Align to 8 bytes
        while (heapPtr % 8 != 0) heapPtr++;
        if (heapPtr + allocSize > memSize) {
            vm.throwError("GardWasmMemoryError", "WasmMemory.allocate: out of memory (need " + std::to_string(allocSize) + " bytes, available=" + std::to_string(memSize - heapPtr) + ")");
            return Value::makeNull();
        }
        int ptr = heapPtr;
        a[0].objVal->fields["_heapPtr"] = Value::makeInt(heapPtr + allocSize);
        return Value::makeInt(ptr);
    });

    // WasmMemory.free(memory, ptr) — mark memory as freed (simplified: no-op for bump allocator)
    vm.registerNative("WasmMemory.free", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.free: requires memory and pointer"); return Value::makeNull(); }
        // Bump allocator doesn't support individual frees — this is a no-op
        // A real implementation would use a free list or arena allocator
        return Value::makeBool(true);
    });

    // WasmMemory.fill(memory, ptr, value, length) — fill memory region with a byte value
    vm.registerNative("WasmMemory.fill", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.fill: requires memory, ptr, value, length"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int val = a[2].toInt() & 0xFF;
        int length = a[3].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + length > size) { vm.throwError("GardWasmMemoryError", "WasmMemory.fill: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (buf.arrVal) {
            for (int i = 0; i < length; i++) buf.arrVal->elements[ptr + i] = Value::makeInt(val);
        }
        return Value::makeBool(true);
    });

    // WasmMemory.copy(memory, destPtr, srcPtr, length) — copy memory region
    vm.registerNative("WasmMemory.copy", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardWasmMemoryError", "WasmMemory.copy: requires memory, destPtr, srcPtr, length"); return Value::makeNull(); }
        int dest = a[1].toInt();
        int src = a[2].toInt();
        int length = a[3].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (dest < 0 || dest + length > size || src < 0 || src + length > size) {
            vm.throwError("GardWasmMemoryError", "WasmMemory.copy: out of bounds");
            return Value::makeNull();
        }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        // Handle overlapping regions (copy to temp first)
        std::vector<Value> temp(length);
        for (int i = 0; i < length; i++) temp[i] = buf.arrVal->elements[src + i];
        for (int i = 0; i < length; i++) buf.arrVal->elements[dest + i] = temp[i];
        return Value::makeBool(true);
    });

    // WasmMemory.size(memory) — get current size in bytes
    vm.registerNative("WasmMemory.size", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        return a[0].objVal->fields["_size"];
    });

    // WasmModule.fromClass(className, options?) — compile a @WasmModule annotated class to Wasm
    // This reads class annotations and builds the module automatically
    vm.registerNative("WasmModule.fromClass", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardWasmError", "WasmModule.fromClass: requires class name"); return Value::makeNull(); }
        // This is a high-level API that would be used with @WasmModule annotation
        // For now, create a module and return it — the annotation processing happens at compile time
        std::string className = a[0].toString();
        auto mod = std::make_shared<WasmModule>();
        mod->hasMemory = true;
        mod->memoryMin = 1;
        mod->memoryMax = 256;

        if (a.size() >= 2 && a[1].type == ValueType::Object && a[1].objVal) {
            auto memIt = a[1].objVal->fields.find("memory");
            if (memIt != a[1].objVal->fields.end() && memIt->second.objVal) {
                auto minIt = memIt->second.objVal->fields.find("min");
                auto maxIt = memIt->second.objVal->fields.find("max");
                if (minIt != memIt->second.objVal->fields.end()) mod->memoryMin = minIt->second.toInt();
                if (maxIt != memIt->second.objVal->fields.end()) mod->memoryMax = maxIt->second.toInt();
            }
        }

        WasmGlobal heapPtrGlobal;
        heapPtrGlobal.type = WASM_I32;
        heapPtrGlobal.mutable_ = true;
        heapPtrGlobal.initExpr.push_back(OP_I32_CONST);
        encodeLEB128_s(heapPtrGlobal.initExpr, (int32_t)mod->nextStringOffset);
        mod->globals.push_back(heapPtrGlobal);

        int id = g_nextWasmId++;
        g_wasmModules[id] = mod;
        g_wasmFuncNames[id] = {};
        g_wasmParamNames[id] = {};

        Value modObj = Value::makeObject("WasmModule");
        modObj.objVal->fields["_wasmId"] = Value::makeInt(id);
        modObj.objVal->fields["className"] = Value::makeString(className);
        modObj.objVal->fields["memoryMin"] = Value::makeInt(mod->memoryMin);
        modObj.objVal->fields["memoryMax"] = Value::makeInt(mod->memoryMax);
        return modObj;
    });

    // WasmModule.instantiate(binary, imports?) — instantiate a compiled wasm module (host-side)
    // Returns an instance object with exported functions callable from Gard
    vm.registerNative("WasmModule.instantiate", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardWasmError", "WasmModule.instantiate: requires binary (byte array)"); return Value::makeNull(); }
        // Validate it's a wasm binary
        if (a[0].type != ValueType::Array || !a[0].arrVal || a[0].arrVal->elements.size() < 8) {
            vm.throwError("GardWasmError", "WasmModule.instantiate: invalid binary format");
            return Value::makeNull();
        }
        // Check magic bytes
        auto& elems = a[0].arrVal->elements;
        if (elems[0].toInt() != 0x00 || elems[1].toInt() != 0x61 ||
            elems[2].toInt() != 0x73 || elems[3].toInt() != 0x6D) {
            vm.throwError("GardWasmError", "WasmModule.instantiate: not a valid .wasm binary (bad magic)");
            return Value::makeNull();
        }

        Value instance = Value::makeObject("WasmInstance");
        instance.objVal->fields["valid"] = Value::makeBool(true);
        instance.objVal->fields["binarySize"] = Value::makeInt((int)elems.size());

        // Parse imports if provided
        if (a.size() >= 2 && a[1].type == ValueType::Object && a[1].objVal) {
            instance.objVal->fields["imports"] = a[1];
        }

        return instance;
    });

    // ===== 7.9 WebAssembly DOM Interop =====

    // --- Handle Table: tracks object references between Wasm and host ---
    // Each DOM element/object gets a unique handle (integer ID) that can be passed through Wasm i32

    // WasmInterop.createHandleTable() — create a handle table for object reference tracking
    vm.registerNative("WasmInterop.createHandleTable", [](const std::vector<Value>& a) -> Value {
        Value table = Value::makeObject("HandleTable");
        table.objVal->fields["_nextHandle"] = Value::makeInt(1); // 0 = null handle
        table.objVal->fields["_handles"] = Value::makeObject("Handles");
        table.objVal->fields["_freeList"] = Value::makeArray();
        table.objVal->fields["size"] = Value::makeInt(0);
        return table;
    });

    // WasmInterop.storeHandle(table, object) — store an object, returns integer handle
    vm.registerNative("WasmInterop.storeHandle", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.storeHandle: requires handle table and object"); return Value::makeNull(); }
        auto& freeList = a[0].objVal->fields["_freeList"];
        int handle;
        // Reuse freed handles if available
        if (freeList.arrVal && !freeList.arrVal->elements.empty()) {
            handle = freeList.arrVal->elements.back().toInt();
            freeList.arrVal->elements.pop_back();
        } else {
            handle = a[0].objVal->fields["_nextHandle"].toInt();
            a[0].objVal->fields["_nextHandle"] = Value::makeInt(handle + 1);
        }
        auto& handles = a[0].objVal->fields["_handles"];
        if (handles.objVal) handles.objVal->fields[std::to_string(handle)] = a[1];
        int sz = a[0].objVal->fields["size"].toInt();
        a[0].objVal->fields["size"] = Value::makeInt(sz + 1);
        return Value::makeInt(handle);
    });

    // WasmInterop.getHandle(table, handle) — retrieve object by handle
    vm.registerNative("WasmInterop.getHandle", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.getHandle: requires handle table and handle ID"); return Value::makeNull(); }
        int handle = a[1].toInt();
        if (handle <= 0) { vm.throwError("GardWasmInteropError", "WasmInterop.getHandle: invalid handle (0 = null)"); return Value::makeNull(); }
        auto& handles = a[0].objVal->fields["_handles"];
        if (!handles.objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.getHandle: corrupted handle table"); return Value::makeNull(); }
        auto it = handles.objVal->fields.find(std::to_string(handle));
        if (it == handles.objVal->fields.end()) { vm.throwError("GardWasmInteropError", "WasmInterop.getHandle: handle " + std::to_string(handle) + " not found (freed or invalid)"); return Value::makeNull(); }
        return it->second;
    });

    // WasmInterop.releaseHandle(table, handle) — release a handle (free the slot)
    vm.registerNative("WasmInterop.releaseHandle", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.releaseHandle: requires handle table and handle ID"); return Value::makeNull(); }
        int handle = a[1].toInt();
        auto& handles = a[0].objVal->fields["_handles"];
        if (!handles.objVal) return Value::makeBool(false);
        auto it = handles.objVal->fields.find(std::to_string(handle));
        if (it == handles.objVal->fields.end()) { vm.throwError("GardWasmInteropError", "WasmInterop.releaseHandle: handle " + std::to_string(handle) + " not found"); return Value::makeNull(); }
        handles.objVal->fields.erase(it);
        auto& freeList = a[0].objVal->fields["_freeList"];
        if (freeList.arrVal) freeList.arrVal->elements.push_back(Value::makeInt(handle));
        int sz = a[0].objVal->fields["size"].toInt();
        a[0].objVal->fields["size"] = Value::makeInt(sz - 1);
        return Value::makeBool(true);
    });

    // --- DOM Element API (generates Wasm import bindings for browser DOM) ---

    // DOM.createElement(tag) — create a virtual DOM element
    vm.registerNative("DOM.createElement", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string tag = a[0].toString();
        Value elem = Value::makeObject("Element");
        elem.objVal->fields["tag"] = Value::makeString(tag);
        elem.objVal->fields["attributes"] = Value::makeObject("Attributes");
        elem.objVal->fields["children"] = Value::makeArray();
        elem.objVal->fields["events"] = Value::makeObject("Events");
        elem.objVal->fields["text"] = Value::makeString("");
        elem.objVal->fields["id"] = Value::makeString("");
        elem.objVal->fields["className"] = Value::makeString("");
        elem.objVal->fields["style"] = Value::makeObject("Style");
        elem.objVal->fields["_handle"] = Value::makeInt(0);
        return elem;
    });

    // DOM.setAttribute(element, name, value) — set an attribute on an element
    vm.registerNative("DOM.setAttribute", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.setAttribute: requires element, name, value"); return Value::makeNull(); }
        auto& attrs = a[0].objVal->fields["attributes"];
        if (attrs.objVal) attrs.objVal->fields[a[1].toString()] = a[2];
        // Special attributes
        std::string name = a[1].toString();
        if (name == "id") a[0].objVal->fields["id"] = a[2];
        else if (name == "class") a[0].objVal->fields["className"] = a[2];
        return a[0];
    });

    // DOM.getAttribute(element, name) — get an attribute value
    vm.registerNative("DOM.getAttribute", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.getAttribute: requires element and name"); return Value::makeNull(); }
        auto& attrs = a[0].objVal->fields["attributes"];
        if (!attrs.objVal) return Value::makeNull();
        auto it = attrs.objVal->fields.find(a[1].toString());
        return (it != attrs.objVal->fields.end()) ? it->second : Value::makeNull();
    });

    // DOM.removeAttribute(element, name) — remove an attribute
    vm.registerNative("DOM.removeAttribute", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.removeAttribute: requires element and name"); return Value::makeNull(); }
        auto& attrs = a[0].objVal->fields["attributes"];
        if (attrs.objVal) attrs.objVal->fields.erase(a[1].toString());
        return a[0];
    });

    // DOM.appendChild(parent, child) — append a child element
    vm.registerNative("DOM.appendChild", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.appendChild: requires parent and child elements"); return Value::makeNull(); }
        auto& children = a[0].objVal->fields["children"];
        if (children.arrVal) children.arrVal->elements.push_back(a[1]);
        return a[0];
    });

    // DOM.removeChild(parent, index) — remove child at index
    vm.registerNative("DOM.removeChild", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.removeChild: requires parent and child index"); return Value::makeNull(); }
        auto& children = a[0].objVal->fields["children"];
        if (!children.arrVal) return Value::makeNull();
        int idx = a[1].toInt();
        if (idx < 0 || idx >= (int)children.arrVal->elements.size()) {
            vm.throwError("GardDOMError", "DOM.removeChild: index " + std::to_string(idx) + " out of bounds");
            return Value::makeNull();
        }
        Value removed = children.arrVal->elements[idx];
        children.arrVal->elements.erase(children.arrVal->elements.begin() + idx);
        return removed;
    });

    // DOM.insertBefore(parent, newChild, referenceIndex) — insert before a specific child
    vm.registerNative("DOM.insertBefore", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.insertBefore: requires parent, child, referenceIndex"); return Value::makeNull(); }
        auto& children = a[0].objVal->fields["children"];
        if (!children.arrVal) return Value::makeNull();
        int idx = a[2].toInt();
        if (idx < 0) idx = 0;
        if (idx >= (int)children.arrVal->elements.size()) {
            children.arrVal->elements.push_back(a[1]);
        } else {
            children.arrVal->elements.insert(children.arrVal->elements.begin() + idx, a[1]);
        }
        return a[0];
    });

    // DOM.setTextContent(element, text) — set text content
    vm.registerNative("DOM.setTextContent", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.setTextContent: requires element and text"); return Value::makeNull(); }
        a[0].objVal->fields["text"] = Value::makeString(a[1].toString());
        return a[0];
    });

    // DOM.getTextContent(element) — get text content
    vm.registerNative("DOM.getTextContent", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        return a[0].objVal->fields["text"];
    });

    // DOM.setStyle(element, property, value) — set a CSS style property
    vm.registerNative("DOM.setStyle", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.setStyle: requires element, property, value"); return Value::makeNull(); }
        auto& style = a[0].objVal->fields["style"];
        if (style.objVal) style.objVal->fields[a[1].toString()] = a[2];
        return a[0];
    });

    // DOM.getStyle(element, property) — get a CSS style property
    vm.registerNative("DOM.getStyle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& style = a[0].objVal->fields["style"];
        if (!style.objVal) return Value::makeNull();
        auto it = style.objVal->fields.find(a[1].toString());
        return (it != style.objVal->fields.end()) ? it->second : Value::makeNull();
    });

    // DOM.addEventListener(element, event, handlerName) — register an event listener
    // handlerName is stored; in real Wasm, this maps to a callback function index
    vm.registerNative("DOM.addEventListener", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.addEventListener: requires element, event, handler"); return Value::makeNull(); }
        std::string event = a[1].toString();
        auto& events = a[0].objVal->fields["events"];
        if (!events.objVal) { a[0].objVal->fields["events"] = Value::makeObject("Events"); events = a[0].objVal->fields["events"]; }
        // Store handlers as array per event type
        auto it = events.objVal->fields.find(event);
        if (it == events.objVal->fields.end()) {
            events.objVal->fields[event] = Value::makeArray();
        }
        events.objVal->fields[event].arrVal->elements.push_back(a[2]);
        return a[0];
    });

    // DOM.removeEventListener(element, event, handlerName) — remove an event listener
    vm.registerNative("DOM.removeEventListener", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.removeEventListener: requires element, event, handler"); return Value::makeNull(); }
        std::string event = a[1].toString();
        std::string handler = a[2].toString();
        auto& events = a[0].objVal->fields["events"];
        if (!events.objVal) return a[0];
        auto it = events.objVal->fields.find(event);
        if (it != events.objVal->fields.end() && it->second.arrVal) {
            auto& handlers = it->second.arrVal->elements;
            handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                [&handler](const Value& v) { return v.toString() == handler; }), handlers.end());
        }
        return a[0];
    });

    // DOM.dispatchEvent(element, event, data?) — trigger an event on an element
    vm.registerNative("DOM.dispatchEvent", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.dispatchEvent: requires element and event name"); return Value::makeNull(); }
        std::string event = a[1].toString();
        auto& events = a[0].objVal->fields["events"];
        if (!events.objVal) return Value::makeArray();
        auto it = events.objVal->fields.find(event);
        Value result = Value::makeArray();
        if (it != events.objVal->fields.end() && it->second.arrVal) {
            for (auto& handler : it->second.arrVal->elements) {
                result.arrVal->elements.push_back(handler);
            }
        }
        return result; // returns list of handler names to invoke
    });

    // DOM.querySelector(root, selector) — find first matching child by tag/id/class
    vm.registerNative("DOM.querySelector", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.querySelector: requires root element and selector"); return Value::makeNull(); }
        std::string selector = a[1].toString();
        // Simple selector matching: tag, #id, .class
        std::function<Value(const Value&)> search = [&](const Value& elem) -> Value {
            if (!elem.objVal) return Value::makeNull();
            // Check current element
            if (selector[0] == '#') {
                if (elem.objVal->fields["id"].toString() == selector.substr(1)) return elem;
            } else if (selector[0] == '.') {
                std::string cls = elem.objVal->fields["className"].toString();
                if (cls.find(selector.substr(1)) != std::string::npos) return elem;
            } else {
                if (elem.objVal->fields["tag"].toString() == selector) return elem;
            }
            // Search children
            auto& children = elem.objVal->fields["children"];
            if (children.arrVal) {
                for (auto& child : children.arrVal->elements) {
                    Value found = search(child);
                    if (found.type != ValueType::Null) return found;
                }
            }
            return Value::makeNull();
        };
        // Search in children of root
        auto& children = a[0].objVal->fields["children"];
        if (children.arrVal) {
            for (auto& child : children.arrVal->elements) {
                Value found = search(child);
                if (found.type != ValueType::Null) return found;
            }
        }
        return Value::makeNull();
    });

    // DOM.querySelectorAll(root, selector) — find all matching children
    vm.registerNative("DOM.querySelectorAll", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.querySelectorAll: requires root element and selector"); return Value::makeNull(); }
        std::string selector = a[1].toString();
        Value results = Value::makeArray();
        std::function<void(const Value&)> search = [&](const Value& elem) {
            if (!elem.objVal) return;
            bool match = false;
            if (selector[0] == '#') { match = (elem.objVal->fields["id"].toString() == selector.substr(1)); }
            else if (selector[0] == '.') { match = (elem.objVal->fields["className"].toString().find(selector.substr(1)) != std::string::npos); }
            else { match = (elem.objVal->fields["tag"].toString() == selector); }
            if (match) results.arrVal->elements.push_back(elem);
            auto& children = elem.objVal->fields["children"];
            if (children.arrVal) { for (auto& child : children.arrVal->elements) search(child); }
        };
        auto& children = a[0].objVal->fields["children"];
        if (children.arrVal) { for (auto& child : children.arrVal->elements) search(child); }
        return results;
    });

    // DOM.render(element) — serialize element tree to HTML string
    vm.registerNative("DOM.render", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardDOMError", "DOM.render: requires element"); return Value::makeNull(); }
        std::function<std::string(const Value&, int)> renderElem = [&](const Value& elem, int depth) -> std::string {
            if (!elem.objVal) return "";
            std::string tag = elem.objVal->fields["tag"].toString();
            std::string indent(depth * 2, ' ');
            std::string html = indent + "<" + tag;
            // Attributes
            auto& attrs = elem.objVal->fields["attributes"];
            if (attrs.objVal) {
                for (auto& [k, v] : attrs.objVal->fields) {
                    html += " " + k + "=\"" + v.toString() + "\"";
                }
            }
            // Style
            auto& style = elem.objVal->fields["style"];
            if (style.objVal && !style.objVal->fields.empty()) {
                html += " style=\"";
                bool first = true;
                for (auto& [k, v] : style.objVal->fields) {
                    if (!first) html += "; ";
                    html += k + ": " + v.toString();
                    first = false;
                }
                html += "\"";
            }
            html += ">";
            // Text content
            std::string text = elem.objVal->fields["text"].toString();
            if (!text.empty()) html += text;
            // Children
            auto& children = elem.objVal->fields["children"];
            if (children.arrVal && !children.arrVal->elements.empty()) {
                html += "\n";
                for (auto& child : children.arrVal->elements) {
                    html += renderElem(child, depth + 1) + "\n";
                }
                html += indent;
            }
            html += "</" + tag + ">";
            return html;
        };
        return Value::makeString(renderElem(a[0], 0));
    });

    // DOM.createTextNode(text) — create a text node element
    vm.registerNative("DOM.createTextNode", [](const std::vector<Value>& a) -> Value {
        Value node = Value::makeObject("Element");
        node.objVal->fields["tag"] = Value::makeString("#text");
        node.objVal->fields["text"] = Value::makeString(a.empty() ? "" : a[0].toString());
        node.objVal->fields["attributes"] = Value::makeObject("Attributes");
        node.objVal->fields["children"] = Value::makeArray();
        node.objVal->fields["events"] = Value::makeObject("Events");
        node.objVal->fields["id"] = Value::makeString("");
        node.objVal->fields["className"] = Value::makeString("");
        node.objVal->fields["style"] = Value::makeObject("Style");
        return node;
    });

    // DOM.cloneElement(element, deep?) — clone an element (optionally deep)
    vm.registerNative("DOM.cloneElement", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        bool deep = a.size() >= 2 ? a[1].toBool() : false;
        Value clone = Value::makeObject("Element");
        // Copy all fields
        for (auto& [k, v] : a[0].objVal->fields) {
            if (k == "children" && !deep) {
                clone.objVal->fields["children"] = Value::makeArray();
            } else if (k == "events") {
                clone.objVal->fields["events"] = Value::makeObject("Events"); // events not cloned
            } else {
                clone.objVal->fields[k] = v;
            }
        }
        return clone;
    });

    // --- String Marshaling for Wasm ↔ JS interop ---

    // WasmInterop.marshalString(memory, str) — write string to wasm memory, returns {ptr, len}
    vm.registerNative("WasmInterop.marshalString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.marshalString: requires memory and string"); return Value::makeNull(); }
        std::string str = a[1].toString();
        int size = a[0].objVal->fields["_size"].toInt();
        // Allocate from heap
        auto heapIt = a[0].objVal->fields.find("_heapPtr");
        int heapPtr = (heapIt != a[0].objVal->fields.end()) ? heapIt->second.toInt() : 0;
        while (heapPtr % 4 != 0) heapPtr++;
        if (heapPtr + (int)str.size() + 4 > size) {
            vm.throwError("GardWasmInteropError", "WasmInterop.marshalString: out of memory");
            return Value::makeNull();
        }
        // Write length prefix (4 bytes LE) + string bytes
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        int ptr = heapPtr;
        int32_t len = (int32_t)str.size();
        buf.arrVal->elements[ptr]   = Value::makeInt(len & 0xFF);
        buf.arrVal->elements[ptr+1] = Value::makeInt((len >> 8) & 0xFF);
        buf.arrVal->elements[ptr+2] = Value::makeInt((len >> 16) & 0xFF);
        buf.arrVal->elements[ptr+3] = Value::makeInt((len >> 24) & 0xFF);
        for (size_t i = 0; i < str.size(); i++) {
            buf.arrVal->elements[ptr + 4 + i] = Value::makeInt((uint8_t)str[i]);
        }
        a[0].objVal->fields["_heapPtr"] = Value::makeInt(ptr + 4 + (int)str.size());
        Value result = Value::makeObject("MarshaledString");
        result.objVal->fields["ptr"] = Value::makeInt(ptr + 4); // data starts after length
        result.objVal->fields["len"] = Value::makeInt(len);
        result.objVal->fields["headerPtr"] = Value::makeInt(ptr);
        return result;
    });

    // WasmInterop.unmarshalString(memory, ptr, len) — read string from wasm memory
    vm.registerNative("WasmInterop.unmarshalString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.unmarshalString: requires memory, ptr, len"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int len = a[2].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + len > size) { vm.throwError("GardWasmInteropError", "WasmInterop.unmarshalString: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeString("");
        std::string result;
        for (int i = 0; i < len; i++) result += (char)buf.arrVal->elements[ptr + i].toInt();
        return Value::makeString(result);
    });

    // WasmInterop.generateImports(module, bindings) — generate Wasm import section from binding config
    // bindings: [{module: "env", name: "log", params: ["i32"], results: []}, ...]
    vm.registerNative("WasmInterop.generateImports", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.generateImports: requires module and bindings array"); return Value::makeNull(); }
        if (!a[1].arrVal) { vm.throwError("GardWasmInteropError", "WasmInterop.generateImports: bindings must be an array"); return Value::makeNull(); }
        int count = 0;
        for (auto& binding : a[1].arrVal->elements) {
            std::string modName, funcName;
            Value params = Value::makeArray(), results = Value::makeArray();
            if (binding.type == ValueType::Object && binding.objVal) {
                modName = binding.objVal->fields["module"].toString();
                funcName = binding.objVal->fields["name"].toString();
                if (binding.objVal->fields.count("params")) params = binding.objVal->fields["params"];
                if (binding.objVal->fields.count("results")) results = binding.objVal->fields["results"];
            } else if (binding.type == ValueType::Map && binding.mapVal) {
                auto mIt = binding.mapVal->entries.find("module");
                auto nIt = binding.mapVal->entries.find("name");
                auto pIt = binding.mapVal->entries.find("params");
                auto rIt = binding.mapVal->entries.find("results");
                if (mIt != binding.mapVal->entries.end()) modName = mIt->second.toString();
                if (nIt != binding.mapVal->entries.end()) funcName = nIt->second.toString();
                if (pIt != binding.mapVal->entries.end()) params = pIt->second;
                if (rIt != binding.mapVal->entries.end()) results = rIt->second;
            } else { continue; }
            if (modName.empty() || funcName.empty()) continue;
            std::vector<Value> importArgs = {a[0], Value::makeString(modName), Value::makeString(funcName), params, results};
            vm.callNative("Wasm.addImport", importArgs);
            count++;
        }
        Value result = Value::makeObject("ImportResult");
        result.objVal->fields["count"] = Value::makeInt(count);
        return result;
    });

    // WasmInterop.generateJSGlue(module) — generate JavaScript glue code for instantiation
    vm.registerNative("WasmInterop.generateJSGlue", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmInteropError", "WasmInterop.generateJSGlue: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmInteropError", "WasmInterop.generateJSGlue: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        std::string js = "// Auto-generated Wasm glue code\n";
        js += "const importObject = {\n";
        // Group imports by module
        std::unordered_map<std::string, std::vector<std::string>> importsByModule;
        for (auto& imp : mod->imports) {
            if (imp.kind == 0) importsByModule[imp.module].push_back(imp.name);
        }
        for (auto& [modName, funcs] : importsByModule) {
            js += "  \"" + modName + "\": {\n";
            for (auto& fn : funcs) {
                js += "    \"" + fn + "\": (...args) => { /* TODO: implement */ },\n";
            }
            js += "  },\n";
        }
        js += "};\n\n";
        js += "async function loadWasm(wasmPath) {\n";
        js += "  const response = await fetch(wasmPath);\n";
        js += "  const bytes = await response.arrayBuffer();\n";
        js += "  const { instance } = await WebAssembly.instantiate(bytes, importObject);\n";
        js += "  return instance.exports;\n";
        js += "}\n";
        return Value::makeString(js);
    });

    // --- Canvas API bindings (virtual canvas for Wasm rendering) ---

    // Canvas.create(width, height) — create a virtual canvas
    vm.registerNative("Canvas.create", [](const std::vector<Value>& a) -> Value {
        int width = a.size() >= 1 ? a[0].toInt() : 800;
        int height = a.size() >= 2 ? a[1].toInt() : 600;
        Value canvas = Value::makeObject("Canvas");
        canvas.objVal->fields["width"] = Value::makeInt(width);
        canvas.objVal->fields["height"] = Value::makeInt(height);
        canvas.objVal->fields["_commands"] = Value::makeArray(); // draw command buffer
        canvas.objVal->fields["fillStyle"] = Value::makeString("#000000");
        canvas.objVal->fields["strokeStyle"] = Value::makeString("#000000");
        canvas.objVal->fields["lineWidth"] = Value::makeInt(1);
        canvas.objVal->fields["font"] = Value::makeString("16px sans-serif");
        return canvas;
    });

    // Canvas.fillRect(canvas, x, y, w, h)
    vm.registerNative("Canvas.fillRect", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) { vm.throwError("GardDOMError", "Canvas.fillRect: requires canvas, x, y, width, height"); return Value::makeNull(); }
        auto& cmds = a[0].objVal->fields["_commands"];
        if (!cmds.arrVal) return Value::makeNull();
        Value cmd = Value::makeObject("DrawCommand");
        cmd.objVal->fields["type"] = Value::makeString("fillRect");
        cmd.objVal->fields["x"] = a[1]; cmd.objVal->fields["y"] = a[2];
        cmd.objVal->fields["w"] = a[3]; cmd.objVal->fields["h"] = a[4];
        cmd.objVal->fields["fill"] = a[0].objVal->fields["fillStyle"];
        cmds.arrVal->elements.push_back(cmd);
        return a[0];
    });

    // Canvas.strokeRect(canvas, x, y, w, h)
    vm.registerNative("Canvas.strokeRect", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) { vm.throwError("GardDOMError", "Canvas.strokeRect: requires canvas, x, y, width, height"); return Value::makeNull(); }
        auto& cmds = a[0].objVal->fields["_commands"];
        if (!cmds.arrVal) return Value::makeNull();
        Value cmd = Value::makeObject("DrawCommand");
        cmd.objVal->fields["type"] = Value::makeString("strokeRect");
        cmd.objVal->fields["x"] = a[1]; cmd.objVal->fields["y"] = a[2];
        cmd.objVal->fields["w"] = a[3]; cmd.objVal->fields["h"] = a[4];
        cmd.objVal->fields["stroke"] = a[0].objVal->fields["strokeStyle"];
        cmd.objVal->fields["lineWidth"] = a[0].objVal->fields["lineWidth"];
        cmds.arrVal->elements.push_back(cmd);
        return a[0];
    });

    // Canvas.fillText(canvas, text, x, y)
    vm.registerNative("Canvas.fillText", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardDOMError", "Canvas.fillText: requires canvas, text, x, y"); return Value::makeNull(); }
        auto& cmds = a[0].objVal->fields["_commands"];
        if (!cmds.arrVal) return Value::makeNull();
        Value cmd = Value::makeObject("DrawCommand");
        cmd.objVal->fields["type"] = Value::makeString("fillText");
        cmd.objVal->fields["text"] = a[1]; cmd.objVal->fields["x"] = a[2]; cmd.objVal->fields["y"] = a[3];
        cmd.objVal->fields["fill"] = a[0].objVal->fields["fillStyle"];
        cmd.objVal->fields["font"] = a[0].objVal->fields["font"];
        cmds.arrVal->elements.push_back(cmd);
        return a[0];
    });

    // Canvas.clearRect(canvas, x, y, w, h)
    vm.registerNative("Canvas.clearRect", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) { vm.throwError("GardDOMError", "Canvas.clearRect: requires canvas, x, y, width, height"); return Value::makeNull(); }
        auto& cmds = a[0].objVal->fields["_commands"];
        if (!cmds.arrVal) return Value::makeNull();
        Value cmd = Value::makeObject("DrawCommand");
        cmd.objVal->fields["type"] = Value::makeString("clearRect");
        cmd.objVal->fields["x"] = a[1]; cmd.objVal->fields["y"] = a[2];
        cmd.objVal->fields["w"] = a[3]; cmd.objVal->fields["h"] = a[4];
        cmds.arrVal->elements.push_back(cmd);
        return a[0];
    });

    // Canvas.drawLine(canvas, x1, y1, x2, y2)
    vm.registerNative("Canvas.drawLine", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) { vm.throwError("GardDOMError", "Canvas.drawLine: requires canvas, x1, y1, x2, y2"); return Value::makeNull(); }
        auto& cmds = a[0].objVal->fields["_commands"];
        if (!cmds.arrVal) return Value::makeNull();
        Value cmd = Value::makeObject("DrawCommand");
        cmd.objVal->fields["type"] = Value::makeString("line");
        cmd.objVal->fields["x1"] = a[1]; cmd.objVal->fields["y1"] = a[2];
        cmd.objVal->fields["x2"] = a[3]; cmd.objVal->fields["y2"] = a[4];
        cmd.objVal->fields["stroke"] = a[0].objVal->fields["strokeStyle"];
        cmd.objVal->fields["lineWidth"] = a[0].objVal->fields["lineWidth"];
        cmds.arrVal->elements.push_back(cmd);
        return a[0];
    });

    // Canvas.drawCircle(canvas, cx, cy, radius)
    vm.registerNative("Canvas.drawCircle", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardDOMError", "Canvas.drawCircle: requires canvas, cx, cy, radius"); return Value::makeNull(); }
        auto& cmds = a[0].objVal->fields["_commands"];
        if (!cmds.arrVal) return Value::makeNull();
        Value cmd = Value::makeObject("DrawCommand");
        cmd.objVal->fields["type"] = Value::makeString("circle");
        cmd.objVal->fields["cx"] = a[1]; cmd.objVal->fields["cy"] = a[2]; cmd.objVal->fields["r"] = a[3];
        cmd.objVal->fields["fill"] = a[0].objVal->fields["fillStyle"];
        cmd.objVal->fields["stroke"] = a[0].objVal->fields["strokeStyle"];
        cmds.arrVal->elements.push_back(cmd);
        return a[0];
    });

    // Canvas.getCommands(canvas) — get the draw command buffer (for serialization to JS)
    vm.registerNative("Canvas.getCommands", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        return a[0].objVal->fields["_commands"];
    });

    // Canvas.clear(canvas) — clear the command buffer
    vm.registerNative("Canvas.clear", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_commands"] = Value::makeArray();
        return a[0];
    });

    // Canvas.setFillStyle(canvas, color)
    vm.registerNative("Canvas.setFillStyle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["fillStyle"] = a[1];
        return a[0];
    });

    // Canvas.setStrokeStyle(canvas, color)
    vm.registerNative("Canvas.setStrokeStyle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["strokeStyle"] = a[1];
        return a[0];
    });

    // Canvas.setLineWidth(canvas, width)
    vm.registerNative("Canvas.setLineWidth", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["lineWidth"] = a[1];
        return a[0];
    });

    // Canvas.setFont(canvas, font)
    vm.registerNative("Canvas.setFont", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["font"] = a[1];
        return a[0];
    });

    // ===== 7.10 WebAssembly Performance: SIMD, Threads, Optimization =====

    // --- SIMD: 128-bit vector operations (Wasm SIMD proposal) ---
    // SIMD vectors are stored as 4-element arrays (Int32x4 or Float32x4)

    // SIMD.Int32x4(a, b, c, d) — create a 128-bit vector of 4 x i32
    vm.registerNative("SIMD.Int32x4", [](const std::vector<Value>& a) -> Value {
        Value v = Value::makeObject("Int32x4");
        v.objVal->fields["_type"] = Value::makeString("i32x4");
        v.objVal->fields["x"] = a.size() > 0 ? Value::makeInt(a[0].toInt()) : Value::makeInt(0);
        v.objVal->fields["y"] = a.size() > 1 ? Value::makeInt(a[1].toInt()) : Value::makeInt(0);
        v.objVal->fields["z"] = a.size() > 2 ? Value::makeInt(a[2].toInt()) : Value::makeInt(0);
        v.objVal->fields["w"] = a.size() > 3 ? Value::makeInt(a[3].toInt()) : Value::makeInt(0);
        return v;
    });

    // SIMD.Float32x4(a, b, c, d) — create a 128-bit vector of 4 x f32
    vm.registerNative("SIMD.Float32x4", [](const std::vector<Value>& a) -> Value {
        Value v = Value::makeObject("Float32x4");
        v.objVal->fields["_type"] = Value::makeString("f32x4");
        v.objVal->fields["x"] = a.size() > 0 ? Value::makeDouble(a[0].toDouble()) : Value::makeDouble(0);
        v.objVal->fields["y"] = a.size() > 1 ? Value::makeDouble(a[1].toDouble()) : Value::makeDouble(0);
        v.objVal->fields["z"] = a.size() > 2 ? Value::makeDouble(a[2].toDouble()) : Value::makeDouble(0);
        v.objVal->fields["w"] = a.size() > 3 ? Value::makeDouble(a[3].toDouble()) : Value::makeDouble(0);
        return v;
    });

    // SIMD.splat(type, value) — create vector with all lanes set to value
    vm.registerNative("SIMD.splat", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardWasmSIMDError", "SIMD.splat: requires type and value"); return Value::makeNull(); }
        std::string type = a[0].toString();
        Value v = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        v.objVal->fields["_type"] = Value::makeString(type);
        if (type == "f32x4") {
            double val = a[1].toDouble();
            v.objVal->fields["x"] = Value::makeDouble(val);
            v.objVal->fields["y"] = Value::makeDouble(val);
            v.objVal->fields["z"] = Value::makeDouble(val);
            v.objVal->fields["w"] = Value::makeDouble(val);
        } else {
            int val = a[1].toInt();
            v.objVal->fields["x"] = Value::makeInt(val);
            v.objVal->fields["y"] = Value::makeInt(val);
            v.objVal->fields["z"] = Value::makeInt(val);
            v.objVal->fields["w"] = Value::makeInt(val);
        }
        return v;
    });

    // SIMD.add(a, b) — lane-wise addition
    vm.registerNative("SIMD.add", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.add: requires two SIMD vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        if (type != a[1].objVal->fields["_type"].toString()) { vm.throwError("GardWasmSIMDError", "SIMD.add: type mismatch"); return Value::makeNull(); }
        Value r = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        r.objVal->fields["_type"] = Value::makeString(type);
        if (type == "f32x4") {
            r.objVal->fields["x"] = Value::makeDouble(a[0].objVal->fields["x"].toDouble() + a[1].objVal->fields["x"].toDouble());
            r.objVal->fields["y"] = Value::makeDouble(a[0].objVal->fields["y"].toDouble() + a[1].objVal->fields["y"].toDouble());
            r.objVal->fields["z"] = Value::makeDouble(a[0].objVal->fields["z"].toDouble() + a[1].objVal->fields["z"].toDouble());
            r.objVal->fields["w"] = Value::makeDouble(a[0].objVal->fields["w"].toDouble() + a[1].objVal->fields["w"].toDouble());
        } else {
            r.objVal->fields["x"] = Value::makeInt(a[0].objVal->fields["x"].toInt() + a[1].objVal->fields["x"].toInt());
            r.objVal->fields["y"] = Value::makeInt(a[0].objVal->fields["y"].toInt() + a[1].objVal->fields["y"].toInt());
            r.objVal->fields["z"] = Value::makeInt(a[0].objVal->fields["z"].toInt() + a[1].objVal->fields["z"].toInt());
            r.objVal->fields["w"] = Value::makeInt(a[0].objVal->fields["w"].toInt() + a[1].objVal->fields["w"].toInt());
        }
        return r;
    });

    // SIMD.sub(a, b) — lane-wise subtraction
    vm.registerNative("SIMD.sub", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.sub: requires two SIMD vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        Value r = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        r.objVal->fields["_type"] = Value::makeString(type);
        if (type == "f32x4") {
            r.objVal->fields["x"] = Value::makeDouble(a[0].objVal->fields["x"].toDouble() - a[1].objVal->fields["x"].toDouble());
            r.objVal->fields["y"] = Value::makeDouble(a[0].objVal->fields["y"].toDouble() - a[1].objVal->fields["y"].toDouble());
            r.objVal->fields["z"] = Value::makeDouble(a[0].objVal->fields["z"].toDouble() - a[1].objVal->fields["z"].toDouble());
            r.objVal->fields["w"] = Value::makeDouble(a[0].objVal->fields["w"].toDouble() - a[1].objVal->fields["w"].toDouble());
        } else {
            r.objVal->fields["x"] = Value::makeInt(a[0].objVal->fields["x"].toInt() - a[1].objVal->fields["x"].toInt());
            r.objVal->fields["y"] = Value::makeInt(a[0].objVal->fields["y"].toInt() - a[1].objVal->fields["y"].toInt());
            r.objVal->fields["z"] = Value::makeInt(a[0].objVal->fields["z"].toInt() - a[1].objVal->fields["z"].toInt());
            r.objVal->fields["w"] = Value::makeInt(a[0].objVal->fields["w"].toInt() - a[1].objVal->fields["w"].toInt());
        }
        return r;
    });

    // SIMD.mul(a, b) — lane-wise multiplication
    vm.registerNative("SIMD.mul", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.mul: requires two SIMD vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        Value r = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        r.objVal->fields["_type"] = Value::makeString(type);
        if (type == "f32x4") {
            r.objVal->fields["x"] = Value::makeDouble(a[0].objVal->fields["x"].toDouble() * a[1].objVal->fields["x"].toDouble());
            r.objVal->fields["y"] = Value::makeDouble(a[0].objVal->fields["y"].toDouble() * a[1].objVal->fields["y"].toDouble());
            r.objVal->fields["z"] = Value::makeDouble(a[0].objVal->fields["z"].toDouble() * a[1].objVal->fields["z"].toDouble());
            r.objVal->fields["w"] = Value::makeDouble(a[0].objVal->fields["w"].toDouble() * a[1].objVal->fields["w"].toDouble());
        } else {
            r.objVal->fields["x"] = Value::makeInt(a[0].objVal->fields["x"].toInt() * a[1].objVal->fields["x"].toInt());
            r.objVal->fields["y"] = Value::makeInt(a[0].objVal->fields["y"].toInt() * a[1].objVal->fields["y"].toInt());
            r.objVal->fields["z"] = Value::makeInt(a[0].objVal->fields["z"].toInt() * a[1].objVal->fields["z"].toInt());
            r.objVal->fields["w"] = Value::makeInt(a[0].objVal->fields["w"].toInt() * a[1].objVal->fields["w"].toInt());
        }
        return r;
    });

    // SIMD.div(a, b) — lane-wise division (f32x4 only)
    vm.registerNative("SIMD.div", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.div: requires two SIMD vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        if (type != "f32x4") { vm.throwError("GardWasmSIMDError", "SIMD.div: only supported for Float32x4"); return Value::makeNull(); }
        Value r = Value::makeObject("Float32x4");
        r.objVal->fields["_type"] = Value::makeString("f32x4");
        double dx = a[1].objVal->fields["x"].toDouble(), dy = a[1].objVal->fields["y"].toDouble();
        double dz = a[1].objVal->fields["z"].toDouble(), dw = a[1].objVal->fields["w"].toDouble();
        if (dx == 0 || dy == 0 || dz == 0 || dw == 0) { vm.throwError("GardWasmSIMDError", "SIMD.div: division by zero in lane"); return Value::makeNull(); }
        r.objVal->fields["x"] = Value::makeDouble(a[0].objVal->fields["x"].toDouble() / dx);
        r.objVal->fields["y"] = Value::makeDouble(a[0].objVal->fields["y"].toDouble() / dy);
        r.objVal->fields["z"] = Value::makeDouble(a[0].objVal->fields["z"].toDouble() / dz);
        r.objVal->fields["w"] = Value::makeDouble(a[0].objVal->fields["w"].toDouble() / dw);
        return r;
    });

    // SIMD.eq(a, b) — lane-wise equality comparison, returns Int32x4 mask (-1 or 0)
    vm.registerNative("SIMD.eq", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.eq: requires two SIMD vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        Value r = Value::makeObject("Int32x4");
        r.objVal->fields["_type"] = Value::makeString("i32x4");
        if (type == "f32x4") {
            r.objVal->fields["x"] = Value::makeInt(a[0].objVal->fields["x"].toDouble() == a[1].objVal->fields["x"].toDouble() ? -1 : 0);
            r.objVal->fields["y"] = Value::makeInt(a[0].objVal->fields["y"].toDouble() == a[1].objVal->fields["y"].toDouble() ? -1 : 0);
            r.objVal->fields["z"] = Value::makeInt(a[0].objVal->fields["z"].toDouble() == a[1].objVal->fields["z"].toDouble() ? -1 : 0);
            r.objVal->fields["w"] = Value::makeInt(a[0].objVal->fields["w"].toDouble() == a[1].objVal->fields["w"].toDouble() ? -1 : 0);
        } else {
            r.objVal->fields["x"] = Value::makeInt(a[0].objVal->fields["x"].toInt() == a[1].objVal->fields["x"].toInt() ? -1 : 0);
            r.objVal->fields["y"] = Value::makeInt(a[0].objVal->fields["y"].toInt() == a[1].objVal->fields["y"].toInt() ? -1 : 0);
            r.objVal->fields["z"] = Value::makeInt(a[0].objVal->fields["z"].toInt() == a[1].objVal->fields["z"].toInt() ? -1 : 0);
            r.objVal->fields["w"] = Value::makeInt(a[0].objVal->fields["w"].toInt() == a[1].objVal->fields["w"].toInt() ? -1 : 0);
        }
        return r;
    });

    // SIMD.extractLane(vec, index) — extract a single lane value
    vm.registerNative("SIMD.extractLane", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.extractLane: requires vector and lane index"); return Value::makeNull(); }
        int lane = a[1].toInt();
        if (lane < 0 || lane > 3) { vm.throwError("GardWasmSIMDError", "SIMD.extractLane: lane must be 0-3"); return Value::makeNull(); }
        const char* keys[] = {"x", "y", "z", "w"};
        return a[0].objVal->fields[keys[lane]];
    });

    // SIMD.replaceLane(vec, index, value) — replace a single lane, returns new vector
    vm.registerNative("SIMD.replaceLane", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.replaceLane: requires vector, lane, value"); return Value::makeNull(); }
        int lane = a[1].toInt();
        if (lane < 0 || lane > 3) { vm.throwError("GardWasmSIMDError", "SIMD.replaceLane: lane must be 0-3"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        Value r = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        r.objVal->fields = a[0].objVal->fields; // copy
        const char* keys[] = {"x", "y", "z", "w"};
        r.objVal->fields[keys[lane]] = a[2];
        return r;
    });

    // SIMD.load(memory, ptr) — load 128-bit vector from memory (4 x i32)
    vm.registerNative("SIMD.load", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.load: requires memory and pointer"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 16 > size) { vm.throwError("GardWasmSIMDError", "SIMD.load: out of bounds (need 16 bytes at ptr=" + std::to_string(ptr) + ")"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        // Read 4 x i32 (little-endian)
        Value v = Value::makeObject("Int32x4");
        v.objVal->fields["_type"] = Value::makeString("i32x4");
        for (int lane = 0; lane < 4; lane++) {
            int offset = ptr + lane * 4;
            int32_t val = (buf.arrVal->elements[offset].toInt() & 0xFF) |
                          ((buf.arrVal->elements[offset+1].toInt() & 0xFF) << 8) |
                          ((buf.arrVal->elements[offset+2].toInt() & 0xFF) << 16) |
                          ((buf.arrVal->elements[offset+3].toInt() & 0xFF) << 24);
            const char* keys[] = {"x", "y", "z", "w"};
            v.objVal->fields[keys[lane]] = Value::makeInt(val);
        }
        return v;
    });

    // SIMD.store(memory, ptr, vec) — store 128-bit vector to memory
    vm.registerNative("SIMD.store", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal || !a[2].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.store: requires memory, pointer, vector"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 16 > size) { vm.throwError("GardWasmSIMDError", "SIMD.store: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        const char* keys[] = {"x", "y", "z", "w"};
        for (int lane = 0; lane < 4; lane++) {
            int32_t val = a[2].objVal->fields[keys[lane]].toInt();
            int offset = ptr + lane * 4;
            buf.arrVal->elements[offset]   = Value::makeInt(val & 0xFF);
            buf.arrVal->elements[offset+1] = Value::makeInt((val >> 8) & 0xFF);
            buf.arrVal->elements[offset+2] = Value::makeInt((val >> 16) & 0xFF);
            buf.arrVal->elements[offset+3] = Value::makeInt((val >> 24) & 0xFF);
        }
        return Value::makeBool(true);
    });

    // SIMD.dot(a, b) — dot product of two Float32x4 vectors (returns scalar)
    vm.registerNative("SIMD.dot", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.dot: requires two vectors"); return Value::makeNull(); }
        double sum = a[0].objVal->fields["x"].toDouble() * a[1].objVal->fields["x"].toDouble()
                   + a[0].objVal->fields["y"].toDouble() * a[1].objVal->fields["y"].toDouble()
                   + a[0].objVal->fields["z"].toDouble() * a[1].objVal->fields["z"].toDouble()
                   + a[0].objVal->fields["w"].toDouble() * a[1].objVal->fields["w"].toDouble();
        return Value::makeDouble(sum);
    });

    // SIMD.min(a, b) — lane-wise minimum
    vm.registerNative("SIMD.min", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.min: requires two vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        Value r = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        r.objVal->fields["_type"] = Value::makeString(type);
        const char* keys[] = {"x", "y", "z", "w"};
        for (int i = 0; i < 4; i++) {
            if (type == "f32x4") {
                double va = a[0].objVal->fields[keys[i]].toDouble(), vb = a[1].objVal->fields[keys[i]].toDouble();
                r.objVal->fields[keys[i]] = Value::makeDouble(va < vb ? va : vb);
            } else {
                int va = a[0].objVal->fields[keys[i]].toInt(), vb = a[1].objVal->fields[keys[i]].toInt();
                r.objVal->fields[keys[i]] = Value::makeInt(va < vb ? va : vb);
            }
        }
        return r;
    });

    // SIMD.max(a, b) — lane-wise maximum
    vm.registerNative("SIMD.max", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardWasmSIMDError", "SIMD.max: requires two vectors"); return Value::makeNull(); }
        std::string type = a[0].objVal->fields["_type"].toString();
        Value r = Value::makeObject(type == "f32x4" ? "Float32x4" : "Int32x4");
        r.objVal->fields["_type"] = Value::makeString(type);
        const char* keys[] = {"x", "y", "z", "w"};
        for (int i = 0; i < 4; i++) {
            if (type == "f32x4") {
                double va = a[0].objVal->fields[keys[i]].toDouble(), vb = a[1].objVal->fields[keys[i]].toDouble();
                r.objVal->fields[keys[i]] = Value::makeDouble(va > vb ? va : vb);
            } else {
                int va = a[0].objVal->fields[keys[i]].toInt(), vb = a[1].objVal->fields[keys[i]].toInt();
                r.objVal->fields[keys[i]] = Value::makeInt(va > vb ? va : vb);
            }
        }
        return r;
    });

    // --- Wasm Threads: SharedArrayBuffer + Atomics ---

    // WasmThread.createSharedMemory(pages, maxPages) — create shared memory for multi-threaded Wasm
    vm.registerNative("WasmThread.createSharedMemory", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 1) { vm.throwError("GardWasmThreadError", "WasmThread.createSharedMemory: requires initial pages"); return Value::makeNull(); }
        int pages = a[0].toInt();
        int maxPages = a.size() >= 2 ? a[1].toInt() : pages * 2;
        if (pages < 1 || maxPages < pages) { vm.throwError("GardWasmThreadError", "WasmThread.createSharedMemory: invalid page bounds"); return Value::makeNull(); }
        int bufSize = pages * 65536;
        Value mem = Value::makeObject("SharedMemory");
        mem.objVal->fields["_pages"] = Value::makeInt(pages);
        mem.objVal->fields["_maxPages"] = Value::makeInt(maxPages);
        mem.objVal->fields["_size"] = Value::makeInt(bufSize);
        mem.objVal->fields["_shared"] = Value::makeBool(true);
        Value buf = Value::makeArray();
        buf.arrVal->elements.resize(bufSize, Value::makeInt(0));
        mem.objVal->fields["_buffer"] = buf;
        mem.objVal->fields["pages"] = Value::makeInt(pages);
        mem.objVal->fields["maxPages"] = Value::makeInt(maxPages);
        mem.objVal->fields["byteLength"] = Value::makeInt(bufSize);
        mem.objVal->fields["shared"] = Value::makeBool(true);
        return mem;
    });

    // WasmThread.atomicLoad(memory, ptr) — atomic read of i32 at ptr
    vm.registerNative("WasmThread.atomicLoad", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicLoad: requires shared memory and pointer"); return Value::makeNull(); }
        if (!a[0].objVal->fields["_shared"].toBool()) { vm.throwError("GardWasmThreadError", "WasmThread.atomicLoad: memory must be shared"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        if (ptr % 4 != 0) { vm.throwError("GardWasmThreadError", "WasmThread.atomicLoad: pointer must be 4-byte aligned (got " + std::to_string(ptr) + ")"); return Value::makeNull(); }
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size) { vm.throwError("GardWasmThreadError", "WasmThread.atomicLoad: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeInt(0);
        int32_t val = (buf.arrVal->elements[ptr].toInt() & 0xFF) |
                      ((buf.arrVal->elements[ptr+1].toInt() & 0xFF) << 8) |
                      ((buf.arrVal->elements[ptr+2].toInt() & 0xFF) << 16) |
                      ((buf.arrVal->elements[ptr+3].toInt() & 0xFF) << 24);
        return Value::makeInt(val);
    });

    // WasmThread.atomicStore(memory, ptr, value) — atomic write of i32 at ptr
    vm.registerNative("WasmThread.atomicStore", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicStore: requires shared memory, pointer, value"); return Value::makeNull(); }
        if (!a[0].objVal->fields["_shared"].toBool()) { vm.throwError("GardWasmThreadError", "WasmThread.atomicStore: memory must be shared"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        if (ptr % 4 != 0) { vm.throwError("GardWasmThreadError", "WasmThread.atomicStore: pointer must be 4-byte aligned"); return Value::makeNull(); }
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size) { vm.throwError("GardWasmThreadError", "WasmThread.atomicStore: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeNull();
        int32_t val = a[2].toInt();
        buf.arrVal->elements[ptr]   = Value::makeInt(val & 0xFF);
        buf.arrVal->elements[ptr+1] = Value::makeInt((val >> 8) & 0xFF);
        buf.arrVal->elements[ptr+2] = Value::makeInt((val >> 16) & 0xFF);
        buf.arrVal->elements[ptr+3] = Value::makeInt((val >> 24) & 0xFF);
        return Value::makeBool(true);
    });

    // WasmThread.atomicAdd(memory, ptr, value) — atomic add, returns old value
    vm.registerNative("WasmThread.atomicAdd", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicAdd: requires shared memory, pointer, value"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        if (ptr % 4 != 0) { vm.throwError("GardWasmThreadError", "WasmThread.atomicAdd: pointer must be 4-byte aligned"); return Value::makeNull(); }
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size) { vm.throwError("GardWasmThreadError", "WasmThread.atomicAdd: out of bounds"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeInt(0);
        int32_t old = (buf.arrVal->elements[ptr].toInt() & 0xFF) | ((buf.arrVal->elements[ptr+1].toInt() & 0xFF) << 8) |
                      ((buf.arrVal->elements[ptr+2].toInt() & 0xFF) << 16) | ((buf.arrVal->elements[ptr+3].toInt() & 0xFF) << 24);
        int32_t newVal = old + a[2].toInt();
        buf.arrVal->elements[ptr]   = Value::makeInt(newVal & 0xFF);
        buf.arrVal->elements[ptr+1] = Value::makeInt((newVal >> 8) & 0xFF);
        buf.arrVal->elements[ptr+2] = Value::makeInt((newVal >> 16) & 0xFF);
        buf.arrVal->elements[ptr+3] = Value::makeInt((newVal >> 24) & 0xFF);
        return Value::makeInt(old);
    });

    // WasmThread.atomicSub(memory, ptr, value) — atomic subtract, returns old value
    vm.registerNative("WasmThread.atomicSub", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicSub: requires shared memory, pointer, value"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size || ptr % 4 != 0) { vm.throwError("GardWasmThreadError", "WasmThread.atomicSub: invalid pointer"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeInt(0);
        int32_t old = (buf.arrVal->elements[ptr].toInt() & 0xFF) | ((buf.arrVal->elements[ptr+1].toInt() & 0xFF) << 8) |
                      ((buf.arrVal->elements[ptr+2].toInt() & 0xFF) << 16) | ((buf.arrVal->elements[ptr+3].toInt() & 0xFF) << 24);
        int32_t newVal = old - a[2].toInt();
        buf.arrVal->elements[ptr] = Value::makeInt(newVal & 0xFF); buf.arrVal->elements[ptr+1] = Value::makeInt((newVal >> 8) & 0xFF);
        buf.arrVal->elements[ptr+2] = Value::makeInt((newVal >> 16) & 0xFF); buf.arrVal->elements[ptr+3] = Value::makeInt((newVal >> 24) & 0xFF);
        return Value::makeInt(old);
    });

    // WasmThread.atomicCompareExchange(memory, ptr, expected, replacement) — CAS, returns old value
    vm.registerNative("WasmThread.atomicCompareExchange", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicCompareExchange: requires memory, ptr, expected, replacement"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size || ptr % 4 != 0) { vm.throwError("GardWasmThreadError", "WasmThread.atomicCompareExchange: invalid pointer"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeInt(0);
        int32_t current = (buf.arrVal->elements[ptr].toInt() & 0xFF) | ((buf.arrVal->elements[ptr+1].toInt() & 0xFF) << 8) |
                          ((buf.arrVal->elements[ptr+2].toInt() & 0xFF) << 16) | ((buf.arrVal->elements[ptr+3].toInt() & 0xFF) << 24);
        int32_t expected = a[2].toInt();
        if (current == expected) {
            int32_t replacement = a[3].toInt();
            buf.arrVal->elements[ptr] = Value::makeInt(replacement & 0xFF); buf.arrVal->elements[ptr+1] = Value::makeInt((replacement >> 8) & 0xFF);
            buf.arrVal->elements[ptr+2] = Value::makeInt((replacement >> 16) & 0xFF); buf.arrVal->elements[ptr+3] = Value::makeInt((replacement >> 24) & 0xFF);
        }
        return Value::makeInt(current);
    });

    // WasmThread.atomicWait(memory, ptr, expected) — block until value changes (simulated)
    vm.registerNative("WasmThread.atomicWait", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicWait: requires memory, ptr, expected"); return Value::makeNull(); }
        int ptr = a[1].toInt();
        int size = a[0].objVal->fields["_size"].toInt();
        if (ptr < 0 || ptr + 4 > size || ptr % 4 != 0) { vm.throwError("GardWasmThreadError", "WasmThread.atomicWait: invalid pointer"); return Value::makeNull(); }
        auto& buf = a[0].objVal->fields["_buffer"];
        if (!buf.arrVal) return Value::makeString("not-equal");
        int32_t current = (buf.arrVal->elements[ptr].toInt() & 0xFF) | ((buf.arrVal->elements[ptr+1].toInt() & 0xFF) << 8) |
                          ((buf.arrVal->elements[ptr+2].toInt() & 0xFF) << 16) | ((buf.arrVal->elements[ptr+3].toInt() & 0xFF) << 24);
        if (current != a[2].toInt()) return Value::makeString("not-equal");
        // In real multi-threaded Wasm, this would block. In single-threaded simulation, return "ok"
        return Value::makeString("ok");
    });

    // WasmThread.atomicNotify(memory, ptr, count) — wake waiting threads
    vm.registerNative("WasmThread.atomicNotify", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardWasmThreadError", "WasmThread.atomicNotify: requires memory, ptr, count"); return Value::makeNull(); }
        // In single-threaded simulation, returns 0 (no waiters woken)
        return Value::makeInt(0);
    });

    // --- Wasm Optimizer: dead code elimination, tree shaking ---

    // WasmOptimizer.deadCodeElimination(module) — remove unreachable functions
    vm.registerNative("WasmOptimizer.deadCodeElimination", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "WasmOptimizer.deadCodeElimination: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "WasmOptimizer.deadCodeElimination: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        // Find all exported function indices
        std::set<uint32_t> reachable;
        uint32_t importCount = 0;
        for (auto& imp : mod->imports) { if (imp.kind == 0) importCount++; }
        for (auto& exp : mod->exports) {
            if (exp.kind == EXPORT_FUNC) reachable.insert(exp.index - importCount);
        }
        if (mod->startFunc >= 0) reachable.insert((uint32_t)mod->startFunc - importCount);

        // Scan reachable function bodies for CALL instructions to find transitive reachability
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t idx : reachable) {
                if (idx >= mod->code.size()) continue;
                auto& body = mod->code[idx].body;
                for (size_t i = 0; i < body.size(); i++) {
                    if (body[i] == OP_CALL && i + 1 < body.size()) {
                        uint32_t target = body[i + 1]; // simplified: single-byte index
                        if (target >= importCount && reachable.find(target - importCount) == reachable.end()) {
                            reachable.insert(target - importCount);
                            changed = true;
                        }
                    }
                }
            }
        }

        int eliminated = 0;
        int total = (int)mod->code.size();
        // Mark unreachable functions with empty bodies (can't remove due to index stability)
        for (int i = 0; i < total; i++) {
            if (reachable.find((uint32_t)i) == reachable.end()) {
                mod->code[i].body.clear();
                mod->code[i].body.push_back(OP_UNREACHABLE);
                eliminated++;
            }
        }

        Value result = Value::makeObject("OptimizationResult");
        result.objVal->fields["eliminated"] = Value::makeInt(eliminated);
        result.objVal->fields["retained"] = Value::makeInt(total - eliminated);
        result.objVal->fields["total"] = Value::makeInt(total);
        return result;
    });

    // WasmOptimizer.treeShake(module, entryPoints) — keep only functions reachable from entry points
    vm.registerNative("WasmOptimizer.treeShake", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmError", "WasmOptimizer.treeShake: requires module and entry points array"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "WasmOptimizer.treeShake: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        // Remove exports not in entry points list
        if (!a[1].arrVal) { vm.throwError("GardWasmError", "WasmOptimizer.treeShake: entry points must be an array of export names"); return Value::makeNull(); }
        std::set<std::string> keepExports;
        for (auto& ep : a[1].arrVal->elements) keepExports.insert(ep.toString());

        int removed = 0;
        auto expIt = mod->exports.begin();
        while (expIt != mod->exports.end()) {
            if (expIt->kind == EXPORT_FUNC && keepExports.find(expIt->name) == keepExports.end()) {
                expIt = mod->exports.erase(expIt);
                removed++;
            } else {
                ++expIt;
            }
        }

        Value result = Value::makeObject("TreeShakeResult");
        result.objVal->fields["removedExports"] = Value::makeInt(removed);
        result.objVal->fields["remainingExports"] = Value::makeInt((int)mod->exports.size());
        return result;
    });

    // WasmOptimizer.getStats(module) — get optimization statistics
    vm.registerNative("WasmOptimizer.getStats", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmError", "WasmOptimizer.getStats: requires module"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_wasmId"].toInt();
        auto it = g_wasmModules.find(id);
        if (it == g_wasmModules.end()) { vm.throwError("GardWasmError", "WasmOptimizer.getStats: module not found"); return Value::makeNull(); }
        auto& mod = it->second;

        int totalCodeBytes = 0, emptyFuncs = 0;
        for (auto& fn : mod->code) {
            totalCodeBytes += (int)fn.body.size();
            if (fn.body.size() <= 1) emptyFuncs++;
        }

        Value stats = Value::makeObject("OptimizerStats");
        stats.objVal->fields["functions"] = Value::makeInt((int)mod->code.size());
        stats.objVal->fields["emptyFunctions"] = Value::makeInt(emptyFuncs);
        stats.objVal->fields["totalCodeBytes"] = Value::makeInt(totalCodeBytes);
        stats.objVal->fields["exports"] = Value::makeInt((int)mod->exports.size());
        stats.objVal->fields["imports"] = Value::makeInt((int)mod->imports.size());
        stats.objVal->fields["dataSegments"] = Value::makeInt((int)mod->dataSegments.size());
        stats.objVal->fields["globals"] = Value::makeInt((int)mod->globals.size());
        return stats;
    });

    // ===== 7.11 WebAssembly Memory Management =====

    // WasmHeap — Real free-list allocator with alignment, ref counting, leak detection
    // Internal block header: [size:4][refCount:4][flags:4][padding:4] = 16 bytes overhead per allocation

    // WasmHeap.create(memory, startOffset?) — create a heap allocator on a WasmMemory instance
    vm.registerNative("WasmHeap.create", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.create: requires WasmMemory instance"); return Value::makeNull(); }
        int memSize = a[0].objVal->fields["_size"].toInt();
        int startOffset = a.size() >= 2 ? a[1].toInt() : 256; // reserve first 256 bytes for stack/globals
        if (startOffset < 0 || startOffset >= memSize) { vm.throwError("GardWasmHeapError", "WasmHeap.create: invalid start offset"); return Value::makeNull(); }
        // Align start to 16 bytes
        while (startOffset % 16 != 0) startOffset++;

        Value heap = Value::makeObject("WasmHeap");
        heap.objVal->fields["_memory"] = a[0]; // reference to the memory
        heap.objVal->fields["_start"] = Value::makeInt(startOffset);
        heap.objVal->fields["_end"] = Value::makeInt(memSize);
        heap.objVal->fields["_nextFree"] = Value::makeInt(startOffset);
        heap.objVal->fields["_totalAllocated"] = Value::makeInt(0);
        heap.objVal->fields["_totalFreed"] = Value::makeInt(0);
        heap.objVal->fields["_allocationCount"] = Value::makeInt(0);
        heap.objVal->fields["_freeCount"] = Value::makeInt(0);
        heap.objVal->fields["_peakUsage"] = Value::makeInt(0);
        heap.objVal->fields["_currentUsage"] = Value::makeInt(0);
        heap.objVal->fields["_maxSize"] = Value::makeInt(memSize - startOffset);
        heap.objVal->fields["_debugMode"] = Value::makeBool(false);
        // Free list: array of {ptr, size} objects
        heap.objVal->fields["_freeList"] = Value::makeArray();
        // Allocation registry (for leak detection): ptr -> {size, refCount, tag}
        heap.objVal->fields["_allocations"] = Value::makeObject("AllocRegistry");
        return heap;
    });

    // WasmHeap.alloc(heap, size, alignment?) — allocate with alignment (default 8)
    vm.registerNative("WasmHeap.alloc", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.alloc: requires heap and size"); return Value::makeNull(); }
        int size = a[1].toInt();
        int alignment = a.size() >= 3 ? a[2].toInt() : 8;
        if (size <= 0) { vm.throwError("GardWasmHeapError", "WasmHeap.alloc: size must be > 0"); return Value::makeNull(); }
        if (alignment < 1 || (alignment & (alignment - 1)) != 0) { vm.throwError("GardWasmHeapError", "WasmHeap.alloc: alignment must be a power of 2 (got " + std::to_string(alignment) + ")"); return Value::makeNull(); }

        int headerSize = 16; // [size:4][refCount:4][flags:4][pad:4]
        int totalNeeded = headerSize + size;
        // Align total to alignment
        while (totalNeeded % alignment != 0) totalNeeded++;

        // 1. Try free list first (first-fit)
        auto& freeList = a[0].objVal->fields["_freeList"];
        int foundIdx = -1;
        if (freeList.arrVal) {
            for (int i = 0; i < (int)freeList.arrVal->elements.size(); i++) {
                auto& block = freeList.arrVal->elements[i];
                if (block.objVal) {
                    int blockSize = block.objVal->fields["size"].toInt();
                    int blockPtr = block.objVal->fields["ptr"].toInt();
                    // Check alignment
                    int dataPtr = blockPtr + headerSize;
                    int alignedDataPtr = dataPtr;
                    while (alignedDataPtr % alignment != 0) alignedDataPtr++;
                    int waste = alignedDataPtr - dataPtr;
                    if (blockSize >= totalNeeded + waste) {
                        foundIdx = i;
                        break;
                    }
                }
            }
        }

        int ptr;
        if (foundIdx >= 0) {
            // Use free list block
            auto& block = freeList.arrVal->elements[foundIdx];
            ptr = block.objVal->fields["ptr"].toInt();
            int blockSize = block.objVal->fields["size"].toInt();
            // Remove from free list (or split if much larger)
            int remaining = blockSize - totalNeeded;
            if (remaining > headerSize + 8) {
                // Split: keep remainder in free list
                block.objVal->fields["ptr"] = Value::makeInt(ptr + totalNeeded);
                block.objVal->fields["size"] = Value::makeInt(remaining);
            } else {
                // Use entire block
                totalNeeded = blockSize;
                freeList.arrVal->elements.erase(freeList.arrVal->elements.begin() + foundIdx);
            }
        } else {
            // 2. Bump allocate from end
            int nextFree = a[0].objVal->fields["_nextFree"].toInt();
            // Align
            while ((nextFree + headerSize) % alignment != 0) nextFree++;
            int end = a[0].objVal->fields["_end"].toInt();
            if (nextFree + totalNeeded > end) {
                // Try to grow memory
                auto& memory = a[0].objVal->fields["_memory"];
                if (memory.objVal) {
                    int currentPages = memory.objVal->fields["_pages"].toInt();
                    int maxPages = memory.objVal->fields["_maxPages"].toInt();
                    int neededBytes = (nextFree + totalNeeded) - end;
                    int neededPages = (neededBytes + 65535) / 65536;
                    if (currentPages + neededPages <= maxPages) {
                        int newPages = currentPages + neededPages;
                        int newSize = newPages * 65536;
                        auto& buf = memory.objVal->fields["_buffer"];
                        if (buf.arrVal) buf.arrVal->elements.resize(newSize, Value::makeInt(0));
                        memory.objVal->fields["_pages"] = Value::makeInt(newPages);
                        memory.objVal->fields["_size"] = Value::makeInt(newSize);
                        memory.objVal->fields["pages"] = Value::makeInt(newPages);
                        memory.objVal->fields["byteLength"] = Value::makeInt(newSize);
                        a[0].objVal->fields["_end"] = Value::makeInt(newSize);
                        end = newSize;
                    } else {
                        vm.throwError("GardWasmHeapError", "WasmHeap.alloc: out of memory (need " + std::to_string(totalNeeded) + " bytes, cannot grow beyond " + std::to_string(maxPages) + " pages)");
                        return Value::makeNull();
                    }
                } else {
                    vm.throwError("GardWasmHeapError", "WasmHeap.alloc: out of memory (need " + std::to_string(totalNeeded) + " bytes)");
                    return Value::makeNull();
                }
            }
            ptr = nextFree;
            a[0].objVal->fields["_nextFree"] = Value::makeInt(nextFree + totalNeeded);
        }

        // Write header to memory: [size][refCount=1][flags=1(allocated)][pad=0]
        auto& memory = a[0].objVal->fields["_memory"];
        if (memory.objVal) {
            auto& buf = memory.objVal->fields["_buffer"];
            if (buf.arrVal) {
                // Write size (4 bytes LE)
                buf.arrVal->elements[ptr]   = Value::makeInt(totalNeeded & 0xFF);
                buf.arrVal->elements[ptr+1] = Value::makeInt((totalNeeded >> 8) & 0xFF);
                buf.arrVal->elements[ptr+2] = Value::makeInt((totalNeeded >> 16) & 0xFF);
                buf.arrVal->elements[ptr+3] = Value::makeInt((totalNeeded >> 24) & 0xFF);
                // Write refCount = 1
                buf.arrVal->elements[ptr+4] = Value::makeInt(1);
                buf.arrVal->elements[ptr+5] = Value::makeInt(0);
                buf.arrVal->elements[ptr+6] = Value::makeInt(0);
                buf.arrVal->elements[ptr+7] = Value::makeInt(0);
                // Write flags = 1 (allocated)
                buf.arrVal->elements[ptr+8] = Value::makeInt(1);
                buf.arrVal->elements[ptr+9] = Value::makeInt(0);
                buf.arrVal->elements[ptr+10] = Value::makeInt(0);
                buf.arrVal->elements[ptr+11] = Value::makeInt(0);
            }
        }

        // Update stats
        int totalAlloc = a[0].objVal->fields["_totalAllocated"].toInt() + totalNeeded;
        int allocCount = a[0].objVal->fields["_allocationCount"].toInt() + 1;
        int currentUsage = a[0].objVal->fields["_currentUsage"].toInt() + totalNeeded;
        int peakUsage = a[0].objVal->fields["_peakUsage"].toInt();
        if (currentUsage > peakUsage) peakUsage = currentUsage;
        a[0].objVal->fields["_totalAllocated"] = Value::makeInt(totalAlloc);
        a[0].objVal->fields["_allocationCount"] = Value::makeInt(allocCount);
        a[0].objVal->fields["_currentUsage"] = Value::makeInt(currentUsage);
        a[0].objVal->fields["_peakUsage"] = Value::makeInt(peakUsage);

        // Register allocation (for leak detection)
        int dataPtr = ptr + headerSize;
        auto& allocs = a[0].objVal->fields["_allocations"];
        if (allocs.objVal) {
            Value entry = Value::makeObject("AllocEntry");
            entry.objVal->fields["size"] = Value::makeInt(size);
            entry.objVal->fields["totalSize"] = Value::makeInt(totalNeeded);
            entry.objVal->fields["refCount"] = Value::makeInt(1);
            entry.objVal->fields["headerPtr"] = Value::makeInt(ptr);
            allocs.objVal->fields[std::to_string(dataPtr)] = entry;
        }

        return Value::makeInt(dataPtr);
    });

    // WasmHeap.free(heap, ptr) — free an allocation
    vm.registerNative("WasmHeap.free", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.free: requires heap and pointer"); return Value::makeNull(); }
        int dataPtr = a[1].toInt();
        std::string key = std::to_string(dataPtr);
        auto& allocs = a[0].objVal->fields["_allocations"];
        if (!allocs.objVal || allocs.objVal->fields.find(key) == allocs.objVal->fields.end()) {
            vm.throwError("GardWasmHeapError", "WasmHeap.free: invalid pointer " + std::to_string(dataPtr) + " (not allocated or already freed)");
            return Value::makeNull();
        }
        auto& entry = allocs.objVal->fields[key];
        int totalSize = entry.objVal->fields["totalSize"].toInt();
        int headerPtr = entry.objVal->fields["headerPtr"].toInt();

        // Mark as freed in memory header (flags = 0)
        auto& memory = a[0].objVal->fields["_memory"];
        if (memory.objVal) {
            auto& buf = memory.objVal->fields["_buffer"];
            if (buf.arrVal && headerPtr + 12 < (int)buf.arrVal->elements.size()) {
                buf.arrVal->elements[headerPtr+8] = Value::makeInt(0); // flags = 0 (freed)
            }
        }

        // Add to free list
        auto& freeList = a[0].objVal->fields["_freeList"];
        if (freeList.arrVal) {
            Value block = Value::makeObject("FreeBlock");
            block.objVal->fields["ptr"] = Value::makeInt(headerPtr);
            block.objVal->fields["size"] = Value::makeInt(totalSize);
            freeList.arrVal->elements.push_back(block);
        }

        // Remove from allocation registry
        allocs.objVal->fields.erase(key);

        // Update stats
        int totalFreed = a[0].objVal->fields["_totalFreed"].toInt() + totalSize;
        int freeCount = a[0].objVal->fields["_freeCount"].toInt() + 1;
        int currentUsage = a[0].objVal->fields["_currentUsage"].toInt() - totalSize;
        a[0].objVal->fields["_totalFreed"] = Value::makeInt(totalFreed);
        a[0].objVal->fields["_freeCount"] = Value::makeInt(freeCount);
        a[0].objVal->fields["_currentUsage"] = Value::makeInt(currentUsage < 0 ? 0 : currentUsage);
        return Value::makeBool(true);
    });

    // WasmHeap.retain(heap, ptr) — increment reference count
    vm.registerNative("WasmHeap.retain", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.retain: requires heap and pointer"); return Value::makeNull(); }
        int dataPtr = a[1].toInt();
        std::string key = std::to_string(dataPtr);
        auto& allocs = a[0].objVal->fields["_allocations"];
        if (!allocs.objVal || allocs.objVal->fields.find(key) == allocs.objVal->fields.end()) {
            vm.throwError("GardWasmHeapError", "WasmHeap.retain: invalid pointer (not allocated)");
            return Value::makeNull();
        }
        auto& entry = allocs.objVal->fields[key];
        int rc = entry.objVal->fields["refCount"].toInt() + 1;
        entry.objVal->fields["refCount"] = Value::makeInt(rc);
        // Update header in memory
        int headerPtr = entry.objVal->fields["headerPtr"].toInt();
        auto& memory = a[0].objVal->fields["_memory"];
        if (memory.objVal) {
            auto& buf = memory.objVal->fields["_buffer"];
            if (buf.arrVal) {
                buf.arrVal->elements[headerPtr+4] = Value::makeInt(rc & 0xFF);
                buf.arrVal->elements[headerPtr+5] = Value::makeInt((rc >> 8) & 0xFF);
            }
        }
        return Value::makeInt(rc);
    });

    // WasmHeap.release(heap, ptr) — decrement reference count, free if reaches 0
    vm.registerNative("WasmHeap.release", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.release: requires heap and pointer"); return Value::makeNull(); }
        int dataPtr = a[1].toInt();
        std::string key = std::to_string(dataPtr);
        auto& allocs = a[0].objVal->fields["_allocations"];
        if (!allocs.objVal || allocs.objVal->fields.find(key) == allocs.objVal->fields.end()) {
            vm.throwError("GardWasmHeapError", "WasmHeap.release: invalid pointer (not allocated)");
            return Value::makeNull();
        }
        auto& entry = allocs.objVal->fields[key];
        int rc = entry.objVal->fields["refCount"].toInt() - 1;
        if (rc <= 0) {
            // Auto-free
            std::vector<Value> freeArgs = {a[0], Value::makeInt(dataPtr)};
            vm.callNative("WasmHeap.free", freeArgs);
            return Value::makeInt(0);
        }
        entry.objVal->fields["refCount"] = Value::makeInt(rc);
        return Value::makeInt(rc);
    });

    // WasmHeap.refCount(heap, ptr) — get current reference count
    vm.registerNative("WasmHeap.refCount", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.refCount: requires heap and pointer"); return Value::makeNull(); }
        std::string key = std::to_string(a[1].toInt());
        auto& allocs = a[0].objVal->fields["_allocations"];
        if (!allocs.objVal || allocs.objVal->fields.find(key) == allocs.objVal->fields.end()) return Value::makeInt(0);
        return allocs.objVal->fields[key].objVal->fields["refCount"];
    });

    // WasmHeap.stats(heap) — get memory usage statistics
    vm.registerNative("WasmHeap.stats", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        Value stats = Value::makeObject("HeapStats");
        stats.objVal->fields["totalAllocated"] = a[0].objVal->fields["_totalAllocated"];
        stats.objVal->fields["totalFreed"] = a[0].objVal->fields["_totalFreed"];
        stats.objVal->fields["currentUsage"] = a[0].objVal->fields["_currentUsage"];
        stats.objVal->fields["peakUsage"] = a[0].objVal->fields["_peakUsage"];
        stats.objVal->fields["allocationCount"] = a[0].objVal->fields["_allocationCount"];
        stats.objVal->fields["freeCount"] = a[0].objVal->fields["_freeCount"];
        stats.objVal->fields["freeListSize"] = Value::makeInt(a[0].objVal->fields["_freeList"].arrVal ? (int)a[0].objVal->fields["_freeList"].arrVal->elements.size() : 0);
        int liveCount = 0;
        auto& allocs = a[0].objVal->fields["_allocations"];
        if (allocs.objVal) liveCount = (int)allocs.objVal->fields.size();
        stats.objVal->fields["liveAllocations"] = Value::makeInt(liveCount);
        return stats;
    });

    // WasmHeap.detectLeaks(heap) — returns array of leaked allocations (not freed)
    vm.registerNative("WasmHeap.detectLeaks", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.detectLeaks: requires heap"); return Value::makeNull(); }
        auto& allocs = a[0].objVal->fields["_allocations"];
        Value leaks = Value::makeArray();
        if (allocs.objVal) {
            for (auto& [key, entry] : allocs.objVal->fields) {
                if (!entry.objVal) continue;
                Value leak = Value::makeObject("MemoryLeak");
                leak.objVal->fields["ptr"] = Value::makeInt(std::atoi(key.c_str()));
                leak.objVal->fields["size"] = entry.objVal->fields["size"];
                leak.objVal->fields["refCount"] = entry.objVal->fields["refCount"];
                leaks.arrVal->elements.push_back(leak);
            }
        }
        return leaks;
    });

    // WasmHeap.setDebugMode(heap, enabled) — enable/disable leak detection tracking
    vm.registerNative("WasmHeap.setDebugMode", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_debugMode"] = Value::makeBool(a[1].toBool());
        return Value::makeBool(true);
    });

    // WasmHeap.compact(heap) — coalesce adjacent free blocks in the free list
    vm.registerNative("WasmHeap.compact", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardWasmHeapError", "WasmHeap.compact: requires heap"); return Value::makeNull(); }
        auto& freeList = a[0].objVal->fields["_freeList"];
        if (!freeList.arrVal || freeList.arrVal->elements.size() < 2) return Value::makeInt(0);

        // Sort free list by pointer
        auto& elems = freeList.arrVal->elements;
        std::sort(elems.begin(), elems.end(), [](const Value& x, const Value& y) {
            return x.objVal->fields["ptr"].toInt() < y.objVal->fields["ptr"].toInt();
        });

        // Merge adjacent blocks
        int merged = 0;
        int i = 0;
        while (i < (int)elems.size() - 1) {
            int ptr1 = elems[i].objVal->fields["ptr"].toInt();
            int size1 = elems[i].objVal->fields["size"].toInt();
            int ptr2 = elems[i+1].objVal->fields["ptr"].toInt();
            if (ptr1 + size1 == ptr2) {
                // Merge: extend first block, remove second
                int size2 = elems[i+1].objVal->fields["size"].toInt();
                elems[i].objVal->fields["size"] = Value::makeInt(size1 + size2);
                elems.erase(elems.begin() + i + 1);
                merged++;
            } else {
                i++;
            }
        }
        return Value::makeInt(merged);
    });

    // WasmHeap.reset(heap) — free all allocations, reset heap to initial state
    vm.registerNative("WasmHeap.reset", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int start = a[0].objVal->fields["_start"].toInt();
        a[0].objVal->fields["_nextFree"] = Value::makeInt(start);
        a[0].objVal->fields["_totalAllocated"] = Value::makeInt(0);
        a[0].objVal->fields["_totalFreed"] = Value::makeInt(0);
        a[0].objVal->fields["_allocationCount"] = Value::makeInt(0);
        a[0].objVal->fields["_freeCount"] = Value::makeInt(0);
        a[0].objVal->fields["_peakUsage"] = Value::makeInt(0);
        a[0].objVal->fields["_currentUsage"] = Value::makeInt(0);
        a[0].objVal->fields["_freeList"] = Value::makeArray();
        a[0].objVal->fields["_allocations"] = Value::makeObject("AllocRegistry");
        return Value::makeBool(true);
    });

    // WasmHeap.usedBytes(heap) — current bytes in use
    vm.registerNative("WasmHeap.usedBytes", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        return a[0].objVal->fields["_currentUsage"];
    });

    // WasmHeap.availableBytes(heap) — bytes available before needing to grow
    vm.registerNative("WasmHeap.availableBytes", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int end = a[0].objVal->fields["_end"].toInt();
        int nextFree = a[0].objVal->fields["_nextFree"].toInt();
        int freeListBytes = 0;
        auto& freeList = a[0].objVal->fields["_freeList"];
        if (freeList.arrVal) {
            for (auto& block : freeList.arrVal->elements) {
                if (block.objVal) freeListBytes += block.objVal->fields["size"].toInt();
            }
        }
        return Value::makeInt((end - nextFree) + freeListBytes);
    });
}

} // namespace stdlib
} // namespace runtime
} // namespace gard
