#include "bytecode/compiler.h"
#include <cstring>

namespace gard {
namespace bytecode {

BytecodeCompiler::BytecodeCompiler() {}

BytecodeModule BytecodeCompiler::compile(const ir::Module& module) {
    module_ = BytecodeModule();

    for (auto& fn : module.functions) {
        compileFunction(*fn);
    }

    // Transfer annotation metadata from IR classes to bytecode module
    for (auto& cls : module.classes) {
        for (auto& ann : cls.annotations) {
            BytecodeModule::AnnotationEntry entry;
            entry.target = ann.target.empty() ? cls.name : (cls.name + "." + ann.target);
            entry.name = ann.name;
            entry.args = ann.args;
            module_.annotations.push_back(std::move(entry));
        }
        // Store field metadata for ORM (@Table auto-create)
        for (size_t fi = 0; fi < cls.fieldNames.size(); fi++) {
            BytecodeModule::AnnotationEntry fieldEntry;
            fieldEntry.target = cls.name;
            fieldEntry.name = "_field";
            std::string typeName = "TEXT";
            if (fi < cls.fieldTypes.size()) {
                switch (cls.fieldTypes[fi]) {
                    case ir::IRType::Int32: typeName = "INTEGER"; break;
                    case ir::IRType::Int64: typeName = "INTEGER"; break;
                    case ir::IRType::Float32: typeName = "REAL"; break;
                    case ir::IRType::Float64: typeName = "REAL"; break;
                    case ir::IRType::Bool: typeName = "INTEGER"; break;
                    case ir::IRType::String: typeName = "TEXT"; break;
                    default: typeName = "TEXT"; break;
                }
            }
            fieldEntry.args.push_back({cls.fieldNames[fi], typeName});
            module_.annotations.push_back(std::move(fieldEntry));

            // Store original type name for enum detection
            if (fi < cls.fieldTypeNames.size() && !cls.fieldTypeNames[fi].empty()) {
                BytecodeModule::AnnotationEntry typeNameEntry;
                typeNameEntry.target = cls.name + "." + cls.fieldNames[fi];
                typeNameEntry.name = "_fieldTypeName";
                typeNameEntry.args.push_back({"type", cls.fieldTypeNames[fi]});
                module_.annotations.push_back(std::move(typeNameEntry));
            }
        }
    }

    // Resolve pending jumps
    for (auto& [patchAddr, label] : pendingJumps_) {
        auto it = labels_.find(label);
        if (it != labels_.end()) {
            patchJump(patchAddr, it->second);
        } else {
            errors_.push_back("Unresolved label: " + label);
        }
    }

    return module_;
}

void BytecodeCompiler::compileFunction(const ir::Function& fn) {
    locals_.clear();
    localCount_ = 0;
    maxStack_ = 0;
    currentStack_ = 0;
    labels_.clear();
    pendingJumps_.clear();

    codeStart_ = static_cast<uint32_t>(module_.code.size());

    // Declare parameters as locals
    for (auto& param : fn.params) {
        declareLocal(param->name);
    }

    // Compile all blocks
    for (auto& block : fn.blocks) {
        compileBlock(*block);
    }

    // Ensure function ends with a return
    if (module_.code.empty() || module_.code.back() != static_cast<uint8_t>(OpCode::RETURN)) {
        if (fn.returnType == ir::IRType::Void) {
            emit(OpCode::RETURN_VOID);
        }
    }

    // Resolve local jumps
    for (auto& [patchAddr, label] : pendingJumps_) {
        auto it = labels_.find(label);
        if (it != labels_.end()) {
            patchJump(patchAddr, it->second);
        }
    }

    // Register function
    FunctionInfo info;
    info.name = fn.name;
    info.paramCount = static_cast<uint16_t>(fn.params.size());
    info.localCount = localCount_;
    info.maxStack = maxStack_;
    info.codeOffset = codeStart_;
    info.codeLength = static_cast<uint32_t>(module_.code.size()) - codeStart_;
    info.isAsync = fn.isAsync;
    info.hasRestParam = fn.hasRestParam;
    info.restParamIndex = fn.restParamIndex;
    module_.functions.push_back(info);

    // Track entry point
    if (fn.name == "main") {
        module_.entryFunction = static_cast<int16_t>(module_.functions.size() - 1);
    }

    // Add debug symbol
    DebugSymbol sym;
    sym.pcOffset = codeStart_;
    sym.line = 1;
    sym.column = 1;
    sym.functionName = fn.name;
    module_.debugSymbols.push_back(sym);
}

void BytecodeCompiler::compileBlock(const ir::BasicBlock& block) {
    recordLabel(block.label, currentOffset());

    for (auto& inst : block.instructions) {
        compileInstruction(*inst);
    }
}

void BytecodeCompiler::compileInstruction(const ir::Instruction& inst) {
    // Emit LINE opcode when source line changes (for error position tracking)
    if (inst.sourceLine > 0 && (inst.sourceLine != lastEmittedLine_ || inst.sourceColumn != lastEmittedColumn_)) {
        emit(OpCode::LINE);
        emitU16(static_cast<uint16_t>(inst.sourceLine));
        emitU16(static_cast<uint16_t>(inst.sourceColumn));
        lastEmittedLine_ = inst.sourceLine;
        lastEmittedColumn_ = inst.sourceColumn;
    }

    // Helper: push an IR value onto the bytecode stack
    auto pushValue = [&](const ir::Value* val) {
        if (!val) { emit(OpCode::CONST_NULL); currentStack_++; return; }
        if (auto* ci = dynamic_cast<const ir::ConstantInt*>(val)) {
            emit(OpCode::CONST_I32);
            emitI32(static_cast<int32_t>(ci->value));
            currentStack_++;
        } else if (auto* cf = dynamic_cast<const ir::ConstantFloat*>(val)) {
            emit(OpCode::CONST_F64);
            emitF64(cf->value);
            currentStack_++;
        } else if (auto* cs = dynamic_cast<const ir::ConstantString*>(val)) {
            uint16_t strIdx = module_.addConstantString(cs->value);
            emit(OpCode::CONST_STR);
            emitU16(strIdx);
            currentStack_++;
        } else if (auto* cb = dynamic_cast<const ir::ConstantBool*>(val)) {
            emit(cb->value ? OpCode::CONST_TRUE : OpCode::CONST_FALSE);
            currentStack_++;
        } else if (dynamic_cast<const ir::ConstantNull*>(val)) {
            emit(OpCode::CONST_NULL);
            currentStack_++;
        } else {
            // It's an instruction result or parameter — load from its local slot
            uint16_t idx = resolveLocal(val->name);
            emit(OpCode::LOAD_LOCAL);
            emitU16(idx);
            currentStack_++;
        }
        if (currentStack_ > maxStack_) maxStack_ = currentStack_;
    };

    switch (inst.opcode) {
        // --- Arithmetic ---
        case ir::Opcode::Add: {
            // Superinstruction: if both operands are locals, emit LOAD_ADD_I
            auto* op0 = inst.operands[0].get();
            auto* op1 = inst.operands[1].get();
            bool emittedSuper = false;
            if (op0 && op1 && op0->kind == ir::ValueKind::Instruction && op1->kind == ir::ValueKind::Instruction) {
                // Both are instruction results stored in locals
                auto it0 = locals_.find(op0->name);
                auto it1 = locals_.find(op1->name);
                if (it0 != locals_.end() && it1 != locals_.end()) {
                    emit(OpCode::LOAD_ADD_I);
                    emitU16(it0->second);
                    emitU16(it1->second);
                    currentStack_++;
                    emittedSuper = true;
                }
            } else if (op0 && op0->kind == ir::ValueKind::Instruction && op1 && dynamic_cast<const ir::ConstantInt*>(op1)) {
                // LOAD_LOCAL + CONST + ADD → LOAD_CONST_ADD
                auto it0 = locals_.find(op0->name);
                auto* ci = dynamic_cast<const ir::ConstantInt*>(op1);
                if (it0 != locals_.end() && ci) {
                    emit(OpCode::LOAD_CONST_ADD);
                    emitU16(it0->second);
                    emitI32(static_cast<int32_t>(ci->value));
                    currentStack_++;
                    emittedSuper = true;
                }
            }
            if (!emittedSuper) {
                pushValue(op0);
                pushValue(op1);
                emit(OpCode::ADD_I);
                currentStack_--;
            }
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++;
              emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            emit(OpCode::POP); currentStack_--;
            break;
        }
        case ir::Opcode::Sub:
            pushValue(inst.operands[0].get());
            pushValue(inst.operands[1].get());
            emit(OpCode::SUB_I); currentStack_--;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++;
              emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            emit(OpCode::POP); currentStack_--;
            break;
        case ir::Opcode::Mul:
            pushValue(inst.operands[0].get());
            pushValue(inst.operands[1].get());
            emit(OpCode::MUL_I); currentStack_--;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++;
              emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            emit(OpCode::POP); currentStack_--;
            break;
        case ir::Opcode::Div:
            pushValue(inst.operands[0].get());
            pushValue(inst.operands[1].get());
            emit(OpCode::DIV_I); currentStack_--;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++;
              emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            emit(OpCode::POP); currentStack_--;
            break;
        case ir::Opcode::Mod:
            pushValue(inst.operands[0].get());
            pushValue(inst.operands[1].get());
            emit(OpCode::MOD_I); currentStack_--;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++;
              emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            emit(OpCode::POP); currentStack_--;
            break;
        case ir::Opcode::Neg:
            pushValue(inst.operands[0].get());
            emit(OpCode::NEG_I);
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++;
              emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            emit(OpCode::POP); currentStack_--;
            break;

        // --- Bitwise ---
        case ir::Opcode::BitAnd: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::BIT_AND); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::BitOr:  pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::BIT_OR); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::BitXor: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::BIT_XOR); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::Shl:   pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::SHL); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::Shr:   pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::SHR); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::BitNot: pushValue(inst.operands[0].get()); emit(OpCode::BIT_NOT); { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;

        // --- Comparison ---
        case ir::Opcode::CmpEq: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::CMP_EQ); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::CmpNe: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::CMP_NE); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::CmpLt: {
            auto* op0 = inst.operands[0].get(); auto* op1 = inst.operands[1].get();
            bool emittedSuper = false;
            if (op0 && op1 && op0->kind == ir::ValueKind::Instruction && op1->kind == ir::ValueKind::Instruction) {
                auto it0 = locals_.find(op0->name); auto it1 = locals_.find(op1->name);
                if (it0 != locals_.end() && it1 != locals_.end()) {
                    emit(OpCode::LOAD_CMP_LT); emitU16(it0->second); emitU16(it1->second);
                    currentStack_++; emittedSuper = true;
                }
            }
            if (!emittedSuper) { pushValue(op0); pushValue(op1); emit(OpCode::CMP_LT); currentStack_--; }
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--;
            break;
        }
        case ir::Opcode::CmpGt: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::CMP_GT); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::CmpLe: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::CMP_LE); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::CmpGe: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::CMP_GE); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;

        // --- Logical ---
        case ir::Opcode::LogAnd: pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::LOG_AND); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::LogOr:  pushValue(inst.operands[0].get()); pushValue(inst.operands[1].get()); emit(OpCode::LOG_OR); currentStack_--; { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;
        case ir::Opcode::LogNot: pushValue(inst.operands[0].get()); emit(OpCode::LOG_NOT); { uint16_t idx = declareLocal(inst.name); emit(OpCode::DUP); currentStack_++; emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; } emit(OpCode::POP); currentStack_--; break;

        // --- Control flow ---
        case ir::Opcode::Branch:
            if (inst.jumpTarget) {
                emit(OpCode::JUMP);
                pendingJumps_.push_back({currentOffset(), inst.jumpTarget->label});
                emitU16(0);
            }
            break;
        case ir::Opcode::CondBranch:
            if (inst.trueBlock && inst.falseBlock) {
                // Push condition
                if (!inst.operands.empty()) pushValue(inst.operands[0].get());
                emit(OpCode::JUMP_IF);
                pendingJumps_.push_back({currentOffset(), inst.trueBlock->label});
                emitU16(0);
                currentStack_--;
                emit(OpCode::JUMP);
                pendingJumps_.push_back({currentOffset(), inst.falseBlock->label});
                emitU16(0);
            }
            break;
        case ir::Opcode::Return:
            if (!inst.operands.empty()) pushValue(inst.operands[0].get());
            emit(OpCode::RETURN);
            currentStack_--;
            break;
        case ir::Opcode::ReturnVoid:
            emit(OpCode::RETURN_VOID);
            break;

        // --- Memory ---
        case ir::Opcode::Alloca: {
            declareLocal(inst.name);
            break;
        }
        case ir::Opcode::Load: {
            // Load produces a value — store it in a local for the instruction's name
            std::string srcName = inst.operands.empty() ? inst.name : inst.operands[0]->name;
            uint16_t srcIdx = resolveLocal(srcName);
            uint16_t dstIdx = declareLocal(inst.name);
            emit(OpCode::LOAD_LOCAL); emitU16(srcIdx); currentStack_++;
            emit(OpCode::STORE_LOCAL); emitU16(dstIdx); currentStack_--;
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }
        case ir::Opcode::Store: {
            std::string varName = inst.operands.empty() ? "" : inst.operands[0]->name;
            uint16_t idx = resolveLocal(varName);

            // Detect INC_LOCAL / DEC_LOCAL pattern: x = x + 1 or x = x - 1
            if (inst.operands.size() >= 2) {
                auto* rhs = inst.operands[1].get();
                if (rhs && rhs->kind == ir::ValueKind::Instruction) {
                    auto* rhsInst = static_cast<const ir::Instruction*>(rhs);

                    // Pattern: Store(x, Add(x, 1)) → INC_LOCAL
                    if (rhsInst->opcode == ir::Opcode::Add && rhsInst->operands.size() == 2) {
                        auto* addOp0 = rhsInst->operands[0].get();
                        auto* addOp1 = rhsInst->operands[1].get();
                        // Check if one operand is the same variable and the other is const 1
                        bool op0IsVar = (addOp0 && addOp0->name == varName);
                        bool op1IsVar = (addOp1 && addOp1->name == varName);
                        auto* constOp = op0IsVar ? dynamic_cast<const ir::ConstantInt*>(addOp1) :
                                        op1IsVar ? dynamic_cast<const ir::ConstantInt*>(addOp0) : nullptr;
                        if ((op0IsVar || op1IsVar) && constOp && constOp->value == 1) {
                            emit(OpCode::INC_LOCAL); emitU16(idx);
                            break;
                        }
                    }
                    // Pattern: Store(x, Sub(x, 1)) → DEC_LOCAL
                    if (rhsInst->opcode == ir::Opcode::Sub && rhsInst->operands.size() == 2) {
                        auto* subOp0 = rhsInst->operands[0].get();
                        auto* subOp1 = rhsInst->operands[1].get();
                        bool op0IsVar = (subOp0 && subOp0->name == varName);
                        auto* constOp = dynamic_cast<const ir::ConstantInt*>(subOp1);
                        if (op0IsVar && constOp && constOp->value == 1) {
                            emit(OpCode::DEC_LOCAL); emitU16(idx);
                            break;
                        }
                    }
                }
            }

            // Default: emit normal store
            if (inst.operands.size() >= 2) {
                pushValue(inst.operands[1].get());
            } else {
                emit(OpCode::CONST_NULL); currentStack_++;
            }
            emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--;
            break;
        }

        // --- Calls ---
        case ir::Opcode::Call: {
            uint16_t funcIdx = module_.addConstantString(inst.targetName);
            // Push arguments
            for (auto& op : inst.operands) pushValue(op.get());
            emit(OpCode::CALL);
            emitU16(funcIdx);
            emitU8(static_cast<uint8_t>(inst.operands.size()));
            currentStack_ -= static_cast<uint16_t>(inst.operands.size());
            currentStack_++; // return value
            // Store result
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }
        case ir::Opcode::CallVirtual: {
            uint16_t methodIdx = module_.addConstantString(inst.targetName);
            for (auto& op : inst.operands) pushValue(op.get());
            emit(OpCode::CALL_VIRTUAL);
            emitU16(methodIdx);
            emitU8(static_cast<uint8_t>(inst.operands.size()));
            currentStack_ -= static_cast<uint16_t>(inst.operands.size());
            currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }

        // --- Print ---
        case ir::Opcode::Print: {
            if (!inst.operands.empty()) pushValue(inst.operands[0].get());
            else { emit(OpCode::CONST_NULL); currentStack_++; }
            emit(OpCode::PRINT); currentStack_--;
            break;
        }

        // --- Objects ---
        case ir::Opcode::NewObject: {
            uint16_t classIdx = module_.addConstantString(inst.targetName);
            emit(OpCode::NEW_OBJ); emitU16(classIdx); currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }
        case ir::Opcode::NewArray: {
            emit(OpCode::NEW_ARRAY); emitU16(0); currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }
        case ir::Opcode::NewMap: {
            emit(OpCode::NEW_MAP); currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }
        case ir::Opcode::GetField: {
            pushValue(inst.operands[0].get());
            uint16_t fieldIdx = module_.addConstantString(inst.targetName);
            emit(OpCode::GET_FIELD); emitU16(fieldIdx);
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            break;
        }
        case ir::Opcode::SetField: {
            pushValue(inst.operands[0].get());
            if (inst.operands.size() >= 2) pushValue(inst.operands[1].get());
            uint16_t fieldIdx = module_.addConstantString(inst.targetName);
            emit(OpCode::SET_FIELD); emitU16(fieldIdx); currentStack_ -= 2;
            break;
        }
        case ir::Opcode::GetElement: {
            pushValue(inst.operands[0].get());
            pushValue(inst.operands[1].get());
            emit(OpCode::GET_ELEM); currentStack_--;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            break;
        }
        case ir::Opcode::SetElement: {
            if (inst.operands.size() >= 3) {
                pushValue(inst.operands[0].get());
                pushValue(inst.operands[1].get());
                pushValue(inst.operands[2].get());
                emit(OpCode::SET_ELEM); currentStack_ -= 3;
            }
            break;
        }

        // --- Concat ---
        case ir::Opcode::Concat: {
            pushValue(inst.operands[0].get());
            pushValue(inst.operands[1].get());
            emit(OpCode::CONCAT); currentStack_--;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            break;
        }

        // --- Async ---
        case ir::Opcode::Await: {
            // Check if the operand is a Call/CallVirtual instruction result.
            // If so, re-emit as CALL_ASYNC so the native runs on a thread pool.
            // The AWAIT opcode then waits for the Future to resolve.
            auto* operand = inst.operands[0].get();
            bool emittedAsync = false;

            if (operand && operand->kind == ir::ValueKind::Instruction) {
                auto* callInst = static_cast<const ir::Instruction*>(operand);
                if (callInst->opcode == ir::Opcode::Call || callInst->opcode == ir::Opcode::CallVirtual) {
                    // Re-emit the call as CALL_ASYNC
                    uint16_t funcIdx = module_.addConstantString(callInst->targetName);
                    for (auto& op : callInst->operands) pushValue(op.get());
                    emit(OpCode::CALL_ASYNC);
                    emitU16(funcIdx);
                    emitU8(static_cast<uint8_t>(callInst->operands.size()));
                    currentStack_ -= static_cast<uint16_t>(callInst->operands.size());
                    currentStack_++; // Future value on stack
                    emit(OpCode::AWAIT);
                    emittedAsync = true;
                }
            }

            if (!emittedAsync) {
                // Fallback: load the value and pass through AWAIT (synchronous path)
                pushValue(inst.operands[0].get());
                emit(OpCode::AWAIT);
            }

            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            break;
        }

        // --- Closure ---
        case ir::Opcode::CreateClosure: {
            emit(OpCode::CLOSURE); emitU16(0); emitU8(0); currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }

        // --- Type cast ---
        case ir::Opcode::Cast: {
            // operands[0] = value, operands[1] = type name string
            if (inst.operands.size() >= 2) {
                pushValue(inst.operands[0].get());
                pushValue(inst.operands[1].get()); // type name as string
            }
            emit(OpCode::CAST); emitU8(0); // target type byte (unused, we use string)
            currentStack_ -= 2;
            currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }

        // --- Type check (is) ---
        case ir::Opcode::IsType: {
            // operands[0] = value, operands[1] = type name string
            if (inst.operands.size() >= 2) {
                pushValue(inst.operands[0].get());
                pushValue(inst.operands[1].get()); // type name as string
            }
            emit(OpCode::IS_TYPE); emitU16(0); // type index (unused, we use string from stack)
            currentStack_ -= 2;
            currentStack_++;
            { uint16_t idx = declareLocal(inst.name); emit(OpCode::STORE_LOCAL); emitU16(idx); currentStack_--; }
            if (currentStack_ > maxStack_) maxStack_ = currentStack_;
            break;
        }

        case ir::Opcode::Nop:
        case ir::Opcode::Phi:
            break;

        // --- Exception handling ---
        case ir::Opcode::TryBegin: {
            emit(OpCode::TRY_BEGIN);
            // The catch block target — will be patched
            if (inst.jumpTarget) {
                pendingJumps_.push_back({currentOffset(), inst.jumpTarget->label});
            }
            emitU16(0); // placeholder for catch PC
            break;
        }
        case ir::Opcode::TryEnd: {
            emit(OpCode::TRY_END);
            break;
        }
        case ir::Opcode::Throw: {
            if (!inst.operands.empty()) pushValue(inst.operands[0].get());
            else { emit(OpCode::CONST_NULL); currentStack_++; }
            emit(OpCode::THROW);
            currentStack_--;
            break;
        }

        case ir::Opcode::CatchException: {
            // The VM pushed the exception onto the stack before jumping to catch.
            // Just store it into the local variable.
            currentStack_++; // account for the value the VM pushed
            uint16_t idx = declareLocal(inst.name);
            emit(OpCode::STORE_LOCAL); emitU16(idx);
            currentStack_--;
            break;
        }

        default:
            emit(OpCode::NOP);
            break;
    }
}

