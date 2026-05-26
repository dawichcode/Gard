#pragma once

#include "bytecode/bytecode.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

namespace gard {
namespace runtime { class VM; }
namespace jit {

// JIT-compiled function signature: takes args array + count, returns i64
using JitFuncPtr = int64_t(*)(int64_t*, int32_t);

// Per-function JIT state
struct JitFunctionInfo {
    uint16_t funcIndex = 0;
    int executionCount = 0;
    JitFuncPtr compiledPtr = nullptr;
    bool compilationFailed = false;
    bool isHot = false;
};

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    // Initialize LLVM targets (call once at startup)
    static void initialize();

    // Check if JIT is available
    bool isAvailable() const { return jit_ != nullptr; }

    // Record a function execution (for hotness detection)
    void recordExecution(uint16_t funcIndex);

    // Check if a function has been JIT-compiled
    JitFuncPtr getCompiledFunction(uint16_t funcIndex);

    bool compileFunction(uint16_t funcIndex, const bytecode::BytecodeModule& module, runtime::VM& vm);

    // Get JIT compilation threshold
    int getThreshold() const { return compilationThreshold_; }
    void setThreshold(int t) { compilationThreshold_ = t; }

    // Get function info (for threshold checking)
    JitFunctionInfo& getFunctionInfo(uint16_t funcIndex) { return functions_[funcIndex]; }

    // Stats
    int totalCompiled() const { return totalCompiled_; }
    int totalFailed() const { return totalFailed_; }

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::unordered_map<uint16_t, JitFunctionInfo> functions_;
    int compilationThreshold_ = 100; // compile after N executions
    int totalCompiled_ = 0;
    int totalFailed_ = 0;

    // LLVM IR generation for a single function
    std::unique_ptr<llvm::Module> generateIR(
        uint16_t funcIndex,
        const bytecode::BytecodeModule& module,
        runtime::VM& vm,
        llvm::LLVMContext& ctx);
};

} // namespace jit
} // namespace gard
