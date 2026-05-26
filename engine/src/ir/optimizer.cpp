#include "ir/optimizer.h"
#include <algorithm>
#include <queue>

namespace gard {
namespace ir {

// ============================================================
// Dead Code Elimination
// ============================================================

bool DeadCodeElimination::runOnFunction(Function& fn) {
    bool changed = false;

    // Simple DCE: remove Nop instructions
    for (auto& block : fn.blocks) {
        auto& insts = block->instructions;
        size_t writeIdx = 0;
        for (size_t readIdx = 0; readIdx < insts.size(); readIdx++) {
            if (insts[readIdx]->opcode == Opcode::Nop) {
                changed = true;
                continue;
            }
            if (writeIdx != readIdx) {
                insts[writeIdx] = std::move(insts[readIdx]);
            }
            writeIdx++;
        }
        insts.resize(writeIdx);
    }

    return changed;
}

bool DeadCodeElimination::isUsed(const Value* val, const Function& fn) const {
    for (auto& block : fn.blocks) {
        for (auto& inst : block->instructions) {
            for (auto& operand : inst->operands) {
                if (operand && operand.get() == val) return true;
            }
        }
    }
    return false;
}

bool DeadCodeElimination::hasSideEffects(const Instruction* inst) const {
    switch (inst->opcode) {
        case Opcode::Store:
        case Opcode::SetField:
        case Opcode::SetElement:
        case Opcode::Call:
        case Opcode::CallVirtual:
        case Opcode::CallAsync:
        case Opcode::Print:
        case Opcode::NewObject:
        case Opcode::NewArray:
        case Opcode::NewMap:
        case Opcode::Await:
        case Opcode::Yield:
            return true;
        default:
            return false;
    }
}

// ============================================================
// Constant Folding
// ============================================================

bool ConstantFolding::runOnFunction(Function& fn) {
    bool changed = false;

    // Collect replacements first, then apply
    std::vector<std::pair<Value*, ValueRef>> replacements;

    for (auto& block : fn.blocks) {
        for (auto& inst : block->instructions) {
            // Only fold arithmetic/comparison/bitwise operations
            bool foldable = false;
            switch (inst->opcode) {
                case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
                case Opcode::Div: case Opcode::Mod:
                case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
                case Opcode::Shl: case Opcode::Shr: case Opcode::UShr:
                case Opcode::CmpEq: case Opcode::CmpNe:
                case Opcode::CmpLt: case Opcode::CmpGt:
                case Opcode::CmpLe: case Opcode::CmpGe:
                case Opcode::Neg: case Opcode::BitNot:
                    foldable = true; break;
                default: break;
            }
            if (!foldable) continue;

            if (inst->operands.size() == 2) {
                auto* leftInt = dynamic_cast<ConstantInt*>(inst->operands[0].get());
                auto* rightInt = dynamic_cast<ConstantInt*>(inst->operands[1].get());
                auto* leftFloat = dynamic_cast<ConstantFloat*>(inst->operands[0].get());
                auto* rightFloat = dynamic_cast<ConstantFloat*>(inst->operands[1].get());

                ValueRef folded = nullptr;

                if (leftInt && rightInt) {
                    folded = foldBinary(inst->opcode, leftInt, rightInt);
                    if (!folded) folded = foldComparison(inst->opcode, leftInt, rightInt);
                } else if (leftFloat && rightFloat) {
                    folded = foldBinaryFloat(inst->opcode, leftFloat, rightFloat);
                }

                if (folded) {
                    replacements.push_back({inst.get(), folded});
                }
            } else if (inst->operands.size() == 1) {
                auto* operandInt = dynamic_cast<ConstantInt*>(inst->operands[0].get());
                if (operandInt) {
                    auto folded = foldUnary(inst->opcode, operandInt);
                    if (folded) {
                        replacements.push_back({inst.get(), folded});
                    }
                }
            }
        }
    }

    // Apply replacements: replace uses of the folded instruction with the constant
    for (auto& [target, replacement] : replacements) {
        for (auto& block : fn.blocks) {
            for (auto& inst : block->instructions) {
                if (inst.get() == target) continue; // skip the instruction itself
                for (size_t i = 0; i < inst->operands.size(); i++) {
                    if (inst->operands[i].get() == target) {
                        inst->operands[i] = replacement;
                        changed = true;
                    }
                }
            }
        }
    }

    return changed;
}

ValueRef ConstantFolding::foldBinary(Opcode op, ConstantInt* left, ConstantInt* right) {
    int64_t l = left->value, r = right->value;
    int64_t result;

    switch (op) {
        case Opcode::Add: result = l + r; break;
        case Opcode::Sub: result = l - r; break;
        case Opcode::Mul: result = l * r; break;
        case Opcode::Div: if (r == 0) return nullptr; result = l / r; break;
        case Opcode::Mod: if (r == 0) return nullptr; result = l % r; break;
        case Opcode::BitAnd: result = l & r; break;
        case Opcode::BitOr: result = l | r; break;
        case Opcode::BitXor: result = l ^ r; break;
        case Opcode::Shl: result = l << r; break;
        case Opcode::Shr: result = l >> r; break;
        default: return nullptr;
    }

    return std::make_shared<ConstantInt>(result, left->type, 0);
}

ValueRef ConstantFolding::foldBinaryFloat(Opcode op, ConstantFloat* left, ConstantFloat* right) {
    double l = left->value, r = right->value;
    double result;

    switch (op) {
        case Opcode::Add: result = l + r; break;
        case Opcode::Sub: result = l - r; break;
        case Opcode::Mul: result = l * r; break;
        case Opcode::Div: if (r == 0.0) return nullptr; result = l / r; break;
        default: return nullptr;
    }

    return std::make_shared<ConstantFloat>(result, left->type, 0);
}

ValueRef ConstantFolding::foldComparison(Opcode op, ConstantInt* left, ConstantInt* right) {
    int64_t l = left->value, r = right->value;
    bool result;

    switch (op) {
        case Opcode::CmpEq: result = (l == r); break;
        case Opcode::CmpNe: result = (l != r); break;
        case Opcode::CmpLt: result = (l < r); break;
        case Opcode::CmpGt: result = (l > r); break;
        case Opcode::CmpLe: result = (l <= r); break;
        case Opcode::CmpGe: result = (l >= r); break;
        default: return nullptr;
    }

    return std::make_shared<ConstantBool>(result, 0);
}

ValueRef ConstantFolding::foldUnary(Opcode op, ConstantInt* operand) {
    int64_t val = operand->value;

    switch (op) {
        case Opcode::Neg: return std::make_shared<ConstantInt>(-val, operand->type, 0);
        case Opcode::BitNot: return std::make_shared<ConstantInt>(~val, operand->type, 0);
        default: return nullptr;
    }
}

// ============================================================
// Constant Propagation
// ============================================================

bool ConstantPropagation::runOnFunction(Function& fn) {
    bool changed = false;

    // Track: alloca address (by pointer identity) -> stored constant
    std::unordered_map<Value*, ValueRef> constants;

    for (auto& block : fn.blocks) {
        for (size_t idx = 0; idx < block->instructions.size(); idx++) {
            auto& inst = block->instructions[idx];

            // Track stores of constants
            if (inst->opcode == Opcode::Store && inst->operands.size() == 2) {
                auto* addr = inst->operands[0].get();
                auto* val = inst->operands[1].get();
                if (val->kind == ValueKind::Constant) {
                    constants[addr] = inst->operands[1];
                } else {
                    constants.erase(addr);
                }
            }
            // Replace loads with known constants
            if (inst->opcode == Opcode::Load && inst->operands.size() == 1) {
                auto* addr = inst->operands[0].get();
                auto it = constants.find(addr);
                if (it != constants.end()) {
                    // Replace all subsequent uses of this load with the constant
                    ValueRef replacement = it->second;
                    Value* loadVal = inst.get();
                    for (auto& b : fn.blocks) {
                        for (auto& i : b->instructions) {
                            for (size_t oi = 0; oi < i->operands.size(); oi++) {
                                if (i->operands[oi].get() == loadVal) {
                                    i->operands[oi] = replacement;
                                    changed = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return changed;
}

// ============================================================
// Common Subexpression Elimination
// ============================================================

bool CSE::runOnFunction(Function& fn) {
    bool changed = false;

    for (auto& block : fn.blocks) {
        std::unordered_map<std::string, std::shared_ptr<Instruction>> seen;

        for (auto& inst : block->instructions) {
            // Only pure computations
            if (inst->opcode == Opcode::Store || inst->opcode == Opcode::Load ||
                inst->opcode == Opcode::Call || inst->opcode == Opcode::CallVirtual ||
                inst->opcode == Opcode::Print || inst->opcode == Opcode::Branch ||
                inst->opcode == Opcode::CondBranch || inst->opcode == Opcode::Return ||
                inst->opcode == Opcode::ReturnVoid || inst->opcode == Opcode::Alloca ||
                inst->opcode == Opcode::NewObject || inst->opcode == Opcode::NewArray ||
                inst->opcode == Opcode::Nop) {
                continue;
            }

            std::string key = computeKey(inst.get());
            if (key.empty()) continue;

            auto it = seen.find(key);
            if (it != seen.end()) {
                // Replace all uses of this instruction with the earlier one
                for (auto& b : fn.blocks) {
                    for (auto& i : b->instructions) {
                        for (auto& op : i->operands) {
                            if (op.get() == inst.get()) {
                                op = it->second;
                                changed = true;
                            }
                        }
                    }
                }
                inst->opcode = Opcode::Nop;
                inst->operands.clear();
            } else {
                seen[key] = inst;
            }
        }
    }

    return changed;
}

std::string CSE::computeKey(const Instruction* inst) const {
    std::string key = std::to_string(static_cast<int>(inst->opcode));
    for (auto& op : inst->operands) {
        key += ":" + std::to_string(op->id);
    }
    return key;
}

// ============================================================
// Tail Call Optimization
// ============================================================

bool TailCallOpt::runOnFunction(Function& fn) {
    bool changed = false;

    for (auto& block : fn.blocks) {
        if (block->instructions.size() < 2) continue;

        auto& insts = block->instructions;
        for (size_t i = 0; i + 1 < insts.size(); i++) {
            if (isTailCall(insts[i].get(), *block)) {
                // Mark as tail call (in a real impl, this would change calling convention)
                insts[i]->targetName = "tail:" + insts[i]->targetName;
                changed = true;
            }
        }
    }

    return changed;
}

bool TailCallOpt::isTailCall(const Instruction* call, const BasicBlock& block) const {
    if (call->opcode != Opcode::Call) return false;

    // Check if the next instruction is a return of this call's result
    auto& insts = block.instructions;
    for (size_t i = 0; i < insts.size(); i++) {
        if (insts[i].get() == call && i + 1 < insts.size()) {
            auto* next = insts[i + 1].get();
            if (next->opcode == Opcode::Return && next->operands.size() == 1 &&
                next->operands[0].get() == call) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================
// Strength Reduction
// ============================================================

bool StrengthReduction::runOnFunction(Function& fn) {
    bool changed = false;

    for (auto& block : fn.blocks) {
        for (auto& inst : block->instructions) {
            if (inst->operands.size() != 2) continue;
            if (!inst->operands[0] || !inst->operands[1]) continue;

            if (inst->opcode == Opcode::Mul) {
                // x * 2 => x << 1, x * 4 => x << 2, etc.
                auto* rightConst = dynamic_cast<ConstantInt*>(inst->operands[1].get());
                if (rightConst) {
                    int64_t val = rightConst->value;
                    if (val > 0 && (val & (val - 1)) == 0) {
                        int shift = 0;
                        int64_t tmp = val;
                        while (tmp > 1) { tmp >>= 1; shift++; }
                        inst->opcode = Opcode::Shl;
                        inst->operands[1] = std::make_shared<ConstantInt>(shift, IRType::Int32, 0);
                        changed = true;
                    }
                }
                auto* leftConst = dynamic_cast<ConstantInt*>(inst->operands[0].get());
                if (leftConst && !rightConst) {
                    int64_t val = leftConst->value;
                    if (val > 0 && (val & (val - 1)) == 0) {
                        int shift = 0;
                        int64_t tmp = val;
                        while (tmp > 1) { tmp >>= 1; shift++; }
                        inst->opcode = Opcode::Shl;
                        inst->operands[0] = inst->operands[1];
                        inst->operands[1] = std::make_shared<ConstantInt>(shift, IRType::Int32, 0);
                        changed = true;
                    }
                }
            }
            if (inst->opcode == Opcode::Div) {
                auto* rightConst = dynamic_cast<ConstantInt*>(inst->operands[1].get());
                if (rightConst && rightConst->value > 0 &&
                    (rightConst->value & (rightConst->value - 1)) == 0) {
                    int shift = 0;
                    int64_t tmp = rightConst->value;
                    while (tmp > 1) { tmp >>= 1; shift++; }
                    inst->opcode = Opcode::Shr;
                    inst->operands[1] = std::make_shared<ConstantInt>(shift, IRType::Int32, 0);
                    changed = true;
                }
            }
        }
    }

    return changed;
}

// ============================================================
// Branch Simplification
// ============================================================

bool BranchSimplification::runOnFunction(Function& fn) {
    bool changed = false;

    for (auto& block : fn.blocks) {
        if (block->instructions.empty()) continue;

        auto& last = block->instructions.back();

        // CondBranch with constant condition
        if (last->opcode == Opcode::CondBranch && last->operands.size() == 1) {
            auto* condConst = dynamic_cast<ConstantBool*>(last->operands[0].get());
            if (condConst) {
                BasicBlock* target = condConst->value ? last->trueBlock : last->falseBlock;
                last->opcode = Opcode::Branch;
                last->operands.clear();
                last->jumpTarget = target;
                last->trueBlock = nullptr;
                last->falseBlock = nullptr;
                changed = true;
            }
        }

        // Branch to block that only contains a branch (chain elimination)
        if (last->opcode == Opcode::Branch && last->jumpTarget) {
            auto* target = last->jumpTarget;
            if (target->instructions.size() == 1 &&
                target->instructions[0]->opcode == Opcode::Branch) {
                last->jumpTarget = target->instructions[0]->jumpTarget;
                changed = true;
            }
        }
    }

    return changed;
}

// ============================================================
// LICM (Loop-Invariant Code Motion) — simplified
// ============================================================

bool LICM::runOnFunction(Function& fn) {
    // Simplified: detect back-edges and hoist invariant computations
    // Full implementation would need dominator tree
    return false; // placeholder — complex analysis deferred
}

std::vector<std::unordered_set<BasicBlock*>> LICM::findLoops(Function& fn) {
    // Simplified loop detection via back-edges
    std::vector<std::unordered_set<BasicBlock*>> loops;
    // Would need DFS + back-edge detection
    return loops;
}

bool LICM::isLoopInvariant(const Instruction* inst,
                           const std::unordered_set<BasicBlock*>& loopBlocks) const {
    // An instruction is loop-invariant if all its operands are defined outside the loop
    for (auto& op : inst->operands) {
        if (auto* opInst = dynamic_cast<Instruction*>(op.get())) {
            if (opInst->parent && loopBlocks.count(opInst->parent)) {
                return false;
            }
        }
    }
    return true;
}

// ============================================================
// Partial Evaluation — specialize generic code
// ============================================================

bool PartialEvaluation::runOnFunction(Function& fn) {
    bool changed = false;

    for (auto& block : fn.blocks) {
        for (auto it = block->instructions.begin(); it != block->instructions.end(); ++it) {
            auto& inst = *it;

            // Fold redundant casts: cast(cast(x, T), T) → cast(x, T)
            if (inst->opcode == Opcode::Cast && inst->operands.size() >= 2) {
                auto* inner = dynamic_cast<Instruction*>(inst->operands[0].get());
                if (inner && inner->opcode == Opcode::Cast && inner->operands.size() >= 2) {
                    auto* outerType = dynamic_cast<ConstantString*>(inst->operands[1].get());
                    auto* innerType = dynamic_cast<ConstantString*>(inner->operands[1].get());
                    if (outerType && innerType && outerType->value == innerType->value) {
                        // Redundant double cast — eliminate outer, use inner's result
                        inst->operands[0] = inner->operands[0];
                        changed = true;
                    }
                }
            }

            // Specialize GetField on known object types: store-to-load forwarding
            if (inst->opcode == Opcode::GetField && inst->operands.size() >= 1) {
                auto* obj = dynamic_cast<Instruction*>(inst->operands[0].get());
                if (obj && obj->opcode == Opcode::NewObject) {
                    for (auto rit = block->instructions.begin(); rit != it; ++rit) {
                        auto& prev = *rit;
                        if (prev->opcode == Opcode::SetField &&
                            prev->targetName == inst->targetName &&
                            prev->operands.size() >= 2 &&
                            prev->operands[0].get() == obj) {
                            // Replace GetField with the stored value (load forwarding)
                            inst->opcode = Opcode::Load;
                            inst->operands = {prev->operands[1]};
                            changed = true;
                            break;
                        }
                    }
                }
            }

            // Eliminate redundant NewObject + immediate discard (unused allocations)
            if (inst->opcode == Opcode::NewObject) {
                bool used = false;
                for (auto checkIt = std::next(it); checkIt != block->instructions.end(); ++checkIt) {
                    for (auto& op : (*checkIt)->operands) {
                        if (op.get() == inst.get()) { used = true; break; }
                    }
                    if (used) break;
                }
                // Also check other blocks
                if (!used) {
                    for (auto& otherBlock : fn.blocks) {
                        if (otherBlock.get() == block.get()) continue;
                        for (auto& otherInst : otherBlock->instructions) {
                            for (auto& op : otherInst->operands) {
                                if (op.get() == inst.get()) { used = true; break; }
                            }
                            if (used) break;
                        }
                        if (used) break;
                    }
                }
                if (!used) {
                    inst->opcode = Opcode::Nop;
                    inst->operands.clear();
                    changed = true;
                }
            }
        }
    }

    return changed;
}

// ============================================================
// Pass Pipeline
// ============================================================

PassPipeline::PassPipeline(OptLevel level) : level_(level) {
    switch (level) {
        case OptLevel::None:
            break;
        case OptLevel::Debug:
            passes_.push_back(std::make_unique<ConstantFolding>());
            passes_.push_back(std::make_unique<DeadCodeElimination>());
            break;
        case OptLevel::Release:
            passes_.push_back(std::make_unique<ConstantFolding>());
            passes_.push_back(std::make_unique<ConstantPropagation>());
            passes_.push_back(std::make_unique<StrengthReduction>());
            passes_.push_back(std::make_unique<PartialEvaluation>());
            passes_.push_back(std::make_unique<CSE>());
            passes_.push_back(std::make_unique<LICM>());
            passes_.push_back(std::make_unique<BranchSimplification>());
            passes_.push_back(std::make_unique<TailCallOpt>());
            passes_.push_back(std::make_unique<DeadCodeElimination>());
            break;
    }
}

void PassPipeline::run(Module& module) {
    totalChanges_ = 0;

    for (auto& pass : passes_) {
        if (auto* inliner = dynamic_cast<FunctionInlining*>(pass.get())) {
            inliner->setModule(&module);
        }
    }

    // Run passes once (iterative fixpoint can cause issues with shared_ptr lifetimes)
    for (auto& fn : module.functions) {
        for (auto& pass : passes_) {
            if (pass->runOnFunction(*fn)) {
                totalChanges_++;
            }
        }
    }
}

void PassPipeline::addPass(std::unique_ptr<OptimizationPass> pass) {
    passes_.push_back(std::move(pass));
}

void PassPipeline::clear() {
    passes_.clear();
}

// ============================================================
// Function Inlining (simplified)
// ============================================================

bool FunctionInlining::runOnFunction(Function& fn) {
    if (!module_) return false;
    // Simplified: mark small functions as inlineable
    // Full inlining would copy the callee's blocks into the caller
    return false;
}

bool FunctionInlining::shouldInline(const Function* callee) const {
    if (!callee) return false;
    int instCount = 0;
    for (auto& block : callee->blocks) {
        instCount += static_cast<int>(block->instructions.size());
    }
    return instCount <= maxInlineSize_;
}

} // namespace ir
} // namespace gard