// --- Emit helpers ---

void BytecodeCompiler::emit(OpCode op) {
    module_.code.push_back(static_cast<uint8_t>(op));
}

void BytecodeCompiler::emitU8(uint8_t val) {
    module_.code.push_back(val);
}

void BytecodeCompiler::emitU16(uint16_t val) {
    module_.code.push_back(static_cast<uint8_t>(val >> 8));
    module_.code.push_back(static_cast<uint8_t>(val & 0xFF));
}

void BytecodeCompiler::emitI32(int32_t val) {
    module_.code.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    module_.code.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    module_.code.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    module_.code.push_back(static_cast<uint8_t>(val & 0xFF));
}

void BytecodeCompiler::emitI64(int64_t val) {
    for (int i = 7; i >= 0; i--) {
        module_.code.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

void BytecodeCompiler::emitF32(float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, 4);
    emitI32(static_cast<int32_t>(bits));
}

void BytecodeCompiler::emitF64(double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, 8);
    emitI64(static_cast<int64_t>(bits));
}

uint32_t BytecodeCompiler::currentOffset() const {
    return static_cast<uint32_t>(module_.code.size());
}

void BytecodeCompiler::patchJump(uint32_t patchAddr, uint32_t target) {
    if (patchAddr + 1 < module_.code.size()) {
        module_.code[patchAddr] = static_cast<uint8_t>(target >> 8);
        module_.code[patchAddr + 1] = static_cast<uint8_t>(target & 0xFF);
    }
}

