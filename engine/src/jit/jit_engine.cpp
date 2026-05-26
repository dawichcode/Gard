#include "jit/jit_engine.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Scalar.h>

#include <iostream>
#include <cstring>
#include <set>
#include <map>

namespace gard {
namespace jit {

void JitEngine::initialize() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
}

JitEngine::JitEngine() {
    auto jitExpected = llvm::orc::LLJITBuilder().create();
    if (!jitExpected) {
        std::cerr << "[JIT] Failed to create LLJIT: "
                  << llvm::toString(jitExpected.takeError()) << std::endl;
        return;
    }
    jit_ = std::move(*jitExpected);
}

JitEngine::~JitEngine() = default;

void JitEngine::recordExecution(uint16_t funcIndex) {
    auto& info = functions_[funcIndex];
    info.funcIndex = funcIndex;
    info.executionCount++;
}

JitFuncPtr JitEngine::getCompiledFunction(uint16_t funcIndex) {
    auto it = functions_.find(funcIndex);
    if (it != functions_.end() && it->second.compiledPtr) {
        return it->second.compiledPtr;
    }
    return nullptr;
}

bool JitEngine::compileFunction(uint16_t funcIndex, const bytecode::BytecodeModule& module, runtime::VM& vm) {
    if (!jit_) return false;

    auto& info = functions_[funcIndex];
    if (info.compiledPtr) return true;
    if (info.compilationFailed) return false;

    auto& fn = module.functions[funcIndex];

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = generateIR(funcIndex, module, vm, *ctx);

    if (!mod) {
        info.compilationFailed = true;
        totalFailed_++;
        return false;
    }

    std::string errStr;
    llvm::raw_string_ostream errStream(errStr);
    if (llvm::verifyModule(*mod, &errStream)) {
        info.compilationFailed = true;
        totalFailed_++;
        return false;
    }

    std::string funcName = "jit_" + fn.name;
    auto tsm = llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx));
    if (auto err = jit_->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        info.compilationFailed = true;
        totalFailed_++;
        return false;
    }

    auto sym = jit_->lookup(funcName);
    if (!sym) {
        llvm::consumeError(sym.takeError());
        info.compilationFailed = true;
        totalFailed_++;
        return false;
    }

    info.compiledPtr = reinterpret_cast<JitFuncPtr>(sym->getValue());
    info.isHot = true;
    totalCompiled_++;
    return true;
}

