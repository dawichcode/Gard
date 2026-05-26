#pragma once

#include "parser/ast.h"
#include <string>
#include <sstream>

namespace gard {
namespace tools {

struct FormatConfig {
    int indentSize = 4;
    bool useSpaces = true;
    int maxLineLength = 100;
    bool trailingComma = false;
    bool sortImports = true;
    bool braceOnSameLine = true;
};

class Formatter {
public:
    Formatter(const FormatConfig& config = FormatConfig());

    // Format a program AST back to source
    std::string format(const Program& program);

private:
    // Statement formatting
    void formatStatement(Statement* stmt);
    void formatBlock(const std::vector<StmtPtr>& stmts);
    void formatVarDecl(VarDeclarationStmt* stmt);
    void formatFunctionDecl(FunctionDeclStmt* stmt);
    void formatClassDecl(ClassDeclStmt* stmt);
    void formatInterfaceDecl(InterfaceDeclStmt* stmt);
    void formatImport(ImportStmt* stmt);
    void formatExport(ExportStmt* stmt);
    void formatIf(IfStmt* stmt);
    void formatFor(ForStmt* stmt);
    void formatForEach(ForEachStmt* stmt);
    void formatWhile(WhileStmt* stmt);
    void formatDoWhile(DoWhileStmt* stmt);
    void formatSwitch(SwitchStmt* stmt);
    void formatMatch(MatchStmt* stmt);
    void formatReturn(ReturnStmt* stmt);
    void formatThrow(ThrowStmt* stmt);
    void formatTryCatch(TryCatchStmt* stmt);
    void formatExprStmt(ExpressionStmt* stmt);
    void formatBlockchainContract(BlockchainContractStmt* stmt);

    // Expression formatting
    std::string formatExpr(Expression* expr);
    std::string formatType(TypeAnnotation* type);

    // Helpers
    void emit(const std::string& text);
    void emitLine(const std::string& text = "");
    void indent();
    void dedent();
    std::string indentStr() const;

    FormatConfig config_;
    std::ostringstream out_;
    int indentLevel_ = 0;
};

} // namespace tools
} // namespace gard
