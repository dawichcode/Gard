#pragma once

#include "ir/ir.h"
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gard {
namespace codegen {

// Target architecture
enum class Target {
    X86_64,
    ARM64,
    WASM32,
};

// Emits textual LLVM IR (.ll) from Gard IR
// The output can be compiled with: llc output.ll -o output.o && clang output.o -o binary
class LLVMEmitter {
public:
    LLVMEmitter(Target target = Target::X86_64);

    // Emit LLVM IR text for a module
    std::string emit(const ir::Module& module);

    // Configuration
    void setDebugInfo(bool enable) { emitDebugInfo_ = enable; }
    void setUnchecked(bool enable) { unchecked_ = enable; }
    void setSourceFile(const std::string& file) { sourceFile_ = file; }
    void setSourceDir(const std::string& dir) { sourceDir_ = dir; }

    // Source map generation (JSON format)
    std::string generateSourceMap() const;

private:
    void emitHeader();
    void emitStringConstants(const ir::Module& module);
    void emitClassStructTypes(const ir::Module& module);
    void emitRuntimeDeclarations();
    void emitFunction(const ir::Function& fn);
    void emitBasicBlock(const ir::BasicBlock& block);
    void emitInstruction(const ir::Instruction& inst);
    void emitDebugMetadata();

    std::string valueRef(const ir::Value* val);
    std::string inferValueType(const ir::Value* val);
    std::string llvmType(ir::IRType type);
    std::string freshName(const std::string& prefix);
    std::string escapeLLVMString(const std::string& str);

    std::ostringstream out_;
    Target target_;
    std::string targetTriple_;
    bool emitDebugInfo_ = false;
    bool unchecked_ = false;
    std::string sourceFile_ = "module.gard";
    std::string sourceDir_ = ".";
    int nameCounter_ = 0;

    std::unordered_map<const ir::Value*, std::string> valueNames_;
    std::unordered_map<const ir::Value*, std::string> allocaTypes_; // alloca → stored type
    std::unordered_map<const ir::Value*, std::string> emittedTypes_; // value → actual LLVM type emitted
    std::unordered_map<std::string, std::string> stringGlobals_; // string value -> @.str.N
    std::unordered_map<std::string, int> classFieldIndices_; // "ClassName.fieldName" -> index
    std::unordered_map<std::string, int> classFieldCounts_; // "ClassName" -> field count
    std::string lastNewObjClass_; // track last NewObject class for field access
    std::unordered_map<const ir::Value*, std::string> objClassMap_; // value → class name
    std::unordered_map<const ir::Value*, std::string> allocaClassMap_; // alloca → class stored in it
    std::unordered_set<const ir::Value*> exceptionAllocas_; // allocas holding exception objects
    std::vector<std::string> tryPrevJmpbufs_; // stack of previous jmpbuf names for TryEnd
    std::vector<std::string> catchPrevJmpbufs_; // stack of previous jmpbuf names for CatchException
    int strCounter_ = 0;
    bool currentIsMain_ = false;
    bool currentIsAsync_ = false;
    std::string currentRetType_ = "void";
    std::string currentFuncName_; // current function name for null guard panic messages
    const ir::Module* currentModule_ = nullptr;

    // Debug info tracking
    int debugMetadataId_ = 0;
    int currentFuncDbgId_ = -1;
    struct DebugFuncInfo {
        std::string name;
        int line;
        int metadataId;
    };
    std::vector<DebugFuncInfo> debugFunctions_;

    // Source map entries: LLVM IR line -> source line
    struct SourceMapEntry {
        int irLine;
        int sourceLine;
        int sourceCol;
        std::string functionName;
    };
    mutable std::vector<SourceMapEntry> sourceMapEntries_;
    int currentIRLine_ = 0;
};

} // namespace codegen
} // namespace gard
