#include "jit/jit_bridge.h"
#include "jit/jit_engine.h"

namespace gard {
namespace jit {

JitDispatcher::JitDispatcher() {
    JitEngine::initialize();
    engine_ = std::make_unique<JitEngine>();
}

JitDispatcher::~JitDispatcher() = default;

void JitDispatcher::recordExecution(uint16_t funcIndex) {
    engine_->recordExecution(funcIndex);
}

void* JitDispatcher::getCompiledFunction(uint16_t funcIndex) {
    return reinterpret_cast<void*>(engine_->getCompiledFunction(funcIndex));
}

bool JitDispatcher::shouldCompile(uint16_t funcIndex) {
    auto& info = engine_->getFunctionInfo(funcIndex);
    return info.executionCount >= engine_->getThreshold() &&
           !info.compilationFailed && !info.compiledPtr;
}

bool JitDispatcher::compile(uint16_t funcIndex, const bytecode::BytecodeModule& module, runtime::VM& vm) {
    return engine_->compileFunction(funcIndex, module, vm);
}

int JitDispatcher::getThreshold() const {
    return engine_->getThreshold();
}

} // namespace jit
} // namespace gard