// Generate LLVM IR with full control flow support (branches, loops)
std::unique_ptr<llvm::Module> JitEngine::generateIR(
    uint16_t funcIndex,
    const bytecode::BytecodeModule& module,
    runtime::VM& vm,
    llvm::LLVMContext& ctx)
{
    auto& fn = module.functions[funcIndex];
    auto mod = std::make_unique<llvm::Module>("jit_" + fn.name, ctx);

    // Use i64 as universal value type (holds int32, int64, bool, pointers)
    // Signature: int64_t jit_funcName(int64_t* args, int32_t argCount)
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64PtrTy = llvm::PointerType::getUnqual(i64Ty);
    auto* f64Ty = llvm::Type::getDoubleTy(ctx);
    auto* funcType = llvm::FunctionType::get(i64Ty, {i64PtrTy, i32Ty}, false);
    auto* llvmFunc = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                             "jit_" + fn.name, mod.get());

    // Step 1: Pre-scan bytecode to find all jump targets (basic block boundaries)
    std::set<uint32_t> blockStarts;
    blockStarts.insert(fn.codeOffset); // entry is always a block start

    uint32_t pc = fn.codeOffset;
    uint32_t end = fn.codeOffset + fn.codeLength;
    while (pc < end) {
        uint8_t op = module.code[pc++];
        switch (static_cast<bytecode::OpCode>(op)) {
            case bytecode::OpCode::JUMP: {
                uint16_t target = (module.code[pc] << 8) | module.code[pc+1];
                pc += 2;
                blockStarts.insert(target);
                blockStarts.insert(pc); // instruction after jump is also a block start
                break;
            }
            case bytecode::OpCode::JUMP_IF:
            case bytecode::OpCode::JUMP_IFNOT: {
                uint16_t target = (module.code[pc] << 8) | module.code[pc+1];
                pc += 2;
                blockStarts.insert(target);
                blockStarts.insert(pc); // fall-through is also a block start
                break;
            }
            case bytecode::OpCode::CONST_I32: pc += 4; break;
            case bytecode::OpCode::CONST_I64: pc += 8; break;
            case bytecode::OpCode::CONST_F32: pc += 4; break;
            case bytecode::OpCode::CONST_F64: pc += 8; break;
            case bytecode::OpCode::CONST_STR: pc += 2; break;
            case bytecode::OpCode::LOAD_LOCAL:
            case bytecode::OpCode::STORE_LOCAL:
            case bytecode::OpCode::LOAD_GLOBAL:
            case bytecode::OpCode::STORE_GLOBAL:
            case bytecode::OpCode::NEW_OBJ:
            case bytecode::OpCode::GET_FIELD:
            case bytecode::OpCode::SET_FIELD:
            case bytecode::OpCode::NEW_ARRAY:
                pc += 2; break;
            case bytecode::OpCode::CALL:
            case bytecode::OpCode::CALL_VIRTUAL:
            case bytecode::OpCode::CALL_ASYNC:
                pc += 3; break; // 2 bytes func + 1 byte argc
            case bytecode::OpCode::CAST: pc += 1; break;
            case bytecode::OpCode::IS_TYPE: pc += 2; break;
            case bytecode::OpCode::TRY_BEGIN: pc += 2; break;
            case bytecode::OpCode::LINE: pc += 4; break;
            case bytecode::OpCode::CLOSURE: pc += 3; break;
            case bytecode::OpCode::LOAD_CAPTURE: pc += 1; break;
            default: break; // single-byte opcodes
        }
    }

    // Step 2: Create LLVM basic blocks for each bytecode offset
    std::map<uint32_t, llvm::BasicBlock*> blocks;
    for (uint32_t offset : blockStarts) {
        if (offset >= fn.codeOffset && offset < end) {
            std::string name = "bb_" + std::to_string(offset);
            blocks[offset] = llvm::BasicBlock::Create(ctx, name, llvmFunc);
        }
    }

    if (blocks.empty()) return nullptr;

    // Create a dedicated entry block that branches to the first code block
    auto* entryBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunc, blocks.begin()->second);

    // Step 3: Create allocas for locals in the entry block
    llvm::IRBuilder<> builder(entryBlock);
    std::vector<llvm::AllocaInst*> locals(fn.localCount);
    for (int i = 0; i < fn.localCount; i++) {
        locals[i] = builder.CreateAlloca(i64Ty, nullptr, "local_" + std::to_string(i));
    }

    // Initialize params from args array (i64*)
    llvm::Value* argsPtr = llvmFunc->getArg(0);
    for (int i = 0; i < fn.paramCount && i < fn.localCount; i++) {
        auto* idx = builder.getInt32(i);
        auto* ptr = builder.CreateGEP(i64Ty, argsPtr, idx, "arg_ptr_" + std::to_string(i));
        auto* val = builder.CreateLoad(i64Ty, ptr, "arg_" + std::to_string(i));
        builder.CreateStore(val, locals[i]);
    }
    // Branch from entry to first code block
    builder.CreateBr(blocks.begin()->second);

    // Step 4: Walk bytecode and generate IR per basic block
    std::vector<llvm::Value*> irStack;
    pc = fn.codeOffset;

    while (pc < end) {
        // Switch to the correct basic block if this PC starts one
        auto blockIt = blocks.find(pc);
        if (blockIt != blocks.end()) {
            auto* newBlock = blockIt->second;
            // If current block doesn't have a terminator, add a branch to the new block
            if (builder.GetInsertBlock() && !builder.GetInsertBlock()->getTerminator()) {
                builder.CreateBr(newBlock);
            }
            builder.SetInsertPoint(newBlock);
            irStack.clear(); // stack doesn't persist across basic blocks in this simple model
        }

        uint8_t opcode = module.code[pc++];
        switch (static_cast<bytecode::OpCode>(opcode)) {
            case bytecode::OpCode::NOP: break;
            case bytecode::OpCode::LINE: pc += 4; break;

            case bytecode::OpCode::CONST_I32: {
                int32_t val = (module.code[pc] << 24) | (module.code[pc+1] << 16) |
                              (module.code[pc+2] << 8) | module.code[pc+3];
                pc += 4;
                irStack.push_back(builder.getInt64((int64_t)val));
                break;
            }
            case bytecode::OpCode::CONST_I64: {
                int64_t val = 0;
                for (int b = 0; b < 8; b++) val = (val << 8) | module.code[pc++];
                irStack.push_back(builder.getInt64(val));
                break;
            }
            case bytecode::OpCode::CONST_F64: {
                int64_t bits = 0;
                for (int b = 0; b < 8; b++) bits = (bits << 8) | module.code[pc++];
                irStack.push_back(builder.getInt64(bits)); // store double as raw bits
                break;
            }
            case bytecode::OpCode::CONST_F32: {
                int32_t bits = (module.code[pc] << 24) | (module.code[pc+1] << 16) |
                               (module.code[pc+2] << 8) | module.code[pc+3];
                pc += 4;
                float fval; std::memcpy(&fval, &bits, 4);
                double dval = (double)fval;
                int64_t dbits; std::memcpy(&dbits, &dval, 8);
                irStack.push_back(builder.getInt64(dbits));
                break;
            }
            case bytecode::OpCode::CONST_NULL: {
                irStack.push_back(builder.getInt64(0));
                break;
            }
            case bytecode::OpCode::CONST_TRUE: {
                irStack.push_back(builder.getInt64(1));
                break;
            }
            case bytecode::OpCode::CONST_FALSE: {
                irStack.push_back(builder.getInt64(0));
                break;
            }
            case bytecode::OpCode::CONST_STR: {
                pc += 2; // skip string constant index — can't handle strings in i64 JIT
                irStack.push_back(builder.getInt64(0)); // placeholder
                break;
            }
            case bytecode::OpCode::LOAD_LOCAL: {
                uint16_t idx = (module.code[pc] << 8) | module.code[pc+1]; pc += 2;
                if (idx < locals.size()) {
                    irStack.push_back(builder.CreateLoad(i64Ty, locals[idx]));
                } else {
                    irStack.push_back(builder.getInt64(0));
                }
                break;
            }
            case bytecode::OpCode::STORE_LOCAL: {
                uint16_t idx = (module.code[pc] << 8) | module.code[pc+1]; pc += 2;
                if (!irStack.empty() && idx < locals.size()) {
                    builder.CreateStore(irStack.back(), locals[idx]);
                    irStack.pop_back();
                }
                break;
            }
            case bytecode::OpCode::POP: {
                if (!irStack.empty()) irStack.pop_back();
                break;
            }
            case bytecode::OpCode::DUP: {
                if (!irStack.empty()) irStack.push_back(irStack.back());
                break;
            }
            case bytecode::OpCode::SWAP: {
                if (irStack.size() >= 2) {
                    auto sz = irStack.size();
                    std::swap(irStack[sz-1], irStack[sz-2]);
                }
                break;
            }

            // Arithmetic
            case bytecode::OpCode::ADD_I: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateAdd(a, b));
                break;
            }
            case bytecode::OpCode::SUB_I: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateSub(a, b));
                break;
            }
            case bytecode::OpCode::MUL_I: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateMul(a, b));
                break;
            }
            case bytecode::OpCode::DIV_I: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateSDiv(a, b));
                break;
            }
            case bytecode::OpCode::MOD_I: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateSRem(a, b));
                break;
            }
            case bytecode::OpCode::NEG_I: {
                if (irStack.empty()) goto bail;
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateNeg(a));
                break;
            }

            // Comparisons → i32 (0 or 1)
            case bytecode::OpCode::CMP_LT: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateZExt(builder.CreateICmpSLT(a, b), i64Ty));
                break;
            }
            case bytecode::OpCode::CMP_GT: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateZExt(builder.CreateICmpSGT(a, b), i64Ty));
                break;
            }
            case bytecode::OpCode::CMP_LE: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateZExt(builder.CreateICmpSLE(a, b), i64Ty));
                break;
            }
            case bytecode::OpCode::CMP_GE: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateZExt(builder.CreateICmpSGE(a, b), i64Ty));
                break;
            }
            case bytecode::OpCode::CMP_EQ: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateZExt(builder.CreateICmpEQ(a, b), i64Ty));
                break;
            }
            case bytecode::OpCode::CMP_NE: {
                if (irStack.size() < 2) goto bail;
                auto* b = irStack.back(); irStack.pop_back();
                auto* a = irStack.back(); irStack.pop_back();
                irStack.push_back(builder.CreateZExt(builder.CreateICmpNE(a, b), i64Ty));
                break;
            }

            // Control flow
            case bytecode::OpCode::JUMP: {
                uint16_t target = (module.code[pc] << 8) | module.code[pc+1]; pc += 2;
                auto tgtIt = blocks.find(target);
                if (tgtIt == blocks.end()) goto bail;
                builder.CreateBr(tgtIt->second);
                irStack.clear();
                // Skip dead code after unconditional jump
                while (pc < end && blocks.find(pc) == blocks.end()) pc++;
                break;
            }
            case bytecode::OpCode::JUMP_IF: {
                uint16_t target = (module.code[pc] << 8) | module.code[pc+1]; pc += 2;
                if (irStack.empty()) goto bail;
                auto* cond = irStack.back(); irStack.pop_back();
                auto* condBool = builder.CreateICmpNE(cond, builder.getInt64(0));
                auto tgtIt = blocks.find(target);
                auto fallIt = blocks.find(pc);
                if (tgtIt == blocks.end() || fallIt == blocks.end()) goto bail;
                builder.CreateCondBr(condBool, tgtIt->second, fallIt->second);
                break;
            }
            case bytecode::OpCode::JUMP_IFNOT: {
                uint16_t target = (module.code[pc] << 8) | module.code[pc+1]; pc += 2;
                if (irStack.empty()) goto bail;
                auto* cond = irStack.back(); irStack.pop_back();
                auto* condBool = builder.CreateICmpEQ(cond, builder.getInt64(0));
                auto tgtIt = blocks.find(target);
                auto fallIt = blocks.find(pc);
                if (tgtIt == blocks.end() || fallIt == blocks.end()) goto bail;
                builder.CreateCondBr(condBool, tgtIt->second, fallIt->second);
                break;
            }

            case bytecode::OpCode::RETURN: {
                if (!irStack.empty()) {
                    builder.CreateRet(irStack.back());
                } else {
                    builder.CreateRet(builder.getInt64(0));
                }
                irStack.clear();
                // Skip remaining bytes until next block start
                while (pc < end && blocks.find(pc) == blocks.end()) {
                    pc++; // skip dead code after return
                }
                break;
            }
            case bytecode::OpCode::RETURN_VOID: {
                builder.CreateRet(builder.getInt64(0));
                irStack.clear();
                while (pc < end && blocks.find(pc) == blocks.end()) {
                    pc++;
                }
                break;
            }

            // Function calls — bail for now (bridge needs full VM integration)
            case bytecode::OpCode::CALL:
            case bytecode::OpCode::CALL_VIRTUAL:
            case bytecode::OpCode::CALL_ASYNC:
                goto bail;

            default:
                // Unsupported opcode — can't JIT this function (silent bail)
                goto bail;
        }
    }

    // Ensure all blocks have terminators
    for (auto& [offset, block] : blocks) {
        if (!block->getTerminator()) {
            builder.SetInsertPoint(block);
            builder.CreateRet(builder.getInt64(0));
        }
    }

    return mod;

bail:
    return nullptr;
}

} // namespace jit
} // namespace gard
