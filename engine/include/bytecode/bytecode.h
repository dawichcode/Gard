#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace gard {
namespace bytecode {

// --- Bytecode instruction set ---

enum class OpCode : uint8_t {
    // Stack operations
    NOP         = 0x00,
    POP         = 0x01,
    DUP         = 0x02,
    SWAP        = 0x03,

    // Constants
    CONST_NULL  = 0x10,
    CONST_TRUE  = 0x11,
    CONST_FALSE = 0x12,
    CONST_I32   = 0x13,  // followed by 4 bytes (i32)
    CONST_I64   = 0x14,  // followed by 8 bytes (i64)
    CONST_F32   = 0x15,  // followed by 4 bytes (f32)
    CONST_F64   = 0x16,  // followed by 8 bytes (f64)
    CONST_STR   = 0x17,  // followed by 2 bytes (constant pool index)

    // Local variables
    LOAD_LOCAL  = 0x20,  // followed by 2 bytes (local index)
    STORE_LOCAL = 0x21,  // followed by 2 bytes (local index)

    // Global variables
    LOAD_GLOBAL = 0x22,  // followed by 2 bytes (global index)
    STORE_GLOBAL= 0x23,  // followed by 2 bytes (global index)

    // Arithmetic
    ADD_I       = 0x30,
    SUB_I       = 0x31,
    MUL_I       = 0x32,
    DIV_I       = 0x33,
    MOD_I       = 0x34,
    NEG_I       = 0x35,
    ADD_F       = 0x36,
    SUB_F       = 0x37,
    MUL_F       = 0x38,
    DIV_F       = 0x39,
    NEG_F       = 0x3A,

    // Bitwise
    BIT_AND     = 0x40,
    BIT_OR      = 0x41,
    BIT_XOR     = 0x42,
    BIT_NOT     = 0x43,
    SHL         = 0x44,
    SHR         = 0x45,
    USHR        = 0x46,

    // Comparison
    CMP_EQ      = 0x50,
    CMP_NE      = 0x51,
    CMP_LT      = 0x52,
    CMP_GT      = 0x53,
    CMP_LE      = 0x54,
    CMP_GE      = 0x55,

    // Logical
    LOG_AND     = 0x58,
    LOG_OR      = 0x59,
    LOG_NOT     = 0x5A,

    // Control flow
    JUMP        = 0x60,  // followed by 2 bytes (offset)
    JUMP_IF     = 0x61,  // followed by 2 bytes (offset) — jump if top is true
    JUMP_IFNOT  = 0x62,  // followed by 2 bytes (offset) — jump if top is false
    RETURN      = 0x63,
    RETURN_VOID = 0x64,

    // Function calls
    CALL        = 0x70,  // followed by 2 bytes (func index) + 1 byte (arg count)
    CALL_VIRTUAL= 0x71,  // followed by 2 bytes (method name index) + 1 byte (arg count)
    CALL_ASYNC  = 0x72,  // followed by 2 bytes (func index) + 1 byte (arg count)

    // Object operations
    NEW_OBJ     = 0x80,  // followed by 2 bytes (class index)
    GET_FIELD   = 0x81,  // followed by 2 bytes (field name index)
    SET_FIELD   = 0x82,  // followed by 2 bytes (field name index)
    NEW_ARRAY   = 0x83,  // followed by 2 bytes (size)
    GET_ELEM    = 0x84,
    SET_ELEM    = 0x85,
    NEW_MAP     = 0x86,
    ARRAY_LEN   = 0x87,

    // String
    CONCAT      = 0x90,

    // Type operations
    CAST        = 0xA0,  // followed by 1 byte (target type)
    IS_TYPE     = 0xA1,  // followed by 2 bytes (type index)
    IS_NULL     = 0xA2,

    // Async
    AWAIT       = 0xB0,
    YIELD       = 0xB1,

    // Exception handling
    TRY_BEGIN   = 0xB8,  // followed by 2 bytes (catch PC offset)
    TRY_END     = 0xB9,  // pop exception handler
    THROW       = 0xBA,  // throw top of stack as exception

    // Built-in
    PRINT       = 0xC0,

    // Closure
    CLOSURE     = 0xD0,  // followed by 2 bytes (func index) + 1 byte (capture count)
    LOAD_CAPTURE= 0xD1,  // followed by 1 byte (capture index)

