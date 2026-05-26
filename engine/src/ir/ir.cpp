#include "ir/ir.h"
#include <sstream>

namespace gard {
namespace ir {

std::string IRTypeInfo::toString() const {
    switch (base) {
        case IRType::Void: return "void";
        case IRType::Int32: return "i32";
        case IRType::Int64: return "i64";
        case IRType::Int16: return "i16";
        case IRType::Float32: return "f32";
        case IRType::Float64: return "f64";
        case IRType::Bool: return "bool";
        case IRType::Char: return "char";
        case IRType::String: return "string";
        case IRType::Pointer: return "ptr";
        case IRType::Array: return "array";
        case IRType::Map: return "map";
        case IRType::Function: return "func";
        case IRType::Null: return "null";
    }
    return "unknown";
}

bool IRTypeInfo::isNumeric() const {
    return isIntegral() || isFloating();
}

bool IRTypeInfo::isIntegral() const {
    return base == IRType::Int32 || base == IRType::Int64 || base == IRType::Int16;
}

bool IRTypeInfo::isFloating() const {
    return base == IRType::Float32 || base == IRType::Float64;
}

static std::string irTypeStr(IRType t) {
    switch (t) {
        case IRType::Void: return "void";
        case IRType::Int32: return "i32";
        case IRType::Int64: return "i64";
        case IRType::Int16: return "i16";
        case IRType::Float32: return "f32";
        case IRType::Float64: return "f64";
        case IRType::Bool: return "bool";
        case IRType::Char: return "char";
        case IRType::String: return "string";
        case IRType::Pointer: return "ptr";
        case IRType::Array: return "array";
        case IRType::Map: return "map";
        case IRType::Function: return "func";
        case IRType::Null: return "null";
    }
    return "?";
}

static std::string opcodeStr(Opcode op) {
    switch (op) {
        case Opcode::Add: return "add";
        case Opcode::Sub: return "sub";
        case Opcode::Mul: return "mul";
        case Opcode::Div: return "div";
        case Opcode::Mod: return "mod";
        case Opcode::Neg: return "neg";
        case Opcode::BitAnd: return "and";
        case Opcode::BitOr: return "or";
        case Opcode::BitXor: return "xor";
        case Opcode::BitNot: return "not";
        case Opcode::Shl: return "shl";
        case Opcode::Shr: return "shr";
        case Opcode::UShr: return "ushr";
        case Opcode::CmpEq: return "eq";
        case Opcode::CmpNe: return "ne";
        case Opcode::CmpLt: return "lt";
        case Opcode::CmpGt: return "gt";
        case Opcode::CmpLe: return "le";
        case Opcode::CmpGe: return "ge";
        case Opcode::LogAnd: return "land";
        case Opcode::LogOr: return "lor";
        case Opcode::LogNot: return "lnot";
        case Opcode::Branch: return "br";
        case Opcode::CondBranch: return "cbr";
        case Opcode::Return: return "ret";
        case Opcode::ReturnVoid: return "ret.void";
        case Opcode::Call: return "call";
        case Opcode::CallVirtual: return "vcall";
        case Opcode::CallAsync: return "acall";
        case Opcode::Alloca: return "alloca";
        case Opcode::Load: return "load";
        case Opcode::Store: return "store";
        case Opcode::GetField: return "getfield";
        case Opcode::SetField: return "setfield";
        case Opcode::GetElement: return "getelem";
        case Opcode::SetElement: return "setelem";
        case Opcode::NewObject: return "newobj";
        case Opcode::NewArray: return "newarr";
        case Opcode::NewMap: return "newmap";
        case Opcode::Cast: return "cast";
        case Opcode::IsType: return "istype";
        case Opcode::Await: return "await";
        case Opcode::Yield: return "yield";
        case Opcode::Resume: return "resume";
        case Opcode::CreateClosure: return "closure";
        case Opcode::LoadCapture: return "loadcap";
        case Opcode::Phi: return "phi";
        case Opcode::Print: return "print";
        case Opcode::Concat: return "concat";
        case Opcode::TryBegin: return "try.begin";
        case Opcode::TryEnd: return "try.end";
        case Opcode::Throw: return "throw";
        case Opcode::Nop: return "nop";
    }
    return "?";
}

std::string Module::dump() const {
    std::ostringstream out;
    out << "; Module: " << name << "\n\n";

    // Globals
    for (auto& g : globals) {
        out << "@" << g->name << " : " << irTypeStr(g->type);
        if (g->isConstant) out << " const";
        out << "\n";
    }
    if (!globals.empty()) out << "\n";

    // Classes
    for (auto& cls : classes) {
        out << "; class " << cls.name;
        if (!cls.baseClass.empty()) out << " extends " << cls.baseClass;
        out << " {\n";
        for (size_t i = 0; i < cls.fieldNames.size(); i++) {
            out << ";   " << cls.fieldNames[i] << " : " << irTypeStr(cls.fieldTypes[i]) << "\n";
        }
        for (auto& m : cls.methodNames) {
            out << ";   method " << m << "\n";
        }
        out << "; }\n\n";
    }

    // Functions
    for (auto& fn : functions) {
        out << "define " << irTypeStr(fn->returnType) << " @" << fn->name << "(";
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (i > 0) out << ", ";
            out << irTypeStr(fn->params[i]->type) << " %" << fn->params[i]->name;
        }
        out << ")";
        if (fn->isAsync) out << " async";
        out << " {\n";

        for (auto& block : fn->blocks) {
            out << block->label << ":\n";
            for (auto& inst : block->instructions) {
                out << "  ";
                if (inst->type != IRType::Void) {
                    out << "%" << inst->name << " = ";
                }
                out << opcodeStr(inst->opcode);

                // Print operands
                for (size_t i = 0; i < inst->operands.size(); i++) {
                    if (inst->operands[i]) {
                        out << " %" << inst->operands[i]->name;
                    } else {
                        out << " <null>";
                    }
                    if (i < inst->operands.size() - 1) out << ",";
                }

                // Print extra info
                if (!inst->targetName.empty()) {
                    out << " @" << inst->targetName;
                }
                if (inst->jumpTarget) {
                    out << " -> " << inst->jumpTarget->label;
                }
                if (inst->trueBlock) {
                    out << " ? " << inst->trueBlock->label << " : " << inst->falseBlock->label;
                }

                out << "\n";
            }
        }
        out << "}\n\n";
    }

    return out.str();
}

} // namespace ir
} // namespace gard
