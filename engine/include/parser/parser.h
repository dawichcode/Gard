#pragma once

#include "parser/ast.h"
#include "lexer/token.h"
#include <vector>
#include <string>
#include <functional>

namespace gard {

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const std::string& filename = "<stdin>");

    // Parse the entire program
    Program parse();

    // Error reporting
    const std::vector<std::string>& getErrors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    // --- Token stream operations ---
    const Token& current() const;
    const Token& peek() const;
    const Token& previous() const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool matchAny(std::initializer_list<TokenType> types);
    Token expect(TokenType type, const std::string& message);
    bool isAtEnd() const;

    // --- Precedence levels for Pratt parser ---
    enum class Precedence {
        None = 0,
        Assignment,     // = += -= *= /=
        Ternary,        // ?:
        NullCoalesce,   // ??
        Or,             // ||
        And,            // &&
        BitOr,          // |
        BitXor,         // ^
        BitAnd,         // &
        Equality,       // == !=
        Comparison,     // < > <= >=
        Shift,          // << >> >>>
        Addition,       // + -
        Multiplication, // * / %
        Unary,          // - ! ~
        Postfix,        // ++ -- . ?. [] ()
        Primary,
    };

    Precedence getTokenPrecedence(TokenType type) const;

    // --- Expression parsing (Pratt parser) ---
    ExprPtr parseExpression(Precedence minPrec = Precedence::None);
    ExprPtr parsePrefixExpression();
    ExprPtr parseInfixExpression(ExprPtr left, Precedence prec);

    // Primary expressions
    ExprPtr parsePrimary();
    ExprPtr parseLiteral();
    ExprPtr parseIdentifierExpr();
    ExprPtr parseFunctionExpression();
    ExprPtr parseGroupedOrLambda();
    ExprPtr parseNewExpression();
    ExprPtr parseAwaitExpression();
    ExprPtr parseArrayLiteral();
    ExprPtr parseMapLiteral();

    // Postfix
    ExprPtr parsePostfix(ExprPtr left);
    ExprPtr parseCallExpr(ExprPtr callee);
    ExprPtr parseMemberAccess(ExprPtr object);
    ExprPtr parseIndexAccess(ExprPtr object);

    // --- Statement parsing ---
    StmtPtr parseStatement();
    StmtPtr parseDeclaration();

    // Declarations
    StmtPtr parseVarDeclaration(VarDeclKind kind);
    StmtPtr parseFunctionDeclaration(const std::string& access, bool isStatic, bool isAsync);
    StmtPtr parseClassDeclaration(bool isAbstract, const std::vector<Annotation>& annotations);
    StmtPtr parseInterfaceDeclaration();
    StmtPtr parseEnumDeclaration(const std::vector<Annotation>& annotations);
    StmtPtr parseImportStatement();
    StmtPtr parseExportStatement();
    StmtPtr parseBlockchainContract();

    // Statements
    StmtPtr parseBlock();
    StmtPtr parseIfStatement();
    StmtPtr parseForStatement();
    StmtPtr parseForEachStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseDoWhileStatement();
    StmtPtr parseSwitchStatement();
    StmtPtr parseMatchStatement();
    StmtPtr parseReturnStatement();
    StmtPtr parseBreakStatement();
    StmtPtr parseContinueStatement();
    StmtPtr parseThrowStatement();
    StmtPtr parseRethrowStatement();
    StmtPtr parseTryCatchStatement();
    StmtPtr parseExpressionStatement();

    // --- Type parsing ---
    TypePtr parseType();
    TypePtr parseBaseType();
    std::vector<GenericParam> parseGenericParams();
    std::vector<TypePtr> parseTypeArguments();

    // --- Class internals ---
    void parseClassBody(ClassDeclStmt& cls);
    void parseContractBody(BlockchainContractStmt& contract);
    ClassField parseClassField(const std::string& access, bool isStatic, VarDeclKind kind);
    ClassMethod parseClassMethod(const std::string& access, bool isStatic, bool isAsync, bool isAbstract);
    Constructor parseConstructor(const std::string& access);

    // --- Function parameters ---
    std::vector<FunctionParam> parseFunctionParams();

    // --- Annotations ---
    std::vector<Annotation> parseAnnotations();

    // --- Error recovery ---
    void synchronize();
    void reportError(const std::string& message);
    void reportError(const std::string& message, const SourceLocation& loc);

    // --- State ---
    std::vector<Token> tokens_;
    std::string filename_;
    int pos_;
    std::vector<std::string> errors_;
};

} // namespace gard