    // Superinstructions (fused opcode sequences for hot paths)
    // Each eliminates 1-3 dispatch cycles
    LOAD_ADD_I  = 0xE0,  // LOAD_LOCAL(a) + LOAD_LOCAL(b) + ADD_I → 2 bytes (idx_a, idx_b)
    LOAD_SUB_I  = 0xE1,  // LOAD_LOCAL(a) + LOAD_LOCAL(b) + SUB_I → 2 bytes (idx_a, idx_b)
    LOAD_CMP_LT = 0xE2,  // LOAD_LOCAL(a) + LOAD_LOCAL(b) + CMP_LT → 2 bytes (idx_a, idx_b)
    LOAD_CMP_GE = 0xE3,  // LOAD_LOCAL(a) + LOAD_LOCAL(b) + CMP_GE → 2 bytes (idx_a, idx_b)
    INC_LOCAL   = 0xE4,  // LOAD_LOCAL(x) + CONST_I32(1) + ADD_I + STORE_LOCAL(x) → 2 bytes (idx)
    DEC_LOCAL   = 0xE5,  // LOAD_LOCAL(x) + CONST_I32(1) + SUB_I + STORE_LOCAL(x) → 2 bytes (idx)
    LOAD_CONST_ADD = 0xE6, // LOAD_LOCAL(x) + CONST_I32(n) + ADD_I → 2 bytes (idx) + 4 bytes (n)
    LOAD_STORE  = 0xE7,  // LOAD_LOCAL(a) + STORE_LOCAL(b) → 2+2 bytes (idx_a, idx_b)

    // Debug
    LINE        = 0xF0,  // followed by 2 bytes (line number) — debug line marker
    HALT        = 0xFF,
};

// --- Constant Pool Entry ---

enum class ConstantTag : uint8_t {
    Integer = 1,
    Long    = 2,
    Float   = 3,
    Double  = 4,
    String  = 5,
    Class   = 6,
    Method  = 7,
};

struct ConstantEntry {
    ConstantTag tag;
    union {
        int32_t intVal;
        int64_t longVal;
        float floatVal;
        double doubleVal;
    };
    std::string strVal; // for String, Class, Method

    ConstantEntry() : tag(ConstantTag::Integer), intVal(0) {}
};

// --- Function Info (in bytecode) ---

struct FunctionInfo {
    std::string name;
    uint16_t paramCount;
    uint16_t localCount;
    uint16_t maxStack;
    uint32_t codeOffset;  // offset into bytecode array
    uint32_t codeLength;
    bool isAsync;
    bool hasRestParam = false;   // last param is ...args
    uint8_t restParamIndex = 0;  // index of the rest param
};

// --- Debug Symbol Entry ---

struct DebugSymbol {
    uint32_t pcOffset;
    uint16_t line;
    uint16_t column;
    std::string functionName;
};

// --- .ga File Format ---
// Header:
//   magic: 4 bytes "GARD"
//   version: 2 bytes (major.minor)
//   flags: 2 bytes
//   constantPoolSize: 4 bytes
//   functionCount: 2 bytes
//   entryFunction: 2 bytes (index of main)
//   codeSize: 4 bytes
// Constant Pool: [ConstantEntry...]
// Function Table: [FunctionInfo...]
// Code: [uint8_t...]
// Debug Symbols (optional): [DebugSymbol...]

struct BytecodeModule {
    // Header
    static constexpr uint8_t MAGIC[4] = {'G', 'A', 'R', 'D'};
    uint16_t versionMajor = 0;
    uint16_t versionMinor = 1;
    uint16_t flags = 0;

    // Content
    std::vector<ConstantEntry> constantPool;
    std::vector<FunctionInfo> functions;
    std::vector<uint8_t> code;
    std::vector<DebugSymbol> debugSymbols;

    // Annotation metadata (class/method annotations accessible at runtime)
    struct AnnotationEntry {
        std::string target;  // "ClassName" or "ClassName.methodName"
        std::string name;    // annotation name
        std::vector<std::pair<std::string, std::string>> args; // key-value pairs
    };
    std::vector<AnnotationEntry> annotations;

    int16_t entryFunction = -1; // index of "main"

    // Helpers
    uint16_t addConstantInt(int32_t val);
    uint16_t addConstantLong(int64_t val);
    uint16_t addConstantFloat(float val);
    uint16_t addConstantDouble(double val);
    uint16_t addConstantString(const std::string& val);

    // Serialization
    std::vector<uint8_t> serialize() const;
    static BytecodeModule deserialize(const std::vector<uint8_t>& data);

    // Verification
    bool verify(std::vector<std::string>& errors) const;
};

} // namespace bytecode
} // namespace gard
