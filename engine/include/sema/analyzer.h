#pragma once

#include "sema/symbol.h"
#include "parser/ast.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace gard {

// Diagnostic severity
enum class DiagSeverity {
    Error,
    Warning,
    Info,
};

// Diagnostic message
struct Diagnostic {
    DiagSeverity severity;
    std::string message;
    SourceLocation location;

    Diagnostic(DiagSeverity sev, const std::string& msg, const SourceLocation& loc)
        : severity(sev), message(msg), location(loc) {}

    std::string toString() const;
};

// --- Semantic Analyzer ---

class SemanticAnalyzer {
public:
    SemanticAnalyzer();

    // Analyze a full program
    void analyze(Program& program);

    // Results
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }
    bool hasErrors() const;
    int errorCount() const;
    int warningCount() const;

    // Access symbol table (for type checker later)
    SymbolTable& getSymbolTable() { return symbols_; }
    const SymbolTable& getSymbolTableConst() const { return symbols_; }

private:
    // --- Statement analysis ---
    void analyzeStatement(Statement* stmt);
    void analyzeBlock(BlockStmt* stmt);
    void analyzeVarDeclaration(VarDeclarationStmt* stmt);
    void analyzeFunctionDecl(FunctionDeclStmt* stmt);
    void analyzeClassDecl(ClassDeclStmt* stmt);
    void analyzeInterfaceDecl(InterfaceDeclStmt* stmt);
    void analyzeImport(ImportStmt* stmt);
    void analyzeExport(ExportStmt* stmt);
    void analyzeBlockchainContract(BlockchainContractStmt* stmt);
    void analyzeIf(IfStmt* stmt);
    void analyzeFor(ForStmt* stmt);
    void analyzeForEach(ForEachStmt* stmt);
    void analyzeWhile(WhileStmt* stmt);
    void analyzeDoWhile(DoWhileStmt* stmt);
    void analyzeSwitch(SwitchStmt* stmt);
    void analyzeMatch(MatchStmt* stmt);
    void analyzeReturn(ReturnStmt* stmt);
    void analyzeBreak(BreakStmt* stmt);
    void analyzeContinue(ContinueStmt* stmt);
    void analyzeThrow(ThrowStmt* stmt);
    void analyzeTryCatch(TryCatchStmt* stmt);
    void analyzeExpressionStmt(ExpressionStmt* stmt);

    // --- Expression analysis ---
    void analyzeExpression(Expression* expr);
    void analyzeIdentifier(IdentifierExpr* expr);
    void analyzeAssignment(AssignmentExpr* expr);
    void analyzeCall(CallExpr* expr);
    void analyzeMemberAccess(MemberAccessExpr* expr);
    void analyzeIndexAccess(IndexAccessExpr* expr);
    void analyzeBinary(BinaryExpr* expr);
    void analyzeUnary(UnaryExpr* expr);
    void analyzeNew(NewExpr* expr);
    void analyzeAwait(AwaitExpr* expr);
    void analyzeLambda(LambdaExpr* expr);
    void analyzeThis(ThisExpr* expr);

    // --- Class analysis helpers ---
    void analyzeClassMethod(ClassMethod& method, const std::string& className);
    void analyzeClassField(ClassField& field);
    void analyzeConstructor(Constructor& ctor, const std::string& className);
    void checkAbstractImplementation(ClassDeclStmt* cls);
    void checkInterfaceImplementation(ClassDeclStmt* cls);

    // --- Module tracking ---
    void registerModule(const std::string& path);

    // --- Diagnostics ---
    void error(const std::string& message, const SourceLocation& loc);
    void warning(const std::string& message, const SourceLocation& loc);
    void info(const std::string& message, const SourceLocation& loc);

    // --- Unused detection ---
    void checkUnused();

    // --- State ---
    SymbolTable symbols_;
    std::vector<Diagnostic> diagnostics_;
    Program* currentProgram_ = nullptr;

    // Track imported modules for circular dependency detection
    std::unordered_set<std::string> importedModules_;
    std::string currentModule_;

    // Track return statements in current function
    bool currentFunctionHasReturn_ = false;
    std::string currentFunctionReturnType_;
};

} // namespace gard
