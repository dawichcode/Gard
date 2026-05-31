#pragma once
// GARDVM Tier 4 — Bytecode Interpreter
// Computed-goto threaded dispatch. Handles all opcodes from bytecode.h.
// Bytecode format: big-endian operands (matching existing gard compiler).

#include "value.h"
#include "object.h"
#include "shape.h"
#include "region.h"
#include "gc.h"
#include "../include/bytecode/bytecode.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace gardvm {

// ============================================================================
// Native Function Type
// ============================================================================

using NativeFn = GValue(*)(GValue* args, int32_t argc);

// ============================================================================
// Call Frame
// ============================================================================

struct Frame {
    const uint8_t* returnPC;
    GValue*        bp;           // base pointer (start of this frame's locals)
    uint16_t       funcIndex;
    uint16_t       localCount;
    GClosure*      closure;      // non-null if this is a closure call
};

// ============================================================================
// Interpreter
// ============================================================================

class Interpreter {
public:
    Interpreter();
    ~Interpreter();

    // Load a bytecode module (deserialized from .ga file)
    void load(const gard::bytecode::BytecodeModule& module);

    // Register a native function callable by name
    void registerNative(const std::string& name, NativeFn fn);

    // Register all standard library natives
    void registerStdlib();

    // Execute. Returns exit code.
    int run();

    // Stats
    uint64_t instructionsExecuted() const { return insCount_; }
    GC& gc() { return gc_; }

private:
    // The main dispatch loop
    void execute();

    // Bytecode reading (big-endian)
    uint16_t readU16() { uint16_t v = (pc_[0]<<8)|pc_[1]; pc_+=2; return v; }
    int32_t  readI32() { int32_t v = (pc_[0]<<24)|(pc_[1]<<16)|(pc_[2]<<8)|pc_[3]; pc_+=4; return v; }
    uint8_t  readU8()  { return *pc_++; }

    // Stack
    void push(GValue v) { *sp_++ = v; }
    GValue pop() { return *--sp_; }
    GValue& peek() { return *(sp_-1); }

    // Frames
    void pushFrame(uint16_t funcIdx, uint8_t argc);
    void popFrame();

    // Allocation helpers
    GString* allocString(const char* data, uint32_t len);
    GArray*  allocArray(int32_t capacity);
    GObject* allocObject(uint32_t shapeId);

    // String concat
    GString* concatStrings(GString* a, GString* b);

    // Field access
    GValue getField(GObject* obj, uint16_t nameIdx);
    void   setField(GObject* obj, uint16_t nameIdx, GValue val);

    // Exception
    void throwError(const char* type, const char* message);
    bool unwindToHandler();

    // State
    const gard::bytecode::BytecodeModule* module_ = nullptr;
    const uint8_t* pc_ = nullptr;
    GValue* sp_ = nullptr;
    GValue* fp_ = nullptr;

    GValue* stack_;
    Frame*  frames_;
    Frame*  frameTop_;
    GValue* locals_;
    size_t  localsCapacity_;

    std::unordered_map<std::string, NativeFn> natives_;
    std::unordered_map<std::string, uint16_t> funcLookup_;
    // Method dispatch: key = (shapeId << 16) | methodNameIndex → funcIndex
    std::unordered_map<uint64_t, uint16_t> methodTable_;
    // Map constant pool string index → method name for dispatch
    std::unordered_map<uint16_t, std::vector<std::pair<uint32_t, uint16_t>>> methodByName_; // nameIdx → [(shapeId, funcIdx)]

    ShapeTable shapes_;
    GC gc_;

    // Exception handling
    struct ExHandler {
        const uint8_t* catchPC;
        GValue* savedSP;
        Frame*  savedFrame;
    };
    std::vector<ExHandler> handlers_;
    GValue currentException_ = GVAL_NULL;

    bool running_ = false;
    int exitCode_ = 0;
    uint64_t insCount_ = 0;
    uint16_t currentLine_ = 0;
    uint16_t currentCol_ = 0;
};

} // namespace gardvm
