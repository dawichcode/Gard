#pragma once

#include "bytecode/bytecode.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <cstdint>

namespace gard {
namespace jit { class JitDispatcher; }
namespace runtime {

// --- Value types ---

enum class ValueType : uint8_t {
    Null,
    Int,
    Long,
    Float,
    Double,
    Bool,
    Char,
    String,
    Array,
    Map,
    Object,
    Function,
};

// --- Runtime Value (tagged union) ---

struct GardObject;
struct GardArray;
struct GardMap;
struct GardString;

struct Value {
    ValueType type;
    union {
        int32_t intVal;
        int64_t longVal;
        float floatVal;
        double doubleVal;
        bool boolVal;
        char charVal;
    };
    // Heap-allocated types use shared_ptr
    std::shared_ptr<GardString> strVal;
    std::shared_ptr<GardArray> arrVal;
    std::shared_ptr<GardMap> mapVal;
    std::shared_ptr<GardObject> objVal;

    Value() : type(ValueType::Null), intVal(0) {}

    static Value makeNull() { Value v; v.type = ValueType::Null; return v; }
    static Value makeInt(int32_t val) { Value v; v.type = ValueType::Int; v.intVal = val; return v; }
    static Value makeLong(int64_t val) { Value v; v.type = ValueType::Long; v.longVal = val; return v; }
    static Value makeFloat(float val) { Value v; v.type = ValueType::Float; v.floatVal = val; return v; }
    static Value makeDouble(double val) { Value v; v.type = ValueType::Double; v.doubleVal = val; return v; }
    static Value makeBool(bool val) { Value v; v.type = ValueType::Bool; v.boolVal = val; return v; }
    static Value makeChar(char val) { Value v; v.type = ValueType::Char; v.charVal = val; return v; }
    static Value makeString(const std::string& val);
    static Value makeArray();
    static Value makeMap();
    static Value makeObject(const std::string& className);

    // Conversions
    std::string toString() const;
    int32_t toInt() const;
    double toDouble() const;
    bool toBool() const;
};

// --- Heap objects ---

struct GardString {
    std::string data;
    uint32_t refCount = 1;
    GardString(const std::string& s) : data(s) {}
};

struct GardArray {
    std::vector<Value> elements;
    uint32_t refCount = 1;
};

struct GardMap {
    std::unordered_map<std::string, Value> entries;
    uint32_t refCount = 1;
};

struct GardObject {
    std::string className;
    std::unordered_map<std::string, Value> fields;
    uint32_t refCount = 1;
    // vtable index for method dispatch
    int vtableIndex = -1;

    GardObject(const std::string& cls) : className(cls) {
        // Pre-allocate bucket space for typical object size (reduces rehashing)
        fields.reserve(8);
    }
};

// --- Call Frame ---

struct CallFrame {
    uint16_t functionIndex;
    uint32_t returnPC;          // where to return to
    uint16_t basePointer;       // base of locals in the stack
    std::string functionName;
};

// --- VM Configuration ---

struct VMConfig {
    size_t maxStackSize = 65536;    // max operand stack entries
    size_t maxCallDepth = 1024;     // max call frames
    size_t maxHeapSize = 256 * 1024 * 1024; // 256MB
    bool enableGC = true;
    bool debugMode = false;
};

// --- Virtual Machine ---

class VM {
public:
    VM(const VMConfig& config = VMConfig());
    ~VM();

    // Load and run a bytecode module
    int run(const bytecode::BytecodeModule& module);

    // Set program arguments (from main's argc/argv)
    void setArgs(const std::vector<std::string>& args) { programArgs_ = args; }
    const std::vector<std::string>& getArgs() const { return programArgs_; }

    // Set source file for error reporting
    void setSourceFile(const std::string& file) { sourceFile_ = file; }

    // Error throwing (creates error object and dispatches to catch or halts)
    void throwError(const std::string& errorType, const std::string& message);

    // Access natives map (for event system)
    bool hasNative(const std::string& name) const { return natives_.count(name) > 0; }
    Value callNative(const std::string& name, const std::vector<Value>& args) {
        auto it = natives_.find(name);
        if (it != natives_.end()) return it->second(args);
        return Value::makeNull();
    }

    // Call a module function by name with args (for event handlers)
    void callFunction(const std::string& name, const std::vector<Value>& args);

    // Register native functions
    using NativeFunc = std::function<Value(const std::vector<Value>&)>;
    void registerNative(const std::string& name, NativeFunc func);

    // Register a native as "always async" — dispatches to thread pool even without await
    void registerAsyncNative(const std::string& name);
    bool isAsyncNative(const std::string& name) const { return asyncNatives_.count(name) > 0; }