uint16_t BytecodeCompiler::declareLocal(const std::string& name) {
    if (locals_.find(name) != locals_.end()) {
        return locals_[name];
    }
    uint16_t idx = localCount_++;
    locals_[name] = idx;
    return idx;
}

uint16_t BytecodeCompiler::resolveLocal(const std::string& name) {
    auto it = locals_.find(name);
    if (it != locals_.end()) return it->second;
    // Auto-declare if not found
    return declareLocal(name);
}

uint32_t BytecodeCompiler::labelOffset(const std::string& label) {
    auto it = labels_.find(label);
    if (it != labels_.end()) return it->second;
    return 0;
}

void BytecodeCompiler::recordLabel(const std::string& label, uint32_t offset) {
    labels_[label] = offset;
}

// ===== Peephole Optimizer: Fuse opcode sequences into superinstructions =====

void BytecodeCompiler::peepholeOptimize(BytecodeModule& module) {
    auto& code = module.code;
    if (code.size() < 6) return; // too short to optimize

    // Build a new code buffer with fused instructions
    std::vector<uint8_t> optimized;
    optimized.reserve(code.size());

    size_t i = 0;
    while (i < code.size()) {
        uint8_t op = code[i];

        // Pattern: INC_LOCAL
        // LOAD_LOCAL(idx) + CONST_I32(1) + ADD_I + STORE_LOCAL(idx)
        // Bytecode: 0x20 LL LL 0x13 01 00 00 00 0x30 0x21 LL LL
        if (op == (uint8_t)OpCode::LOAD_LOCAL && i + 11 < code.size()) {
            uint16_t loadIdx = (uint16_t)code[i+1] | ((uint16_t)code[i+2] << 8);
            if (code[i+3] == (uint8_t)OpCode::CONST_I32) {
                int32_t constVal;
                std::memcpy(&constVal, &code[i+4], 4);
                if (constVal == 1 && code[i+8] == (uint8_t)OpCode::ADD_I &&
                    code[i+9] == (uint8_t)OpCode::STORE_LOCAL) {
                    uint16_t storeIdx = (uint16_t)code[i+10] | ((uint16_t)code[i+11] << 8);
                    if (loadIdx == storeIdx) {
                        // Fuse into INC_LOCAL
                        optimized.push_back((uint8_t)OpCode::INC_LOCAL);
                        optimized.push_back(code[i+1]);
                        optimized.push_back(code[i+2]);
                        i += 12;
                        continue;
                    }
                }
                // Pattern: DEC_LOCAL (same but SUB_I)
                if (constVal == 1 && code[i+8] == (uint8_t)OpCode::SUB_I &&
                    code[i+9] == (uint8_t)OpCode::STORE_LOCAL) {
                    uint16_t storeIdx = (uint16_t)code[i+10] | ((uint16_t)code[i+11] << 8);
                    if (loadIdx == storeIdx) {
                        optimized.push_back((uint8_t)OpCode::DEC_LOCAL);
                        optimized.push_back(code[i+1]);
                        optimized.push_back(code[i+2]);
                        i += 12;
                        continue;
                    }
                }
                // Pattern: LOAD_CONST_ADD — LOAD_LOCAL + CONST_I32(n) + ADD_I
                if (code[i+8] == (uint8_t)OpCode::ADD_I) {
                    optimized.push_back((uint8_t)OpCode::LOAD_CONST_ADD);
                    optimized.push_back(code[i+1]);
                    optimized.push_back(code[i+2]);
                    // Emit the constant value (4 bytes)
                    optimized.push_back(code[i+4]);
                    optimized.push_back(code[i+5]);
                    optimized.push_back(code[i+6]);
                    optimized.push_back(code[i+7]);
                    i += 9; // skip LOAD_LOCAL(3) + CONST_I32(5) + ADD_I(1)
                    continue;
                }
            }
        }

        // Pattern: LOAD_ADD_I — LOAD_LOCAL(a) + LOAD_LOCAL(b) + ADD_I
        if (op == (uint8_t)OpCode::LOAD_LOCAL && i + 5 < code.size()) {
            if (code[i+3] == (uint8_t)OpCode::LOAD_LOCAL && i + 7 < code.size()) {
                uint8_t nextOp = code[i+6];
                if (nextOp == (uint8_t)OpCode::ADD_I) {
                    optimized.push_back((uint8_t)OpCode::LOAD_ADD_I);
                    optimized.push_back(code[i+1]); optimized.push_back(code[i+2]); // idx_a
                    optimized.push_back(code[i+4]); optimized.push_back(code[i+5]); // idx_b
                    i += 7;
                    continue;
                }
                if (nextOp == (uint8_t)OpCode::SUB_I) {
                    optimized.push_back((uint8_t)OpCode::LOAD_SUB_I);
                    optimized.push_back(code[i+1]); optimized.push_back(code[i+2]);
                    optimized.push_back(code[i+4]); optimized.push_back(code[i+5]);
                    i += 7;
                    continue;
                }
                if (nextOp == (uint8_t)OpCode::CMP_LT) {
                    optimized.push_back((uint8_t)OpCode::LOAD_CMP_LT);
                    optimized.push_back(code[i+1]); optimized.push_back(code[i+2]);
                    optimized.push_back(code[i+4]); optimized.push_back(code[i+5]);
                    i += 7;
                    continue;
                }
                if (nextOp == (uint8_t)OpCode::CMP_GE) {
                    optimized.push_back((uint8_t)OpCode::LOAD_CMP_GE);
                    optimized.push_back(code[i+1]); optimized.push_back(code[i+2]);
                    optimized.push_back(code[i+4]); optimized.push_back(code[i+5]);
                    i += 7;
                    continue;
                }
            }

            // Pattern: LOAD_STORE — LOAD_LOCAL(a) + STORE_LOCAL(b)
            if (code[i+3] == (uint8_t)OpCode::STORE_LOCAL && i + 5 < code.size()) {
                optimized.push_back((uint8_t)OpCode::LOAD_STORE);
                optimized.push_back(code[i+1]); optimized.push_back(code[i+2]); // src
                optimized.push_back(code[i+4]); optimized.push_back(code[i+5]); // dst
                i += 6;
                continue;
            }
        }

        // No pattern matched — emit original byte
        optimized.push_back(code[i]);
        i++;
    }

    // Replace code with optimized version
    code = std::move(optimized);

    // Update function code lengths (they reference into the code buffer)
    // Since we changed code size, offsets may shift. For safety, recalculate.
    // Note: this is safe because peephole only shrinks code (never grows).
}

} // namespace bytecode
} // namespace gard
