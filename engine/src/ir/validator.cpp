#include "ir/validator.h"
#include <unordered_set>

namespace gard {
namespace ir {

bool IRValidator::validate(const Module& module) {
    errors_.clear();

    for (auto& fn : module.functions) {
        validateFunction(*fn);
    }

    return errors_.empty();
}

void IRValidator::validateFunction(const Function& fn) {
    if (fn.name.empty()) {
        error("Function has empty name");
    }

    if (fn.blocks.empty()) {
        error("Function '" + fn.name + "' has no basic blocks");
        return;
    }

    // Check all blocks have unique labels
    std::unordered_set<std::string> labels;
    for (auto& block : fn.blocks) {
        if (labels.count(block->label)) {
            error("Duplicate block label '" + block->label + "' in function '" + fn.name + "'");
        }
        labels.insert(block->label);
    }

    // Validate each block
    for (auto& block : fn.blocks) {
        validateBlock(*block, fn);
    }
}

void IRValidator::validateBlock(const BasicBlock& block, const Function& fn) {
    if (block.label.empty()) {
        error("Block has empty label in function '" + fn.name + "'");
    }

    // Check terminator
    checkTerminator(block, fn);

    // Validate instructions
    for (auto& inst : block.instructions) {
        validateInstruction(*inst, block);
    }

    // Ensure no instructions after terminator
    bool seenTerminator = false;
    for (auto& inst : block.instructions) {
        if (seenTerminator) {
            error("Instruction after terminator in block '" + block.label +
                  "' of function '" + fn.name + "'");
            break;
        }
        if (inst->opcode == Opcode::Branch || inst->opcode == Opcode::CondBranch ||
            inst->opcode == Opcode::Return || inst->opcode == Opcode::ReturnVoid) {
            seenTerminator = true;
        }
    }
}

void IRValidator::validateInstruction(const Instruction& inst, const BasicBlock& block) {
    checkOperandCount(inst);

    // Type-specific checks
    switch (inst.opcode) {
        case Opcode::CondBranch:
            if (!inst.trueBlock || !inst.falseBlock) {
                error("CondBranch missing target blocks in block '" + block.label + "'");
            }
            break;
        case Opcode::Branch:
            if (!inst.jumpTarget) {
                error("Branch missing target in block '" + block.label + "'");
            }
            break;
        case Opcode::Phi:
            if (inst.operands.size() < 2) {
                error("Phi node needs at least 2 operands in block '" + block.label + "'");
            }
            break;
        default:
            break;
    }
}

void IRValidator::checkTerminator(const BasicBlock& block, const Function& fn) {
    if (block.instructions.empty()) {
        error("Block '" + block.label + "' in function '" + fn.name + "' has no instructions");
        return;
    }

    auto* last = block.instructions.back().get();
    if (last->opcode != Opcode::Branch && last->opcode != Opcode::CondBranch &&
        last->opcode != Opcode::Return && last->opcode != Opcode::ReturnVoid) {
        error("Block '" + block.label + "' in function '" + fn.name +
              "' is not terminated (last instruction: " + std::to_string(static_cast<int>(last->opcode)) + ")");
    }
}

void IRValidator::checkOperandCount(const Instruction& inst) {
    switch (inst.opcode) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div:
        case Opcode::Mod:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::Shl:
        case Opcode::Shr:
        case Opcode::UShr:
        case Opcode::CmpEq:
        case Opcode::CmpNe:
        case Opcode::CmpLt:
        case Opcode::CmpGt:
        case Opcode::CmpLe:
        case Opcode::CmpGe:
        case Opcode::LogAnd:
        case Opcode::LogOr:
            if (inst.operands.size() != 2) {
                error("Binary instruction expects 2 operands, got " +
                      std::to_string(inst.operands.size()));
            }
            break;
        case Opcode::Neg:
        case Opcode::BitNot:
        case Opcode::LogNot:
        case Opcode::Load:
        case Opcode::Await:
            if (inst.operands.size() != 1) {
                error("Unary instruction expects 1 operand, got " +
                      std::to_string(inst.operands.size()));
            }
            break;
        case Opcode::Store:
            if (inst.operands.size() != 2) {
                error("Store expects 2 operands (addr, value), got " +
                      std::to_string(inst.operands.size()));
            }
            break;
        case Opcode::CondBranch:
            if (inst.operands.size() != 1) {
                error("CondBranch expects 1 operand (condition), got " +
                      std::to_string(inst.operands.size()));
            }
            break;
        default:
            break;
    }
}

void IRValidator::checkBranchTargets(const Instruction& inst, const Function& fn) {
    auto blockExists = [&](BasicBlock* target) -> bool {
        for (auto& block : fn.blocks) {
            if (block.get() == target) return true;
        }
        return false;
    };

    if (inst.opcode == Opcode::Branch && inst.jumpTarget) {
        if (!blockExists(inst.jumpTarget)) {
            error("Branch target not found in function '" + fn.name + "'");
        }
    }
    if (inst.opcode == Opcode::CondBranch) {
        if (inst.trueBlock && !blockExists(inst.trueBlock)) {
            error("CondBranch true target not found in function '" + fn.name + "'");
        }
        if (inst.falseBlock && !blockExists(inst.falseBlock)) {
            error("CondBranch false target not found in function '" + fn.name + "'");
        }
    }
}

void IRValidator::error(const std::string& msg) {
    errors_.push_back("IR validation: " + msg);
}

} // namespace ir
} // namespace gard
