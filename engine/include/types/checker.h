#pragma once

#include "types/types.h"
#include "sema/symbol.h"
#include "sema/analyzer.h"
#include "parser/ast.h"
#include <vector>
#include <string>

namespace gard {

class TypeChecker {
public:
    TypeChecker(SymbolTable& symbols);

    // Type check a full program
    void check(Program& program);

    // Results
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }
    bool hasErrors() const;
    int errorCount() const;
    int warningCount() const;

private:
    // --- Statement type checking ---
    void checkStatement(Statement* stmt);
    void checkBlock(BlockStmt* stmt);
    void checkVarDeclaration(VarDeclarationStmt* stmt);
    void checkFunctionDecl(FunctionDeclStmt* stmt);
    void checkClassDecl(ClassDeclStmt* stmt);
    void checkReturn(ReturnStmt* stmt);
    void checkIf(IfStmt* stmt);
    void checkFor(ForStmt* stmt);
    void checkForEach(ForEachStmt* stmt);
    void checkWhile(WhileStmt* stmt);
    void checkDoWhile(DoWhileStmt* stmt);
    void checkSwitch(SwitchStmt* stmt);
    void checkMatch(MatchStmt* stmt);
    void checkThrow(ThrowStmt* stmt);
    void checkTryCatch(TryCatchStmt* stmt);
    void checkExpressionStmt(ExpressionStmt* stmt);
    void checkExport(ExportStmt* stmt);
    void checkBlockchainContract(BlockchainContractStmt* stmt);

    // --- Expression type inference ---
    TypeRef inferExpression(Expression* expr);
    TypeRef inferLiteral(Expression* expr);
    TypeRef inferIdentifier(IdentifierExpr* expr);
    TypeRef inferBinary(BinaryExpr* expr);
    TypeRef inferUnary(UnaryExpr* expr);
    TypeRef inferCall(CallExpr* expr);
    TypeRef inferMemberAccess(MemberAccessExpr* expr);
    TypeRef inferIndexAccess(IndexAccessExpr* expr);
    TypeRef inferAssignment(AssignmentExpr* expr);
    TypeRef inferTernary(TernaryExpr* expr);
    TypeRef inferNew(NewExpr* expr);
    TypeRef inferAwait(AwaitExpr* expr);
    TypeRef inferLambda(LambdaExpr* expr);
    TypeRef inferArray(ArrayExpr* expr);
    TypeRef inferMap(MapExpr* expr);

    // --- Type resolution ---
    TypeRef resolveTypeAnnotation(TypeAnnotation* annotation);
    void registerClassType(ClassDeclStmt* cls);
    void registerInterfaceType(InterfaceDeclStmt* iface);

    // --- Compatibility checks ---
    void checkAssignability(const TypeRef& from, const TypeRef& to,
                           const SourceLocation& loc, const std::string& context);
    void checkConditionType(Expression* expr);

    // --- Diagnostics ---
    void error(const std::string& message, const SourceLocation& loc);
    void warning(const std::string& message, const SourceLocation& loc);

    // --- State ---
    SymbolTable& symbols_;
    TypeRegistry registry_;
    std::vector<Diagnostic> diagnostics_;

    // Current function context
    TypeRef currentReturnType_;
    std::string currentFunctionName_;

    // Type environment (variable name -> type) with scope stack
    std::vector<std::unordered_map<std::string, TypeRef>> typeEnv_;
    void pushTypeEnv();
    void popTypeEnv();
    void setVarType(const std::string& name, TypeRef type);
    TypeRef getVarType(const std::string& name) const;
};

} // namespace gard
