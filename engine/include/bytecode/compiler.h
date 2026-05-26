#pragma once

#include "bytecode/bytecode.h"
#include "ir/ir.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace gard {
namespace bytecode {

// IR-to-bytecode compiler
class BytecodeCompiler {
public:
    BytecodeCompiler();

    // Compile IR module to bytecode
    BytecodeModule compile(const ir::Module& module);

    const std::vector<std::string>& getErrors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

    // Peephole optimization: fuse opcode sequences into superinstructions
    static void peepholeOptimize(BytecodeModule& module);

private:
    void compileFunction(const ir::Function& fn);
    void compileBlock(const ir::BasicBlock& block);
    void compileInstruction(const ir::Instruction& inst);

    // Emit bytecode
    void emit(OpCode op);
    void emitU8(uint8_t val);
    void emitU16(uint16_t val);
    void emitI32(int32_t val);
    void emitI64(int64_t val);
    void emitF32(float val);
    void emitF64(double val);

    // Patch jump targets
    uint32_t currentOffset() const;
    void patchJump(uint32_t patchAddr, uint32_t target);

    // Local variable management
    uint16_t declareLocal(const std::string& name);
    uint16_t resolveLocal(const std::string& name);

    // Label management for jumps
    uint32_t labelOffset(const std::string& label);
    void recordLabel(const std::string& label, uint32_t offset);

    // State
    BytecodeModule module_;
    uint32_t codeStart_ = 0; // start of current function's code
    std::unordered_map<std::string, uint16_t> locals_;
    uint16_t localCount_ = 0;
    uint16_t maxStack_ = 0;
    uint16_t currentStack_ = 0;

    // Label -> code offset mapping
    std::unordered_map<std::string, uint32_t> labels_;
    // Pending jumps: code offset -> target label
    std::vector<std::pair<uint32_t, std::string>> pendingJumps_;

    // Line tracking for debug info
    int lastEmittedLine_ = 0;
    int lastEmittedColumn_ = 0;

    std::vector<std::string> errors_;
};

} // namespace bytecode
} // namespace gard