    // Hot loop profiling (for JIT)
    const std::unordered_set<uint32_t>& getHotLoops() const { return hotLoops_; }
    size_t hotLoopCount() const { return hotLoops_.size(); }

    // State access
    size_t heapUsage() const { return heapUsage_; }
    size_t gcCollections() const { return gcCollections_; }

    // JIT bridge accessors (public for extern "C" callback)
    void push(const Value& val);
    Value pop();
    void pushFrame(uint16_t funcIdx, uint8_t argCount);
    void executeInstruction(bytecode::OpCode op);
    bool isRunning() const { return running_; }
    size_t callDepth() const { return callStack_.size(); }
    uint32_t getPC() const { return pc_; }
    void advancePC() { pc_++; }
    bool stackEmpty() const { return stack_.empty(); }

private:
    // Execution
    void execute();

    // Stack operations (push/pop/pushFrame/executeInstruction are public for JIT bridge)
    Value& peek();
    Value& peekAt(int offset);

    // Call frame management
    void popFrame();

    // Built-in functions
    void builtinPrint(const Value& val);

    // Object operations
    Value createObject(const std::string& className);
    Value getField(const Value& obj, const std::string& field);
    void setField(Value& obj, const std::string& field, const Value& val);

    // Memory management
    void collectGarbage();
    void trackAllocation(size_t bytes);

    // Helpers
    uint8_t readU8();
    uint16_t readU16();
    int32_t readI32();
    int64_t readI64();
    float readF32();
    double readF64();
    const std::string& getConstantString(uint16_t index);
    int32_t getConstantInt(uint16_t index);

    // State
    VMConfig config_;
    const bytecode::BytecodeModule* module_ = nullptr;

    // Operand stack
    std::vector<Value> stack_;

    // Call stack
    std::vector<CallFrame> callStack_;

    // Local variables (flat array, indexed by frame base + local index)
    std::vector<Value> locals_;

    // Pre-allocated args buffer for native calls (avoids malloc per call)
    std::vector<Value> argsBuffer_;

    // Program counter
    uint32_t pc_ = 0;

    // Running flag
    bool running_ = false;
    int exitCode_ = 0;

    // Source tracking for error messages
    uint16_t currentLine_ = 0;
    uint16_t currentColumn_ = 0;
    std::string sourceFile_;

    // Exception handling
    struct ExceptionHandler {
        uint32_t catchPC;       // PC to jump to on throw
        uint16_t stackSize;     // stack size to restore
        uint16_t callStackSize; // call stack depth to restore
    };
    std::vector<ExceptionHandler> exceptionHandlers_;
    Value currentException_;    // the thrown value
    bool hasException_ = false;

    // Native functions
    std::unordered_map<std::string, NativeFunc> natives_;

    // Natives that always dispatch to thread pool (fire-and-forget without await)
    std::unordered_set<std::string> asyncNatives_;

    // Memory tracking
    size_t heapUsage_ = 0;
    size_t gcCollections_ = 0;

    // Program arguments
    std::vector<std::string> programArgs_;

    // Function lookup cache: name → index (O(1) instead of O(n) linear scan)
    std::unordered_map<std::string, uint16_t> funcLookup_;

    // Inline cache for CALL_VIRTUAL: per call-site monomorphic cache
    // Key: PC of the CALL_VIRTUAL instruction
    // Value: cached resolution (avoids re-resolving on every call)
    struct InlineCacheEntry {
        std::string cachedClassName;  // receiver type that was cached
        uint16_t cachedFuncIdx = 0;   // resolved function index (module function)
        NativeFunc cachedNative;      // resolved native function
        bool isNative = false;        // true if resolved to native
        bool valid = false;           // cache entry is populated
    };
    std::unordered_map<uint32_t, InlineCacheEntry> inlineCache_;

    // Hot loop detection: count backward jump executions per PC
    // When count exceeds threshold, loop is marked hot (eligible for JIT)
    static constexpr uint32_t HOT_LOOP_THRESHOLD = 1000;
    std::unordered_map<uint32_t, uint32_t> loopCounters_; // PC → execution count
    std::unordered_set<uint32_t> hotLoops_;               // PCs of hot loops

    // String interning: deduplicate constant strings (same content → same pointer)
    std::unordered_map<std::string, std::shared_ptr<GardString>> internedStrings_;

    // JIT engine (optional, created on first use)
    std::unique_ptr<jit::JitDispatcher> jit_;
};

} // namespace runtime
} // namespace gard
