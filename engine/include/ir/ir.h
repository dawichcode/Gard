#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>

namespace gard {
namespace ir {

// Forward declarations
struct BasicBlock;
struct Function;
struct Module;
struct Value;

using ValueRef = std::shared_ptr<Value>;

// --- IR Types ---

enum class IRType {
    Void,
    Int32,
    Int64,
    Int16,
    Float32,
    Float64,
    Bool,
    Char,
    String,
    Pointer,    // generic pointer (for objects)
    Array,
    Map,
    Function,
    Null,
};

struct IRTypeInfo {
    IRType base;
    std::string name;           // for class/struct types
    IRTypeInfo* elementType = nullptr;  // for arrays
    IRTypeInfo* keyType = nullptr;      // for maps
    IRTypeInfo* valueType = nullptr;    // for maps
    std::vector<IRTypeInfo*> paramTypes; // for function types
    IRTypeInfo* returnType = nullptr;

    IRTypeInfo(IRType base) : base(base) {}
    std::string toString() const;
    bool isNumeric() const;
    bool isIntegral() const;
    bool isFloating() const;
};

// --- SSA Values ---

enum class ValueKind {
    Constant,
    Instruction,
    Parameter,
    Global,
    Undef,
};

struct Value {
    ValueKind kind;
    IRType type;
    std::string name;
    int id;

    Value(ValueKind kind, IRType type, const std::string& name, int id)
        : kind(kind), type(type), name(name), id(id) {}
    virtual ~Value() = default;

    std::string ref() const { return "%" + name; }
};

// --- Constants ---

struct ConstantInt : Value {
    int64_t value;
    ConstantInt(int64_t val, IRType type, int id)
        : Value(ValueKind::Constant, type, "c" + std::to_string(id), id), value(val) {}
};

struct ConstantFloat : Value {
    double value;
    ConstantFloat(double val, IRType type, int id)
        : Value(ValueKind::Constant, type, "c" + std::to_string(id), id), value(val) {}
};

struct ConstantBool : Value {
    bool value;
    ConstantBool(bool val, int id)
        : Value(ValueKind::Constant, IRType::Bool, "c" + std::to_string(id), id), value(val) {}
};

struct ConstantString : Value {
    std::string value;
    ConstantString(const std::string& val, int id)
        : Value(ValueKind::Constant, IRType::String, "c" + std::to_string(id), id), value(val) {}
};

struct ConstantNull : Value {
    ConstantNull(int id)
        : Value(ValueKind::Constant, IRType::Null, "null", id) {}
};

// --- Instructions (SSA) ---

enum class Opcode {
    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,

    // Bitwise
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,
    UShr,

    // Comparison
    CmpEq,
    CmpNe,
    CmpLt,
    CmpGt,
    CmpLe,
    CmpGe,

    // Logical
    LogAnd,
    LogOr,
    LogNot,

    // Control flow
    Branch,         // unconditional jump
    CondBranch,     // conditional jump
    Return,
    ReturnVoid,

    // Function calls
    Call,
    CallVirtual,    // virtual method dispatch
    CallAsync,      // async function call

    // Memory / Variables
    Alloca,         // stack allocation
    Load,           // load from address
    Store,          // store to address
    GetField,       // object field access
    SetField,       // object field write
    GetElement,     // array/map index
    SetElement,     // array/map index write

    // Object operations
    NewObject,      // allocate class instance
    NewArray,       // allocate array
    NewMap,         // allocate map

    // Type operations
    Cast,           // type cast
    IsType,         // type check

    // Async
    Await,          // suspend coroutine
    Yield,          // yield from generator
    Resume,         // resume coroutine

    // Closures
    CreateClosure,  // capture environment
    LoadCapture,    // load captured variable

    // Misc
    Phi,            // SSA phi node
    Print,          // built-in print
    Concat,         // string concatenation
    TryBegin,       // push exception handler (target = catch block)
    TryEnd,         // pop exception handler
    Throw,          // throw exception (unwind to handler)
    CatchException, // store caught exception from stack into local
    Nop,            // no operation
};

struct Instruction : Value {
    Opcode opcode;
    std::vector<ValueRef> operands;
    BasicBlock* parent = nullptr;

