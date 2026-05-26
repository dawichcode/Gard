#pragma once

#include "ir/ir.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace gard {
namespace ir {

// --- Optimization pass interface ---

class OptimizationPass {
public:
    virtual ~OptimizationPass() = default;
    virtual std::string name() const = 0;
    virtual bool runOnFunction(Function& fn) = 0;
};

// --- Dead Code Elimination ---

class DeadCodeElimination : public OptimizationPass {
public:
    std::string name() const override { return "dce"; }
    bool runOnFunction(Function& fn) override;

private:
    bool isUsed(const Value* val, const Function& fn) const;
    bool hasSideEffects(const Instruction* inst) const;
};

// --- Constant Folding and Propagation ---

class ConstantFolding : public OptimizationPass {
public:
    std::string name() const override { return "constfold"; }
    bool runOnFunction(Function& fn) override;

private:
    ValueRef foldBinary(Opcode op, ConstantInt* left, ConstantInt* right);
    ValueRef foldBinaryFloat(Opcode op, ConstantFloat* left, ConstantFloat* right);
    ValueRef foldComparison(Opcode op, ConstantInt* left, ConstantInt* right);
    ValueRef foldUnary(Opcode op, ConstantInt* operand);
};

// --- Constant Propagation ---

class ConstantPropagation : public OptimizationPass {
public:
    std::string name() const override { return "constprop"; }
    bool runOnFunction(Function& fn) override;
};

// --- Function Inlining ---

class FunctionInlining : public OptimizationPass {
public:
    std::string name() const override { return "inline"; }
    bool runOnFunction(Function& fn) override;

    void setModule(Module* mod) { module_ = mod; }
    void setMaxSize(int size) { maxInlineSize_ = size; }

private:
    bool shouldInline(const Function* callee) const;
    Module* module_ = nullptr;
    int maxInlineSize_ = 20; // max instructions to inline
};

// --- Loop-Invariant Code Motion ---

class LICM : public OptimizationPass {
public:
    std::string name() const override { return "licm"; }
    bool runOnFunction(Function& fn) override;

private:
    bool isLoopInvariant(const Instruction* inst,
                         const std::unordered_set<BasicBlock*>& loopBlocks) const;
    std::vector<std::unordered_set<BasicBlock*>> findLoops(Function& fn);
};

// --- Common Subexpression Elimination ---

class CSE : public OptimizationPass {
public:
    std::string name() const override { return "cse"; }
    bool runOnFunction(Function& fn) override;

private:
    std::string computeKey(const Instruction* inst) const;
};

// --- Tail Call Optimization ---

class TailCallOpt : public OptimizationPass {
public:
    std::string name() const override { return "tailcall"; }
    bool runOnFunction(Function& fn) override;

private:
    bool isTailCall(const Instruction* call, const BasicBlock& block) const;
};

// --- Strength Reduction ---

class StrengthReduction : public OptimizationPass {
public:
    std::string name() const override { return "strength"; }
    bool runOnFunction(Function& fn) override;
};

// --- Branch Simplification ---

class BranchSimplification : public OptimizationPass {
public:
    std::string name() const override { return "branchsimp"; }
    bool runOnFunction(Function& fn) override;
};

// --- Partial Evaluation (specialize generic code) ---

class PartialEvaluation : public OptimizationPass {
public:
    std::string name() const override { return "partialeval"; }
    bool runOnFunction(Function& fn) override;
};

// --- Pass Pipeline ---

enum class OptLevel {
    None,   // -O0: no optimizations
    Debug,  // -O1: basic optimizations (constfold, dce)
    Release,// -O2: full optimizations
};

class PassPipeline {
public:
    PassPipeline(OptLevel level = OptLevel::Release);

    void run(Module& module);

    // Manual pass management
    void addPass(std::unique_ptr<OptimizationPass> pass);
    void clear();

    int totalChanges() const { return totalChanges_; }

private:
    std::vector<std::unique_ptr<OptimizationPass>> passes_;
    OptLevel level_;
    int totalChanges_ = 0;
};

} // namespace ir
} // namespace gard
