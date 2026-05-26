#pragma once

#include "ir/ir.h"
#include <vector>
#include <string>

namespace gard {
namespace ir {

// IR validation pass — checks well-formedness
class IRValidator {
public:
    bool validate(const Module& module);

    const std::vector<std::string>& getErrors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    void validateFunction(const Function& fn);
    void validateBlock(const BasicBlock& block, const Function& fn);
    void validateInstruction(const Instruction& inst, const BasicBlock& block);

    // Checks
    void checkTerminator(const BasicBlock& block, const Function& fn);
    void checkOperandCount(const Instruction& inst);
    void checkBranchTargets(const Instruction& inst, const Function& fn);

    void error(const std::string& msg);

    std::vector<std::string> errors_;
};

} // namespace ir
} // namespace gard
