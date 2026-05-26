#pragma once

#include "ir/ir.h"
#include "parser/ast.h"
#include "types/types.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gard {
namespace ir {

// AST-to-IR lowering
class IRGenerator {
public:
    IRGenerator();

    // Generate IR for a full program
    std::unique_ptr<Module> generate(Program& program);

    // Error reporting
    const std::vector<std::string>& getErrors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    // --- Statement lowering ---
    void lowerStatement(Statement* stmt);
    void lowerBlock(BlockStmt* stmt);
    void lowerVarDeclaration(VarDeclarationStmt* stmt);
    void lowerFunctionDecl(FunctionDeclStmt* stmt);
    void lowerClassDecl(ClassDeclStmt* stmt);
    void lowerEnumDecl(EnumDeclStmt* stmt);
    void lowerReturn(ReturnStmt* stmt);
    void lowerIf(IfStmt* stmt);
    void lowerFor(ForStmt* stmt);
    void lowerForEach(ForEachStmt* stmt);
    void lowerWhile(WhileStmt* stmt);
    void lowerDoWhile(DoWhileStmt* stmt);
    void lowerSwitch(SwitchStmt* stmt);
    void lowerMatch(MatchStmt* stmt);
    void lowerThrow(ThrowStmt* stmt);
    void lowerTryCatch(TryCatchStmt* stmt);
    void lowerExpressionStmt(ExpressionStmt* stmt);
    void lowerExport(ExportStmt* stmt);
    void lowerImport(ImportStmt* stmt);
    void lowerBlockchainContract(BlockchainContractStmt* stmt);

    // --- Expression lowering ---
    ValueRef lowerExpression(Expression* expr);
    ValueRef lowerLiteral(Expression* expr);
    ValueRef lowerIdentifier(IdentifierExpr* expr);
    ValueRef lowerBinary(BinaryExpr* expr);
    ValueRef lowerUnary(UnaryExpr* expr);
    ValueRef lowerCall(CallExpr* expr);
    ValueRef lowerMemberAccess(MemberAccessExpr* expr);
    ValueRef lowerIndexAccess(IndexAccessExpr* expr);
    ValueRef lowerAssignment(AssignmentExpr* expr);
    ValueRef lowerTernary(TernaryExpr* expr);
    ValueRef lowerNew(NewExpr* expr);
    ValueRef lowerAwait(AwaitExpr* expr);
    ValueRef lowerCast(CastExpr* expr);
    ValueRef lowerLambda(LambdaExpr* expr);
    ValueRef lowerArray(ArrayExpr* expr);
    ValueRef lowerMap(MapExpr* expr);
    ValueRef lowerSet(SetExpr* expr);

    // --- Helpers ---
    IRType mapType(const std::string& typeName);
    IRType mapTypeAnnotation(TypeAnnotation* annotation);
    ValueRef emit(Opcode op, IRType type, std::vector<ValueRef> operands = {});
    ValueRef emitCall(const std::string& funcName, std::vector<ValueRef> args, IRType retType);
    void emitBranch(BasicBlock* target);
    void emitCondBranch(ValueRef cond, BasicBlock* trueBlock, BasicBlock* falseBlock);
    void emitReturn(ValueRef value);
    void emitReturnVoid();
    void emitStore(ValueRef addr, ValueRef value);
    ValueRef emitLoad(ValueRef addr, IRType type);
    ValueRef emitAlloca(IRType type, const std::string& name);

    // Constant creation
    ValueRef makeConstInt(int64_t val, IRType type = IRType::Int32);
    ValueRef makeConstFloat(double val, IRType type = IRType::Float64);
    ValueRef makeConstBool(bool val);
    ValueRef makeConstString(const std::string& val);
    ValueRef makeConstNull();

    // Block management
    BasicBlock* createBlock(const std::string& label);
    void setInsertPoint(BasicBlock* block);

    // Variable tracking
    void declareVar(const std::string& name, ValueRef addr);
    ValueRef lookupVar(const std::string& name);

    // Scope management
    void pushScope();
    void popScope();

    // ID generation
    int nextId();
    std::string nextLabel(const std::string& prefix);

    // --- State ---
    std::unique_ptr<Module> module_;
    Function* currentFunction_ = nullptr;
    BasicBlock* currentBlock_ = nullptr;

    // Variable name -> alloca address
    std::vector<std::unordered_map<std::string, ValueRef>> varScopes_;

    // Variable name -> lambda function name (for calling lambdas stored in variables)
    std::unordered_map<std::string, std::string> lambdaNames_;

    // Last generated lambda name (for tracking assignments)
    std::string lastLambdaName_;

    // Track const variables to prevent reassignment
    std::unordered_set<std::string> constVars_;

    // Track already-imported files to prevent circular imports
    std::unordered_set<std::string> importedFiles_;

    // Break/continue targets for loops
    struct LoopContext {
        BasicBlock* breakTarget;
        BasicBlock* continueTarget;
    };
    std::vector<LoopContext> loopStack_;

    int idCounter_ = 0;
    int labelCounter_ = 0;
    int currentSourceLine_ = 0;
    int currentSourceColumn_ = 0;
    std::vector<std::string> errors_;
};

} // namespace ir
} // namespace gard