    // Extra data depending on opcode
    std::string targetName;     // for Call, GetField, etc.
    BasicBlock* trueBlock = nullptr;   // for CondBranch
    BasicBlock* falseBlock = nullptr;  // for CondBranch
    BasicBlock* jumpTarget = nullptr;  // for Branch
    int fieldIndex = -1;        // for GetField/SetField
    int sourceLine = 0;         // source line number for debug/error reporting
    int sourceColumn = 0;       // source column number

    Instruction(Opcode op, IRType resultType, const std::string& name, int id)
        : Value(ValueKind::Instruction, resultType, name, id), opcode(op) {}

    void addOperand(ValueRef val) { operands.push_back(std::move(val)); }
};

// --- Basic Block ---

struct BasicBlock {
    std::string label;
    std::vector<std::shared_ptr<Instruction>> instructions;
    Function* parent = nullptr;

    // Predecessors and successors for CFG
    std::vector<BasicBlock*> predecessors;
    std::vector<BasicBlock*> successors;

    BasicBlock(const std::string& label) : label(label) {}

    Instruction* getTerminator() const {
        if (instructions.empty()) return nullptr;
        auto& last = instructions.back();
        if (last->opcode == Opcode::Branch || last->opcode == Opcode::CondBranch ||
            last->opcode == Opcode::Return || last->opcode == Opcode::ReturnVoid) {
            return last.get();
        }
        return nullptr;
    }

    bool isTerminated() const { return getTerminator() != nullptr; }
};

// --- Function ---

struct Parameter : Value {
    int index;
    Parameter(IRType type, const std::string& name, int index, int id)
        : Value(ValueKind::Parameter, type, name, id), index(index) {}
};

struct Function {
    std::string name;
    IRType returnType;
    std::vector<std::shared_ptr<Parameter>> params;
    std::vector<std::unique_ptr<BasicBlock>> blocks;
    bool isAsync = false;
    bool isExported = false;
    bool hasRestParam = false;
    uint8_t restParamIndex = 0;

    // Captured variables (for closures)
    std::vector<std::string> captures;

    Function(const std::string& name, IRType retType)
        : name(name), returnType(retType) {}

    BasicBlock* createBlock(const std::string& label) {
        auto block = std::make_unique<BasicBlock>(label);
        block->parent = this;
        BasicBlock* ptr = block.get();
        blocks.push_back(std::move(block));
        return ptr;
    }

    BasicBlock* entryBlock() const {
        return blocks.empty() ? nullptr : blocks[0].get();
    }
};

// --- Global Variable ---

struct Global : Value {
    ValueRef initializer;
    bool isConstant = false;

    Global(IRType type, const std::string& name, int id)
        : Value(ValueKind::Global, type, name, id) {}
};

// --- Module (top-level IR unit) ---

struct Module {
    std::string name;
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::shared_ptr<Global>> globals;
    std::vector<std::shared_ptr<ConstantString>> stringPool;

    // Class metadata
    struct ClassInfo {
        std::string name;
        std::string baseClass;
        std::vector<std::string> fieldNames;
        std::vector<IRType> fieldTypes;
        std::vector<std::string> fieldTypeNames; // original type names (for enum detection)
        std::vector<std::string> methodNames;
        // Annotation metadata
        struct AnnotationMeta {
            std::string target; // "" for class-level, "methodName" for method-level
            std::string name;
            std::vector<std::pair<std::string, std::string>> args;
        };
        std::vector<AnnotationMeta> annotations;
    };
    std::vector<ClassInfo> classes;

    Module(const std::string& name) : name(name) {}

    Function* createFunction(const std::string& name, IRType retType) {
        auto fn = std::make_unique<Function>(name, retType);
        Function* ptr = fn.get();
        functions.push_back(std::move(fn));
        return ptr;
    }

    // Print IR for debugging
    std::string dump() const;
};

} // namespace ir
} // namespace gard
