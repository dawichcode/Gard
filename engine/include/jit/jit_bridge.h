#pragma once
// Thin JIT interface — no LLVM headers, safe to include from runtime.cpp

#include <cstdint>
#include <memory>

namespace gard {
namespace bytecode { struct BytecodeModule; }
namespace runtime { class VM; }

namespace jit {

struct JitInfo {
    int executionCount = 0;
    bool compilationFailed = false;
    void* compiledPtr = nullptr;
};

class JitEngine; // opaque — defined in jit_engine.h with LLVM

// Thin wrapper that hides LLVM from the caller
class JitDispatcher {
public:
    JitDispatcher();
    ~JitDispatcher();

    void recordExecution(uint16_t funcIndex);
    void* getCompiledFunction(uint16_t funcIndex);
    bool shouldCompile(uint16_t funcIndex);
    bool compile(uint16_t funcIndex, const bytecode::BytecodeModule& module, runtime::VM& vm);
    int getThreshold() const;

private:
    std::unique_ptr<JitEngine> engine_;
};

} // namespace jit
} // namespace gard
