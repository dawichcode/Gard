#include "parser/parser.h"
#include <stdexcept>
#include <algorithm>

namespace gard {

Parser::Parser(const std::vector<Token>& tokens, const std::string& filename)
    : tokens_(tokens), filename_(filename), pos_(0) {}

// --- Token stream operations ---

const Token& Parser::current() const {
    if (pos_ >= static_cast<int>(tokens_.size())) {
        return tokens_.back(); // EOF
    }
    return tokens_[pos_];
}

const Token& Parser::peek() const {
    int next = pos_ + 1;
    if (next >= static_cast<int>(tokens_.size())) {
        return tokens_.back();
    }
    return tokens_[next];
}

const Token& Parser::previous() const {
    if (pos_ > 0) return tokens_[pos_ - 1];
    return tokens_[0];
}

const Token& Parser::advance() {
    const Token& tok = current();
    if (!isAtEnd()) pos_++;
    return tok;
}

bool Parser::check(TokenType type) const {
    return current().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::matchAny(std::initializer_list<TokenType> types) {
    for (auto t : types) {
        if (check(t)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::expect(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    reportError(message + ", got '" + current().value + "' (" + current().typeName() + ")");
    // Advance past the unexpected token to prevent infinite loops
    return advance();
}

bool Parser::isAtEnd() const {
    return current().type == TokenType::EndOfFile;
}

// --- Precedence ---

Parser::Precedence Parser::getTokenPrecedence(TokenType type) const {
    switch (type) {
        case TokenType::Assign:
        case TokenType::PlusAssign:
        case TokenType::MinusAssign:
        case TokenType::StarAssign:
        case TokenType::SlashAssign:
            return Precedence::Assignment;
        case TokenType::QuestionMark:
            return Precedence::Ternary;
        case TokenType::NullCoalesce:
            return Precedence::NullCoalesce;
        case TokenType::Or:
            return Precedence::Or;
        case TokenType::And:
            return Precedence::And;
        case TokenType::BitOr:
            return Precedence::BitOr;
        case TokenType::BitXor:
            return Precedence::BitXor;
        case TokenType::BitAnd:
            return Precedence::BitAnd;
        case TokenType::Equal:
        case TokenType::NotEqual:
            return Precedence::Equality;
        case TokenType::Less:
        case TokenType::Greater:
        case TokenType::LessEqual:
        case TokenType::GreaterEqual:
        case TokenType::Is:
        case TokenType::As:
            return Precedence::Comparison;
        case TokenType::ShiftLeft:
        case TokenType::ShiftRight:
        case TokenType::UnsignedShiftRight:
            return Precedence::Shift;
        case TokenType::Plus:
        case TokenType::Minus:
            return Precedence::Addition;
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
            return Precedence::Multiplication;
        case TokenType::Dot:
        case TokenType::OptionalChain:
        case TokenType::LeftParen:
        case TokenType::LeftBracket:
        case TokenType::Increment:
        case TokenType::Decrement:
            return Precedence::Postfix;
        default:
            return Precedence::None;
    }
}

// --- Program ---

Program Parser::parse() {
    Program program;
    program.filename = filename_;

    while (!isAtEnd()) {
        try {
            auto stmt = parseDeclaration();
            if (stmt) {
                program.statements.push_back(std::move(stmt));
            }
        } catch (...) {
            synchronize();
        }
    }

    return program;
}

// --- Expression parsing (Pratt parser) ---

ExprPtr Parser::parseExpression(Precedence minPrec) {
    ExprPtr left = parsePrefixExpression();
    if (!left) return nullptr;

    while (!isAtEnd()) {
        Precedence prec = getTokenPrecedence(current().type);
        if (prec == Precedence::None) break;
        if (static_cast<int>(prec) < static_cast<int>(minPrec)) break;

        left = parseInfixExpression(std::move(left), prec);
        if (!left) return nullptr;
    }

    return left;
}

ExprPtr Parser::parsePrefixExpression() {
    auto loc = current().location;

    // Unary operators
    if (check(TokenType::Minus) || check(TokenType::Not) || check(TokenType::BitNot)) {
        TokenType op = advance().type;
        auto operand = parseExpression(Precedence::Unary);
        return std::make_unique<UnaryExpr>(op, std::move(operand), true, loc);
    }

    // Address-of operator: &variable (creates pointer)
    if (check(TokenType::BitAnd)) {
        advance(); // &
        auto operand = parseExpression(Precedence::Unary);
        return std::make_unique<UnaryExpr>(TokenType::BitAnd, std::move(operand), true, loc);
    }

    // Dereference operator: *ptr (reads value at pointer)
    if (check(TokenType::Star)) {
        advance(); // *
        auto operand = parseExpression(Precedence::Unary);
        return std::make_unique<UnaryExpr>(TokenType::Star, std::move(operand), true, loc);
    }

    // Prefix increment/decrement
    if (check(TokenType::Increment) || check(TokenType::Decrement)) {
        TokenType op = advance().type;
        auto operand = parseExpression(Precedence::Unary);
        return std::make_unique<UnaryExpr>(op, std::move(operand), true, loc);
    }

    // Await
    if (check(TokenType::Await)) {
        return parseAwaitExpression();
    }

    // Typeof
    if (check(TokenType::Typeof)) {
        advance();
        expect(TokenType::LeftParen, "Expected '(' after 'typeof'");
        auto expr = parseExpression();
        expect(TokenType::RightParen, "Expected ')' after typeof expression");
        // Represent as a call-like expression
        auto typeofId = std::make_unique<IdentifierExpr>("typeof", loc);
        std::vector<ExprPtr> args;
        args.push_back(std::move(expr));
        return std::make_unique<CallExpr>(std::move(typeofId), std::move(args), loc);
    }

    return parsePrimary();
}

ExprPtr Parser::parseInfixExpression(ExprPtr left, Precedence prec) {
    auto loc = current().location;

    // Postfix operators
    if (prec == Precedence::Postfix) {
        return parsePostfix(std::move(left));
    }

    // Ternary
    if (check(TokenType::QuestionMark)) {
        advance(); // ?
        auto thenExpr = parseExpression();
        expect(TokenType::Colon, "Expected ':' in ternary expression");
        auto elseExpr = parseExpression(Precedence::Ternary);
        return std::make_unique<TernaryExpr>(std::move(left), std::move(thenExpr),
                                             std::move(elseExpr), loc);
    }

    // Assignment operators (right-associative)
    if (current().type == TokenType::Assign || current().type == TokenType::PlusAssign ||
        current().type == TokenType::MinusAssign || current().type == TokenType::StarAssign ||
        current().type == TokenType::SlashAssign) {
        TokenType op = advance().type;
        auto value = parseExpression(Precedence::None); // right-associative
        return std::make_unique<AssignmentExpr>(std::move(left), op, std::move(value), loc);
    }

    // Cast expression: expr as Type
    if (check(TokenType::As)) {
        advance(); // consume 'as'
        auto targetType = parseType();
        return std::make_unique<CastExpr>(std::move(left), std::move(targetType), loc);
    }

    // Binary operators (left-associative: parse right side at higher precedence)
    TokenType op = advance().type;
    Precedence nextPrec = static_cast<Precedence>(static_cast<int>(prec) + 1);
    auto right = parseExpression(nextPrec);
    return std::make_unique<BinaryExpr>(std::move(left), op, std::move(right), loc);
}

// --- Primary expressions ---

ExprPtr Parser::parsePrimary() {
    auto loc = current().location;

    switch (current().type) {
        case TokenType::IntLiteral:
            return std::make_unique<IntLiteralExpr>(advance().value, loc);
        case TokenType::DoubleLiteral:
            return std::make_unique<DoubleLiteralExpr>(advance().value, loc);
        case TokenType::FloatLiteral:
            return std::make_unique<FloatLiteralExpr>(advance().value, loc);
        case TokenType::LongLiteral:
            return std::make_unique<LongLiteralExpr>(advance().value, loc);
        case TokenType::HexLiteral:
            return std::make_unique<HexLiteralExpr>(advance().value, loc);
        case TokenType::BinaryLiteral:
            return std::make_unique<BinaryLiteralExpr>(advance().value, loc);
        case TokenType::StringLiteral:
            return std::make_unique<StringLiteralExpr>(advance().value, loc);
        case TokenType::TemplateLiteral:
            return std::make_unique<TemplateLiteralExpr>(advance().value, loc);
        case TokenType::CharLiteral:
            return std::make_unique<CharLiteralExpr>(advance().value, loc);
        case TokenType::BoolLiteral:
            return std::make_unique<BoolLiteralExpr>(current().value == "true" ? (advance(), true) : (advance(), false), loc);
        case TokenType::NullLiteral:
            advance();
            return std::make_unique<NullLiteralExpr>(loc);
        case TokenType::This:
            advance();
            return std::make_unique<ThisExpr>(loc);
        case TokenType::Super:
            advance();
            return std::make_unique<SuperExpr>(loc);
        case TokenType::New:
            return parseNewExpression();
        case TokenType::LeftParen:
            return parseGroupedOrLambda();
        case TokenType::LeftBracket:
            return parseArrayLiteral();
        case TokenType::LeftBrace:
            return parseMapLiteral();
        case TokenType::Function:
            return parseFunctionExpression();
        case TokenType::Identifier:
            return parseIdentifierExpr();
        // Keywords that can be used as identifiers in expression context
        // (callable built-ins, or used as variable-like references)
        case TokenType::Print:
        case TokenType::Validate:
        case TokenType::Hash:
        case TokenType::Mine:
        case TokenType::Sign:
        case TokenType::Block:
        case TokenType::Transaction:
        case TokenType::Lock:
        case TokenType::Unlock:
        case TokenType::Wait:
        case TokenType::Signal:
        case TokenType::Ledger:
        case TokenType::Stream:
        case TokenType::Mutex:
        case TokenType::Semaphore:
        case TokenType::Barrier:
        {
            std::string name = advance().value;
            return std::make_unique<IdentifierExpr>(name, loc);
        }
        default:
            reportError("Unexpected token in expression: '" + current().value + "'");
            advance(); // skip
            return nullptr;
    }
}

ExprPtr Parser::parseIdentifierExpr() {
    auto loc = current().location;
    std::string name = advance().value;
    return std::make_unique<IdentifierExpr>(name, loc);
}

ExprPtr Parser::parseFunctionExpression() {
    auto loc = current().location;
    advance(); // consume 'function'

    // Optional function name (for named function expressions)
    std::string funcName;
    if (check(TokenType::Identifier)) {
        funcName = advance().value;
    }

    // Parameters
    expect(TokenType::LeftParen, "Expected '(' after function");
    std::vector<LambdaParam> params;
    while (!check(TokenType::RightParen) && !isAtEnd()) {
        LambdaParam param;
        param.name = current().value;
        advance();
        if (check(TokenType::Colon)) {
            advance();
            param.type = parseType();
        }
        params.push_back(std::move(param));
        if (check(TokenType::Comma)) advance();
    }
    expect(TokenType::RightParen, "Expected ')' after parameters");

    // Optional return type
    TypePtr retType = nullptr;
    if (check(TokenType::Colon)) {
        advance();
        retType = parseType();
    }

    // Body block
    expect(TokenType::LeftBrace, "Expected '{' for function body");
    std::vector<StmtPtr> body;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        auto stmt = parseDeclaration();
        if (stmt) body.push_back(std::move(stmt));
    }
    expect(TokenType::RightBrace, "Expected '}' after function body");

    return std::make_unique<LambdaExpr>(std::move(params), std::move(retType),
                                        std::move(body), loc);
}

ExprPtr Parser::parseGroupedOrLambda() {
    auto loc = current().location;
    advance(); // (

    // Empty parens => lambda with no params: () => ...
    if (check(TokenType::RightParen)) {
        advance(); // )
        if (check(TokenType::Arrow)) {
            advance(); // =>
            std::vector<LambdaParam> params;
            if (check(TokenType::LeftBrace)) {
                // Block body
                auto block = parseBlock();
                auto* blockStmt = dynamic_cast<BlockStmt*>(block.get());
                std::vector<StmtPtr> body;
                if (blockStmt) {
                    body = std::move(blockStmt->statements);
                }
                return std::make_unique<LambdaExpr>(std::move(params), nullptr, std::move(body), loc);
            } else {
                auto body = parseExpression();
                return std::make_unique<LambdaExpr>(std::move(params), nullptr, std::move(body), loc);
            }
        }
        // Empty grouped expression? That's an error
        reportError("Expected '=>' after empty parentheses or expression inside");
        return nullptr;
    }

    // Try to determine if this is a lambda or grouped expression
    // Heuristic: if we see "identifier :" or "identifier ," or "identifier )" followed by "=>"
    // it's likely a lambda
    bool isLambda = false;

    // Quick lookahead: try to parse as lambda params
    if (check(TokenType::Identifier)) {
        // Scan ahead to see if we find ) => pattern
        int depth = 1;
        int scanPos = pos_;
        while (scanPos < static_cast<int>(tokens_.size()) && depth > 0) {
            if (tokens_[scanPos].type == TokenType::LeftParen) depth++;
            if (tokens_[scanPos].type == TokenType::RightParen) depth--;
            if (depth == 0) {
                // Check if next token after ) is =>
                if (scanPos + 1 < static_cast<int>(tokens_.size()) &&
                    tokens_[scanPos + 1].type == TokenType::Arrow) {
                    isLambda = true;
                }
                break;
            }
            scanPos++;
        }
    }

    if (isLambda) {
        // Parse lambda parameters
        std::vector<LambdaParam> params;
        while (!check(TokenType::RightParen) && !isAtEnd()) {
            LambdaParam param;
            param.location = current().location;
            param.name = expect(TokenType::Identifier, "Expected parameter name").value;
            if (match(TokenType::Colon)) {
                param.type = parseType();
            }
            params.push_back(std::move(param));
            if (!check(TokenType::RightParen)) {
                expect(TokenType::Comma, "Expected ',' between lambda parameters");
            }
        }
        expect(TokenType::RightParen, "Expected ')' after lambda parameters");
        
        // Optional return type
        TypePtr returnType = nullptr;
        if (match(TokenType::Colon)) {
            returnType = parseType();
        }
        
        expect(TokenType::Arrow, "Expected '=>' in lambda expression");

        if (check(TokenType::LeftBrace)) {
            auto block = parseBlock();
            auto* blockStmt = dynamic_cast<BlockStmt*>(block.get());
            std::vector<StmtPtr> body;
            if (blockStmt) {
                body = std::move(blockStmt->statements);
            }
            return std::make_unique<LambdaExpr>(std::move(params), std::move(returnType), std::move(body), loc);
        } else {
            auto body = parseExpression();
            return std::make_unique<LambdaExpr>(std::move(params), std::move(returnType), std::move(body), loc);
        }
    }

    // Regular grouped expression (or tuple literal)
    auto expr = parseExpression();
    
    // Check if this is a tuple: (expr, expr, ...)
    if (check(TokenType::Comma)) {
        // It's a tuple / array-like expression — parse as array for now
        std::vector<ExprPtr> elements;
        elements.push_back(std::move(expr));
        while (match(TokenType::Comma)) {
            elements.push_back(parseExpression());
        }
        expect(TokenType::RightParen, "Expected ')' after tuple expression");
        return std::make_unique<ArrayExpr>(std::move(elements), loc);
    }

    expect(TokenType::RightParen, "Expected ')' after expression");
    return std::make_unique<GroupedExpr>(std::move(expr), loc);
}

ExprPtr Parser::parseNewExpression() {
    auto loc = current().location;
    advance(); // new

    // Class name can be an identifier OR a keyword used as a type name (e.g. transaction)
    std::string className;
    if (check(TokenType::Identifier) || current().isKeyword()) {
        className = advance().value;
    } else {
        reportError("Expected class name after 'new'");
        return nullptr;
    }

    // Optional type arguments
    std::vector<TypePtr> typeArgs;
    if (check(TokenType::Less)) {
        typeArgs = parseTypeArguments();
    }

    // Arguments
    expect(TokenType::LeftParen, "Expected '(' after class name in new expression");
    std::vector<ExprPtr> args;
    while (!check(TokenType::RightParen) && !isAtEnd()) {
        args.push_back(parseExpression());
        if (!check(TokenType::RightParen)) {
            expect(TokenType::Comma, "Expected ',' between arguments");
        }
    }
    expect(TokenType::RightParen, "Expected ')' after arguments");

    return std::make_unique<NewExpr>(className, std::move(typeArgs), std::move(args), loc);
}

ExprPtr Parser::parseAwaitExpression() {
    auto loc = current().location;
    advance(); // await
    auto operand = parseExpression(Precedence::Unary);
    return std::make_unique<AwaitExpr>(std::move(operand), loc);
}

ExprPtr Parser::parseArrayLiteral() {
    auto loc = current().location;
    advance(); // [

    std::vector<ExprPtr> elements;
    while (!check(TokenType::RightBracket) && !isAtEnd()) {
        elements.push_back(parseExpression());
        if (!check(TokenType::RightBracket)) {
            expect(TokenType::Comma, "Expected ',' between array elements");
        }
    }
    expect(TokenType::RightBracket, "Expected ']' after array literal");

    return std::make_unique<ArrayExpr>(std::move(elements), loc);
}

ExprPtr Parser::parseMapLiteral() {
    auto loc = current().location;
    advance(); // {

    // Empty braces = empty map
    if (check(TokenType::RightBrace)) {
        advance();
        return std::make_unique<MapExpr>(std::vector<std::pair<ExprPtr, ExprPtr>>{}, loc);
    }

    // Parse first expression to determine if this is a map or set
    // Allow keywords as map keys (like JavaScript): {memory: ..., from: ...}
    ExprPtr first;
    if (current().isKeyword() && peek().type == TokenType::Colon) {
        first = std::make_unique<IdentifierExpr>(advance().value, loc);
    } else {
        first = parseExpression();
    }

    if (check(TokenType::Colon)) {
        // It's a map: {key: value, ...}
        advance(); // :
        auto value = parseExpression();
        std::vector<std::pair<ExprPtr, ExprPtr>> entries;
        entries.push_back({std::move(first), std::move(value)});

        while (match(TokenType::Comma)) {
            if (check(TokenType::RightBrace)) break;
            ExprPtr key;
            if (current().isKeyword() && peek().type == TokenType::Colon) {
                key = std::make_unique<IdentifierExpr>(advance().value, current().location);
            } else {
                key = parseExpression();
            }
            expect(TokenType::Colon, "Expected ':' in map literal");
            auto val = parseExpression();
            entries.push_back({std::move(key), std::move(val)});
        }
        expect(TokenType::RightBrace, "Expected '}' after map literal");
        return std::make_unique<MapExpr>(std::move(entries), loc);
    } else {
        // It's a set literal: {val, val, ...}
        std::vector<ExprPtr> elements;
        elements.push_back(std::move(first));

        while (match(TokenType::Comma)) {
            if (check(TokenType::RightBrace)) break;
            elements.push_back(parseExpression());
        }
        expect(TokenType::RightBrace, "Expected '}' after set literal");
        return std::make_unique<SetExpr>(std::move(elements), loc);
    }
}

// --- Postfix ---

ExprPtr Parser::parsePostfix(ExprPtr left) {
    auto loc = current().location;

    if (check(TokenType::LeftParen)) {
        return parseCallExpr(std::move(left));
    }
    if (check(TokenType::Dot)) {
        return parseMemberAccess(std::move(left));
    }
    if (check(TokenType::OptionalChain)) {
        advance(); // ?.
        std::string member;
        if (check(TokenType::Identifier) || current().isKeyword()) {
            member = advance().value;
        } else {
            reportError("Expected member name after '?.'");
            member = "";
        }
        return std::make_unique<OptionalChainExpr>(std::move(left), member, loc);
    }
    if (check(TokenType::LeftBracket)) {
        return parseIndexAccess(std::move(left));
    }
    if (check(TokenType::Increment)) {
        advance();
        return std::make_unique<UnaryExpr>(TokenType::Increment, std::move(left), false, loc);
    }
    if (check(TokenType::Decrement)) {
        advance();
        return std::make_unique<UnaryExpr>(TokenType::Decrement, std::move(left), false, loc);
    }

    return left;
}

ExprPtr Parser::parseCallExpr(ExprPtr callee) {
    auto loc = current().location;
    advance(); // (

    std::vector<ExprPtr> args;
    while (!check(TokenType::RightParen) && !isAtEnd()) {
        // Support named arguments: name: value
        args.push_back(parseExpression());
        if (!check(TokenType::RightParen)) {
            expect(TokenType::Comma, "Expected ',' between arguments");
        }
    }
    expect(TokenType::RightParen, "Expected ')' after arguments");

    return std::make_unique<CallExpr>(std::move(callee), std::move(args), loc);
}

ExprPtr Parser::parseMemberAccess(ExprPtr object) {
    auto loc = current().location;
    advance(); // .
    // Member name can be an identifier or a keyword (e.g. .map, .set, .hash)
    std::string member;
    if (check(TokenType::Identifier) || current().isKeyword()) {
        member = advance().value;
    } else {
        reportError("Expected member name after '.'");
        member = "";
    }
    return std::make_unique<MemberAccessExpr>(std::move(object), member, loc);
}

ExprPtr Parser::parseIndexAccess(ExprPtr object) {
    auto loc = current().location;
    advance(); // [
    auto index = parseExpression();
    expect(TokenType::RightBracket, "Expected ']' after index");
    return std::make_unique<IndexAccessExpr>(std::move(object), std::move(index), loc);
}

// --- Statement parsing ---

StmtPtr Parser::parseDeclaration() {
    // Skip doc comments (attach to next declaration later if needed)
    while (match(TokenType::DocComment)) {}

    // Annotations
    std::vector<Annotation> annotations = parseAnnotations();

    auto loc = current().location;

    // Access modifiers and other prefixes
    std::string accessModifier;
    bool isStatic = false;
    bool isAsync = false;
    bool isAbstract = false;

    // Collect modifiers
    while (true) {
        if (check(TokenType::Public)) { accessModifier = "public"; advance(); }
        else if (check(TokenType::Private)) { accessModifier = "private"; advance(); }
        else if (check(TokenType::Protected)) { accessModifier = "protected"; advance(); }
        else if (check(TokenType::Static)) { isStatic = true; advance(); }
        else if (check(TokenType::Async)) { isAsync = true; advance(); }
        else if (check(TokenType::Abstract)) { isAbstract = true; advance(); }
        else break;
    }

    // Function declaration
    if (check(TokenType::Function)) {
        return parseFunctionDeclaration(accessModifier, isStatic, isAsync);
    }

    // Class declaration
    if (check(TokenType::Class)) {
        return parseClassDeclaration(isAbstract, annotations);
    }

    // Interface declaration
    if (check(TokenType::Interface)) {
        return parseInterfaceDeclaration();
    }

    // Enum declaration
    if (check(TokenType::Enum)) {
        return parseEnumDeclaration(annotations);
    }

    // Variable declarations
    if (check(TokenType::Let)) { advance(); return parseVarDeclaration(VarDeclKind::Let); }
    if (check(TokenType::Var)) { advance(); return parseVarDeclaration(VarDeclKind::Var); }
    if (check(TokenType::Const)) { advance(); return parseVarDeclaration(VarDeclKind::Const); }
    if (check(TokenType::Readonly)) { advance(); return parseVarDeclaration(VarDeclKind::Readonly); }

    // Import
    if (check(TokenType::Import)) { return parseImportStatement(); }

    // Export
    if (check(TokenType::Export)) { return parseExportStatement(); }

    // Blockchain contract
    if (check(TokenType::Blockchain)) { return parseBlockchainContract(); }

    // If we had modifiers but no declaration follows, that's an error
    if (!accessModifier.empty() || isStatic || isAsync) {
        reportError("Expected declaration after modifiers");
        synchronize();
        return nullptr;
    }

    return parseStatement();
}

StmtPtr Parser::parseStatement() {
    auto loc = current().location;

    if (check(TokenType::LeftBrace)) return parseBlock();
    if (check(TokenType::If)) return parseIfStatement();
    if (check(TokenType::For)) return parseForStatement();
    if (check(TokenType::Foreach)) return parseForEachStatement();
    if (check(TokenType::While)) return parseWhileStatement();
    if (check(TokenType::Do)) return parseDoWhileStatement();
    if (check(TokenType::Switch)) return parseSwitchStatement();
    if (check(TokenType::Match)) return parseMatchStatement();
    if (check(TokenType::Return)) return parseReturnStatement();
    if (check(TokenType::Break)) return parseBreakStatement();
    if (check(TokenType::Continue)) return parseContinueStatement();
    if (check(TokenType::Throw)) return parseThrowStatement();
    if (check(TokenType::Rethrow)) return parseRethrowStatement();
    if (check(TokenType::Try)) return parseTryCatchStatement();

    // lock(resource) { ... } — RAII-style mutex lock/unlock with exception safety
    // Desugars to: let __lock_N = resource; Mutex.lock(__lock_N); try { body } finally { Mutex.unlock(__lock_N); }
    if (check(TokenType::Lock)) {
        auto loc = current().location;
        advance(); // lock
        expect(TokenType::LeftParen, "Expected '(' after 'lock'");
        auto resource = parseExpression();
        expect(TokenType::RightParen, "Expected ')' after lock resource");
        auto body = parseBlock();

        auto* blockBody = static_cast<BlockStmt*>(body.get());

        static int lockCounter = 0;
        std::string lockVarName = "__lock_" + std::to_string(lockCounter++);

        std::vector<StmtPtr> outerStmts;

        // 1. let __lock_N = resource;
        outerStmts.push_back(std::make_unique<VarDeclarationStmt>(
            VarDeclKind::Let, lockVarName, nullptr, std::move(resource), loc));

        // 2. Mutex.lock(__lock_N);
        auto lockCallee = std::make_unique<MemberAccessExpr>(
            std::make_unique<IdentifierExpr>("Mutex", loc), "lock", loc);
        std::vector<ExprPtr> lockArgs;
        lockArgs.push_back(std::make_unique<IdentifierExpr>(lockVarName, loc));
        outerStmts.push_back(std::make_unique<ExpressionStmt>(
            std::make_unique<CallExpr>(std::move(lockCallee), std::move(lockArgs), loc), loc));

        // 3. try { body } finally { Mutex.unlock(__lock_N); }
        std::vector<StmtPtr> tryBody;
        for (auto& s : blockBody->statements) tryBody.push_back(std::move(s));

        std::vector<StmtPtr> finallyBody;
        auto unlockCallee = std::make_unique<MemberAccessExpr>(
            std::make_unique<IdentifierExpr>("Mutex", loc), "unlock", loc);
        std::vector<ExprPtr> unlockArgs;
        unlockArgs.push_back(std::make_unique<IdentifierExpr>(lockVarName, loc));
        finallyBody.push_back(std::make_unique<ExpressionStmt>(
            std::make_unique<CallExpr>(std::move(unlockCallee), std::move(unlockArgs), loc), loc));

        std::vector<CatchClause> emptyCatches;
        outerStmts.push_back(std::make_unique<TryCatchStmt>(
            std::move(tryBody), std::move(emptyCatches), std::move(finallyBody), loc));

        return std::make_unique<BlockStmt>(std::move(outerStmts), loc);
    }

    return parseExpressionStatement();
}

// --- Declarations ---

StmtPtr Parser::parseVarDeclaration(VarDeclKind kind) {
    auto loc = previous().location;

    // Variable name can be an identifier or a keyword used as a name
    std::string name;
    if (check(TokenType::Identifier) || current().isKeyword()) {
        name = advance().value;
    } else {
        reportError("Expected variable name");
        name = "";
    }

    TypePtr type = nullptr;
    if (match(TokenType::Colon)) {
        type = parseType();
    }

    ExprPtr initializer = nullptr;
    if (match(TokenType::Assign)) {
        initializer = parseExpression();
    }

    expect(TokenType::Semicolon, "Expected ';' after variable declaration");
    return std::make_unique<VarDeclarationStmt>(kind, name, std::move(type),
                                                 std::move(initializer), loc);
}

StmtPtr Parser::parseFunctionDeclaration(const std::string& access, bool isStatic, bool isAsync) {
    auto loc = current().location;
    advance(); // function

    // Function name can be an identifier or a keyword used as a name
    std::string name;
    if (check(TokenType::Identifier) || current().isKeyword()) {
        name = advance().value;
    } else {
        name = expect(TokenType::Identifier, "Expected function name").value;
    }

    auto stmt = std::make_unique<FunctionDeclStmt>(name, loc);
    stmt->accessModifier = access;
    stmt->isStatic = isStatic;
    stmt->isAsync = isAsync;

    // Generic parameters
    if (check(TokenType::Less)) {
        stmt->genericParams = parseGenericParams();
    }

    // Parameters
    expect(TokenType::LeftParen, "Expected '(' after function name");
    stmt->params = parseFunctionParams();
    expect(TokenType::RightParen, "Expected ')' after parameters");

    // Return type
    if (match(TokenType::Colon)) {
        stmt->returnType = parseType();
    }

    // Body
    if (check(TokenType::LeftBrace)) {
        auto block = parseBlock();
        auto* blockStmt = dynamic_cast<BlockStmt*>(block.get());
        if (blockStmt) {
            stmt->body = std::move(blockStmt->statements);
        }
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after function signature");
    }

    return stmt;
}

StmtPtr Parser::parseClassDeclaration(bool isAbstract, const std::vector<Annotation>& annotations) {
    auto loc = current().location;
    advance(); // class

    std::string name = expect(TokenType::Identifier, "Expected class name").value;

    auto cls = std::make_unique<ClassDeclStmt>(name, loc);
    cls->isAbstract = isAbstract;
    cls->annotations = annotations;

    // Generic parameters
    if (check(TokenType::Less)) {
        cls->genericParams = parseGenericParams();
    }

    // Extends
    if (match(TokenType::Extends)) {
        cls->baseClass = expect(TokenType::Identifier, "Expected base class name").value;
        // Skip generic type arguments on base class (e.g. extends List<T>)
        if (check(TokenType::Less)) {
            int depth = 1;
            advance(); // <
            while (!isAtEnd() && depth > 0) {
                if (check(TokenType::Less)) depth++;
                else if (check(TokenType::Greater)) depth--;
                advance();
            }
        }
    }

    // Implements
    if (match(TokenType::Implements)) {
        cls->interfaces.push_back(expect(TokenType::Identifier, "Expected interface name").value);
        while (match(TokenType::Comma)) {
            cls->interfaces.push_back(expect(TokenType::Identifier, "Expected interface name").value);
        }
    }

    // Body
    expect(TokenType::LeftBrace, "Expected '{' before class body");
    parseClassBody(*cls);
    expect(TokenType::RightBrace, "Expected '}' after class body");

    return cls;
}

StmtPtr Parser::parseInterfaceDeclaration() {
    auto loc = current().location;
    advance(); // interface

    std::string name = expect(TokenType::Identifier, "Expected interface name").value;

    auto iface = std::make_unique<InterfaceDeclStmt>(name, loc);

    // Generic parameters
    if (check(TokenType::Less)) {
        iface->genericParams = parseGenericParams();
    }

    // Extends
    if (match(TokenType::Extends)) {
        iface->extends.push_back(expect(TokenType::Identifier, "Expected interface name").value);
        while (match(TokenType::Comma)) {
            iface->extends.push_back(expect(TokenType::Identifier, "Expected interface name").value);
        }
    }

    expect(TokenType::LeftBrace, "Expected '{' before interface body");

    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        InterfaceMethod method;
        method.location = current().location;
        if (check(TokenType::Identifier) || current().isKeyword()) {
            method.name = advance().value;
        } else {
            method.name = expect(TokenType::Identifier, "Expected method name").value;
        }

        if (check(TokenType::Less)) {
            method.genericParams = parseGenericParams();
        }

        expect(TokenType::LeftParen, "Expected '(' after method name");
        method.params = parseFunctionParams();
        expect(TokenType::RightParen, "Expected ')' after parameters");

        if (match(TokenType::Colon)) {
            method.returnType = parseType();
        }

        expect(TokenType::Semicolon, "Expected ';' after interface method");
        iface->methods.push_back(std::move(method));
    }

    expect(TokenType::RightBrace, "Expected '}' after interface body");
    return iface;
}

StmtPtr Parser::parseEnumDeclaration(const std::vector<Annotation>& annotations) {
    auto loc = current().location;
    advance(); // enum

    std::string name = expect(TokenType::Identifier, "Expected enum name").value;
    auto enumDecl = std::make_unique<EnumDeclStmt>(name, loc);
    enumDecl->annotations = annotations;

    expect(TokenType::LeftBrace, "Expected '{' before enum body");

    // Parse variants: RED, GREEN, BLUE or RED(255, 0, 0), GREEN(0, 255, 0)
    bool parsingVariants = true;
    while (!check(TokenType::RightBrace) && !isAtEnd() && parsingVariants) {
        // Skip doc comments
        while (match(TokenType::DocComment)) {}

        // Check if we hit a semicolon (separator between variants and members)
        if (check(TokenType::Semicolon)) {
            advance();
            parsingVariants = false;
            break;
        }

        // Check if we hit a method/field declaration (no more variants)
        if (check(TokenType::Public) || check(TokenType::Private) || check(TokenType::Protected) ||
            check(TokenType::Static) || check(TokenType::Function) ||
            check(TokenType::Let) || check(TokenType::Var) || check(TokenType::Const)) {
            parsingVariants = false;
            break;
        }

        EnumVariant variant;
        variant.location = current().location;
        variant.name = expect(TokenType::Identifier, "Expected enum variant name").value;

        // Optional constructor args: RED(255, 0, 0)
        if (match(TokenType::LeftParen)) {
            while (!check(TokenType::RightParen) && !isAtEnd()) {
                variant.args.push_back(parseExpression());
                if (!check(TokenType::RightParen)) {
                    expect(TokenType::Comma, "Expected ',' between enum variant arguments");
                }
            }
            expect(TokenType::RightParen, "Expected ')' after enum variant arguments");
        }

        enumDecl->variants.push_back(std::move(variant));

        // Comma between variants is optional
        if (check(TokenType::Comma)) advance();
    }

    // Parse optional fields and methods (Java-style enum body after semicolon)
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        while (match(TokenType::DocComment)) {}

        std::vector<Annotation> memberAnnotations = parseAnnotations();

        std::string access;
        bool isStatic = false;

        if (match(TokenType::Public)) access = "public";
        else if (match(TokenType::Private)) access = "private";
        else if (match(TokenType::Protected)) access = "protected";

        if (match(TokenType::Static)) isStatic = true;

        if (check(TokenType::Function)) {
            auto method = parseClassMethod(access, isStatic, false, false);
            method.annotations = memberAnnotations;
            enumDecl->methods.push_back(std::move(method));
        } else if (check(TokenType::Let) || check(TokenType::Var) || check(TokenType::Const)) {
            VarDeclKind kind = VarDeclKind::Let;
            if (check(TokenType::Var)) kind = VarDeclKind::Var;
            else if (check(TokenType::Const)) kind = VarDeclKind::Const;
            advance();
            auto field = parseClassField(access, isStatic, kind);
            field.annotations = memberAnnotations;
            enumDecl->fields.push_back(std::move(field));
        } else if (check(TokenType::Identifier)) {
            // Constructor: EnumName(params) { ... }
            if (current().value == name && peek().type == TokenType::LeftParen) {
                advance(); // name
                advance(); // (
                auto params = parseFunctionParams();
                expect(TokenType::RightParen, "Expected ')' after constructor params");
                auto block = parseBlock();
                Constructor ctor;
                ctor.params = std::move(params);
                ctor.location = current().location;
                auto* blockStmt = dynamic_cast<BlockStmt*>(block.get());
                if (blockStmt) ctor.body = std::move(blockStmt->statements);
                enumDecl->constructor = std::move(ctor);
            } else {
                // Field without let/var
                auto field = parseClassField(access, isStatic, VarDeclKind::Let);
                field.annotations = memberAnnotations;
                enumDecl->fields.push_back(std::move(field));
            }
        } else {
            advance(); // skip unknown tokens
        }
    }

    expect(TokenType::RightBrace, "Expected '}' after enum body");
    return enumDecl;
}

StmtPtr Parser::parseImportStatement() {
    auto loc = current().location;
    advance(); // import

    std::vector<std::string> names;

    expect(TokenType::LeftBrace, "Expected '{' in import statement");
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        names.push_back(expect(TokenType::Identifier, "Expected import name").value);
        if (!check(TokenType::RightBrace)) {
            expect(TokenType::Comma, "Expected ',' between import names");
        }
    }
    expect(TokenType::RightBrace, "Expected '}' after import names");

    expect(TokenType::From, "Expected 'from' in import statement");
    std::string path = expect(TokenType::StringLiteral, "Expected module path string").value;
    expect(TokenType::Semicolon, "Expected ';' after import statement");

    return std::make_unique<ImportStmt>(std::move(names), path, loc);
}

StmtPtr Parser::parseExportStatement() {
    auto loc = current().location;
    advance(); // export

    auto decl = parseDeclaration();
    return std::make_unique<ExportStmt>(std::move(decl), loc);
}

StmtPtr Parser::parseBlockchainContract() {
    auto loc = current().location;
    advance(); // blockchain
    expect(TokenType::Contract, "Expected 'contract' after 'blockchain'");

    std::string name = expect(TokenType::Identifier, "Expected contract name").value;

    auto contract = std::make_unique<BlockchainContractStmt>(name, loc);

    expect(TokenType::LeftBrace, "Expected '{' before contract body");
    parseContractBody(*contract);
    expect(TokenType::RightBrace, "Expected '}' after contract body");

    return contract;
}

// --- Statements ---

StmtPtr Parser::parseBlock() {
    auto loc = current().location;
    expect(TokenType::LeftBrace, "Expected '{'");

    std::vector<StmtPtr> stmts;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        auto stmt = parseDeclaration();
        if (stmt) stmts.push_back(std::move(stmt));
    }

    expect(TokenType::RightBrace, "Expected '}'");
    return std::make_unique<BlockStmt>(std::move(stmts), loc);
}

StmtPtr Parser::parseIfStatement() {
    auto loc = current().location;
    advance(); // if

    expect(TokenType::LeftParen, "Expected '(' after 'if'");
    auto condition = parseExpression();
    expect(TokenType::RightParen, "Expected ')' after if condition");

    auto thenBranch = parseStatement();

    StmtPtr elseBranch = nullptr;
    if (match(TokenType::Else)) {
        elseBranch = parseStatement();
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch),
                                     std::move(elseBranch), loc);
}

StmtPtr Parser::parseForStatement() {
    auto loc = current().location;
    advance(); // for

    // Check for foreach-style: for await (let x of iter)
    bool isAwait = false;
    if (match(TokenType::Await)) {
        isAwait = true;
    }

    expect(TokenType::LeftParen, "Expected '(' after 'for'");

    // Check if this is a foreach: for (let x in/of collection)
    if (check(TokenType::Let) || check(TokenType::Var) || check(TokenType::Const)) {
        int savedPos = pos_;
        advance(); // let/var/const
        if (check(TokenType::Identifier)) {
            std::string varName = current().value;
            int afterName = pos_ + 1;
            if (afterName < static_cast<int>(tokens_.size()) &&
                (tokens_[afterName].type == TokenType::In || tokens_[afterName].type == TokenType::Of)) {
                // This is a foreach
                advance(); // variable name
                advance(); // in/of
                auto iterable = parseExpression();
                expect(TokenType::RightParen, "Expected ')' after for-each");
                auto body = parseStatement();
                return std::make_unique<ForEachStmt>(varName, nullptr, std::move(iterable),
                                                     std::move(body), isAwait, loc);
            }
            // Check with type annotation: for (let x: Type in/of ...)
            if (afterName < static_cast<int>(tokens_.size()) &&
                tokens_[afterName].type == TokenType::Colon) {
                advance(); // variable name
                advance(); // :
                auto type = parseType();
                if (check(TokenType::In) || check(TokenType::Of)) {
                    advance(); // in/of
                    auto iterable = parseExpression();
                    expect(TokenType::RightParen, "Expected ')' after for-each");
                    auto body = parseStatement();
                    return std::make_unique<ForEachStmt>(varName, std::move(type), std::move(iterable),
                                                         std::move(body), isAwait, loc);
                }
                // Not a foreach, restore and fall through to regular for
                pos_ = savedPos;
            } else {
                pos_ = savedPos;
            }
        } else {
            pos_ = savedPos;
        }
    }

    // Regular for loop: for (init; cond; inc)
    StmtPtr initializer = nullptr;
    if (!check(TokenType::Semicolon)) {
        if (check(TokenType::Let) || check(TokenType::Var) || check(TokenType::Const)) {
            VarDeclKind kind;
            if (check(TokenType::Let)) kind = VarDeclKind::Let;
            else if (check(TokenType::Var)) kind = VarDeclKind::Var;
            else kind = VarDeclKind::Const;
            advance();
            initializer = parseVarDeclaration(kind);
        } else {
            auto expr = parseExpression();
            expect(TokenType::Semicolon, "Expected ';' after for initializer");
            initializer = std::make_unique<ExpressionStmt>(std::move(expr), loc);
        }
    } else {
        advance(); // ;
    }

    ExprPtr condition = nullptr;
    if (!check(TokenType::Semicolon)) {
        condition = parseExpression();
    }
    expect(TokenType::Semicolon, "Expected ';' after for condition");

    ExprPtr increment = nullptr;
    if (!check(TokenType::RightParen)) {
        increment = parseExpression();
    }
    expect(TokenType::RightParen, "Expected ')' after for clauses");

    auto body = parseStatement();
    return std::make_unique<ForStmt>(std::move(initializer), std::move(condition),
                                      std::move(increment), std::move(body), loc);
}

StmtPtr Parser::parseForEachStatement() {
    auto loc = current().location;
    advance(); // foreach

    expect(TokenType::LeftParen, "Expected '(' after 'foreach'");

    std::string varName = expect(TokenType::Identifier, "Expected variable name in foreach").value;

    TypePtr varType = nullptr;
    if (match(TokenType::Colon)) {
        varType = parseType();
    }

    // Expect 'in'
    expect(TokenType::In, "Expected 'in' in foreach statement");

    auto iterable = parseExpression();
    expect(TokenType::RightParen, "Expected ')' after foreach");

    auto body = parseStatement();
    return std::make_unique<ForEachStmt>(varName, std::move(varType), std::move(iterable),
                                          std::move(body), false, loc);
}

StmtPtr Parser::parseWhileStatement() {
    auto loc = current().location;
    advance(); // while

    expect(TokenType::LeftParen, "Expected '(' after 'while'");
    auto condition = parseExpression();
    expect(TokenType::RightParen, "Expected ')' after while condition");

    auto body = parseStatement();
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body), loc);
}

StmtPtr Parser::parseDoWhileStatement() {
    auto loc = current().location;
    advance(); // do

    auto body = parseStatement();

    expect(TokenType::While, "Expected 'while' after do body");
    expect(TokenType::LeftParen, "Expected '(' after 'while'");
    auto condition = parseExpression();
    expect(TokenType::RightParen, "Expected ')' after condition");
    expect(TokenType::Semicolon, "Expected ';' after do-while");

    return std::make_unique<DoWhileStmt>(std::move(body), std::move(condition), loc);
}

StmtPtr Parser::parseSwitchStatement() {
    auto loc = current().location;
    advance(); // switch

    expect(TokenType::LeftParen, "Expected '(' after 'switch'");
    auto discriminant = parseExpression();
    expect(TokenType::RightParen, "Expected ')' after switch expression");

    expect(TokenType::LeftBrace, "Expected '{' before switch body");

    std::vector<SwitchCase> cases;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        SwitchCase sc;
        if (match(TokenType::Case)) {
            sc.isDefault = false;
            sc.value = parseExpression();
            expect(TokenType::Colon, "Expected ':' after case value");
        } else if (match(TokenType::Default)) {
            sc.isDefault = true;
            expect(TokenType::Colon, "Expected ':' after 'default'");
        } else {
            reportError("Expected 'case' or 'default' in switch");
            synchronize();
            continue;
        }

        while (!check(TokenType::Case) && !check(TokenType::Default) &&
               !check(TokenType::RightBrace) && !isAtEnd()) {
            auto stmt = parseDeclaration();
            if (stmt) sc.body.push_back(std::move(stmt));
        }
        cases.push_back(std::move(sc));
    }

    expect(TokenType::RightBrace, "Expected '}' after switch body");
    return std::make_unique<SwitchStmt>(std::move(discriminant), std::move(cases), loc);
}

StmtPtr Parser::parseMatchStatement() {
    auto loc = current().location;
    advance(); // match

    auto value = parseExpression();
    expect(TokenType::LeftBrace, "Expected '{' after match expression");

    std::vector<MatchArm> arms;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        MatchArm arm;
        // Handle wildcard/default patterns: _ or default
        if (check(TokenType::Identifier) && current().value == "_") {
            arm.pattern = std::make_unique<IdentifierExpr>("_", current().location);
            advance();
            arm.isDefault = true;
        } else if (check(TokenType::Default)) {
            arm.pattern = std::make_unique<IdentifierExpr>("_", current().location);
            advance();
            arm.isDefault = true;
        } else {
            arm.pattern = parseExpression(Precedence::Assignment);
            arm.isDefault = false;
        }
        expect(TokenType::Arrow, "Expected '=>' in match arm");
        arm.body = parseExpression(Precedence::Assignment);
        arms.push_back(std::move(arm));

        // Optional comma between arms
        match(TokenType::Comma);
    }

    expect(TokenType::RightBrace, "Expected '}' after match body");
    return std::make_unique<MatchStmt>(std::move(value), std::move(arms), loc);
}

StmtPtr Parser::parseReturnStatement() {
    auto loc = current().location;
    advance(); // return

    ExprPtr value = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = parseExpression();
    }
    expect(TokenType::Semicolon, "Expected ';' after return statement");

    return std::make_unique<ReturnStmt>(std::move(value), loc);
}

StmtPtr Parser::parseBreakStatement() {
    auto loc = current().location;
    advance(); // break
    expect(TokenType::Semicolon, "Expected ';' after 'break'");
    return std::make_unique<BreakStmt>(loc);
}

StmtPtr Parser::parseContinueStatement() {
    auto loc = current().location;
    advance(); // continue
    expect(TokenType::Semicolon, "Expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>(loc);
}

StmtPtr Parser::parseThrowStatement() {
    auto loc = current().location;
    advance(); // throw

    auto value = parseExpression();
    expect(TokenType::Semicolon, "Expected ';' after throw statement");

    return std::make_unique<ThrowStmt>(std::move(value), loc);
}

StmtPtr Parser::parseRethrowStatement() {
    auto loc = current().location;
    advance(); // rethrow
    expect(TokenType::Semicolon, "Expected ';' after rethrow");
    // Rethrow is represented as a ThrowStmt with null value (re-throws current exception)
    return std::make_unique<ThrowStmt>(nullptr, loc);
}

StmtPtr Parser::parseTryCatchStatement() {
    auto loc = current().location;
    advance(); // try

    // Try body
    expect(TokenType::LeftBrace, "Expected '{' after 'try'");
    std::vector<StmtPtr> tryBody;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        auto stmt = parseDeclaration();
        if (stmt) tryBody.push_back(std::move(stmt));
    }
    expect(TokenType::RightBrace, "Expected '}' after try body");

    // Catch clauses
    std::vector<CatchClause> catches;
    while (match(TokenType::Catch)) {
        CatchClause clause;
        clause.location = previous().location;

        expect(TokenType::LeftParen, "Expected '(' after 'catch'");
        clause.paramName = expect(TokenType::Identifier, "Expected catch parameter name").value;
        if (match(TokenType::Colon)) {
            clause.paramType = parseType();
        }
        expect(TokenType::RightParen, "Expected ')' after catch parameter");

        expect(TokenType::LeftBrace, "Expected '{' after catch");
        while (!check(TokenType::RightBrace) && !isAtEnd()) {
            auto stmt = parseDeclaration();
            if (stmt) clause.body.push_back(std::move(stmt));
        }
        expect(TokenType::RightBrace, "Expected '}' after catch body");

        catches.push_back(std::move(clause));
    }

    // Finally
    std::vector<StmtPtr> finallyBody;
    if (match(TokenType::Finally)) {
        expect(TokenType::LeftBrace, "Expected '{' after 'finally'");
        while (!check(TokenType::RightBrace) && !isAtEnd()) {
            auto stmt = parseDeclaration();
            if (stmt) finallyBody.push_back(std::move(stmt));
        }
        expect(TokenType::RightBrace, "Expected '}' after finally body");
    }

    return std::make_unique<TryCatchStmt>(std::move(tryBody), std::move(catches),
                                           std::move(finallyBody), loc);
}

StmtPtr Parser::parseExpressionStatement() {
    auto loc = current().location;
    auto expr = parseExpression();
    if (!expr) {
        synchronize();
        return nullptr;
    }
    expect(TokenType::Semicolon, "Expected ';' after expression");
    return std::make_unique<ExpressionStmt>(std::move(expr), loc);
}

// --- Type parsing ---

TypePtr Parser::parseType() {
    auto base = parseBaseType();
    if (!base) return nullptr;

    // Check for nullable: T?
    if (match(TokenType::QuestionMark)) {
        auto loc = previous().location;
        return std::make_unique<NullableType>(std::move(base), loc);
    }

    return base;
}

TypePtr Parser::parseBaseType() {
    auto loc = current().location;

    // Tuple type: (T1, T2, ...)
    if (check(TokenType::LeftParen)) {
        advance(); // (
        std::vector<TypePtr> elements;
        while (!check(TokenType::RightParen) && !isAtEnd()) {
            elements.push_back(parseType());
            if (!check(TokenType::RightParen)) {
                expect(TokenType::Comma, "Expected ',' between tuple type elements");
            }
        }
        expect(TokenType::RightParen, "Expected ')' after tuple type");

        // Check if it's a function type: (T1, T2) => ReturnType
        if (match(TokenType::Arrow)) {
            auto returnType = parseType();
            return std::make_unique<FunctionType>(std::move(elements), std::move(returnType), loc);
        }

        return std::make_unique<TupleType>(std::move(elements), loc);
    }

    // Named type or generic type
    std::string name;
    if (check(TokenType::Identifier) || current().isKeyword()) {
        // Type keywords are valid type names
        name = advance().value;
    } else {
        reportError("Expected type name");
        return nullptr;
    }

    // Check for generic arguments: Type<T1, T2>
    if (check(TokenType::Less)) {
        auto typeArgs = parseTypeArguments();
        return std::make_unique<GenericType>(name, std::move(typeArgs), loc);
    }

    return std::make_unique<NamedType>(name, loc);
}

std::vector<GenericParam> Parser::parseGenericParams() {
    std::vector<GenericParam> params;
    expect(TokenType::Less, "Expected '<'");

    while (!check(TokenType::Greater) && !isAtEnd()) {
        GenericParam param;
        param.location = current().location;

        // Variance: in/out keywords
        if (check(TokenType::Identifier) && current().value == "out") {
            param.variance = Variance::Covariant;
            advance();
        } else if (check(TokenType::Identifier) && current().value == "in") {
            param.variance = Variance::Contravariant;
            advance();
        }

        // Reified keyword
        if (check(TokenType::Identifier) && current().value == "reified") {
            param.reified = true;
            advance();
        }

        param.name = expect(TokenType::Identifier, "Expected type parameter name").value;

        // Constraints: <T: Comparable & Serializable>
        if (match(TokenType::Colon)) {
            TypePtr firstBound = parseType();
            param.constraints.push_back(std::move(firstBound));

            // Multiple bounds separated by &
            while (check(TokenType::BitAnd) || (check(TokenType::Identifier) && current().value == "&")) {
                advance(); // skip &
                TypePtr nextBound = parseType();
                param.constraints.push_back(std::move(nextBound));
            }

            // Set primary constraint to first bound (for backward compat)
            if (!param.constraints.empty()) {
                if (auto* named = dynamic_cast<NamedType*>(param.constraints[0].get())) {
                    param.constraint = std::make_unique<NamedType>(named->name, named->location);
                }
            }
        }

        params.push_back(std::move(param));
        if (!check(TokenType::Greater)) {
            expect(TokenType::Comma, "Expected ',' between type parameters");
        }
    }

    expect(TokenType::Greater, "Expected '>'");
    return params;
}

std::vector<TypePtr> Parser::parseTypeArguments() {
    std::vector<TypePtr> args;
    advance(); // <

    while (!check(TokenType::Greater) && !check(TokenType::ShiftRight) &&
           !check(TokenType::UnsignedShiftRight) && !isAtEnd()) {
        args.push_back(parseType());
        if (!check(TokenType::Greater) && !check(TokenType::ShiftRight) &&
            !check(TokenType::UnsignedShiftRight)) {
            expect(TokenType::Comma, "Expected ',' between type arguments");
        }
    }

    // Handle >> as two > tokens (nested generics like map<string, array<int>>)
    if (check(TokenType::ShiftRight)) {
        // Replace >> with a single > and leave one > for the outer context
        // We "consume" one > by changing the current token
        tokens_[pos_] = Token(TokenType::Greater, ">", current().location);
        return args;
    }
    if (check(TokenType::UnsignedShiftRight)) {
        // Replace >>> with >> (consume one >)
        tokens_[pos_] = Token(TokenType::ShiftRight, ">>", current().location);
        return args;
    }

    expect(TokenType::Greater, "Expected '>'");
    return args;
}

// --- Function parameters ---

std::vector<FunctionParam> Parser::parseFunctionParams() {
    std::vector<FunctionParam> params;

    while (!check(TokenType::RightParen) && !isAtEnd()) {
        FunctionParam param;
        param.location = current().location;

        // Rest parameter: ...args
        if (match(TokenType::Spread)) {
            param.isRest = true;
            param.name = expect(TokenType::Identifier, "Expected parameter name after '...'").value;
            if (match(TokenType::Colon)) {
                param.type = parseType();
            }
            params.push_back(std::move(param));
            break; // rest must be last parameter
        }

        param.name = expect(TokenType::Identifier, "Expected parameter name").value;

        if (match(TokenType::Colon)) {
            param.type = parseType();
        }

        // Default value
        if (match(TokenType::Assign)) {
            param.defaultValue = parseExpression();
        }

        params.push_back(std::move(param));
        if (!check(TokenType::RightParen)) {
            expect(TokenType::Comma, "Expected ',' between parameters");
        }
    }

    return params;
}

// --- Class internals ---

void Parser::parseClassBody(ClassDeclStmt& cls) {
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        // Skip doc comments
        while (match(TokenType::DocComment)) {}

        // Parse annotations
        std::vector<Annotation> annotations = parseAnnotations();

        auto loc = current().location;
        std::string access;
        bool isStatic = false;
        bool isAsync = false;
        bool isAbstract = false;

        // Collect modifiers
        while (true) {
            if (check(TokenType::Public)) { access = "public"; advance(); }
            else if (check(TokenType::Private)) { access = "private"; advance(); }
            else if (check(TokenType::Protected)) { access = "protected"; advance(); }
            else if (check(TokenType::Static)) { isStatic = true; advance(); }
            else if (check(TokenType::Async)) { isAsync = true; advance(); }
            else if (check(TokenType::Abstract)) { isAbstract = true; advance(); }
            else break;
        }

        // Constructor
        if (check(TokenType::Identifier) && current().value == "constructor") {
            cls.constructor = parseConstructor(access);
            continue;
        }

        // Method: function keyword
        if (check(TokenType::Function)) {
            auto method = parseClassMethod(access, isStatic, isAsync, isAbstract);
            method.annotations = annotations;
            cls.methods.push_back(std::move(method));
            continue;
        }

        // Field: let/var/const/readonly or identifier with type
        if (check(TokenType::Let) || check(TokenType::Var) ||
            check(TokenType::Const) || check(TokenType::Readonly)) {
            VarDeclKind kind;
            if (check(TokenType::Let)) kind = VarDeclKind::Let;
            else if (check(TokenType::Var)) kind = VarDeclKind::Var;
            else if (check(TokenType::Const)) kind = VarDeclKind::Const;
            else kind = VarDeclKind::Readonly;
            advance();
            auto field = parseClassField(access, isStatic, kind);
            field.annotations = annotations;
            cls.fields.push_back(std::move(field));
            continue;
        }

        // Field without let/var: just identifier with type annotation
        if (check(TokenType::Identifier)) {
            auto field = parseClassField(access, isStatic, VarDeclKind::Let);
            field.annotations = annotations;
            cls.fields.push_back(std::move(field));
            continue;
        }

        reportError("Unexpected token in class body: '" + current().value + "'");
        advance();
    }
}

void Parser::parseContractBody(BlockchainContractStmt& contract) {
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        while (match(TokenType::DocComment)) {}

        auto loc = current().location;
        std::string access;
        bool isStatic = false;
        bool isAsync = false;

        while (true) {
            if (check(TokenType::Public)) { access = "public"; advance(); }
            else if (check(TokenType::Private)) { access = "private"; advance(); }
            else if (check(TokenType::Protected)) { access = "protected"; advance(); }
            else if (check(TokenType::Static)) { isStatic = true; advance(); }
            else if (check(TokenType::Async)) { isAsync = true; advance(); }
            else break;
        }

        if (check(TokenType::Identifier) && current().value == "constructor") {
            contract.constructor = parseConstructor(access);
            continue;
        }

        if (check(TokenType::Function)) {
            contract.methods.push_back(parseClassMethod(access, isStatic, isAsync, false));
            continue;
        }

        // Ledger fields or regular fields
        if (check(TokenType::Ledger) || check(TokenType::Let) || check(TokenType::Var) ||
            check(TokenType::Const) || check(TokenType::Readonly)) {
            VarDeclKind kind = VarDeclKind::Let;
            if (check(TokenType::Const)) kind = VarDeclKind::Const;
            else if (check(TokenType::Readonly)) kind = VarDeclKind::Readonly;
            advance();
            contract.fields.push_back(parseClassField(access, isStatic, kind));
            continue;
        }

        if (check(TokenType::Identifier)) {
            contract.fields.push_back(parseClassField(access, isStatic, VarDeclKind::Let));
            continue;
        }

        reportError("Unexpected token in contract body: '" + current().value + "'");
        advance();
    }
}

ClassField Parser::parseClassField(const std::string& access, bool isStatic, VarDeclKind kind) {
    ClassField field;
    field.location = current().location;
    field.accessModifier = access;
    field.isStatic = isStatic;
    field.declKind = kind;

    field.name = (check(TokenType::Identifier) || current().isKeyword()) ?
        advance().value : expect(TokenType::Identifier, "Expected field name").value;

    if (match(TokenType::Colon)) {
        field.type = parseType();
    }

    if (match(TokenType::Assign)) {
        field.initializer = parseExpression();
    }

    expect(TokenType::Semicolon, "Expected ';' after field declaration");
    return field;
}

ClassMethod Parser::parseClassMethod(const std::string& access, bool isStatic, bool isAsync, bool isAbstract) {
    ClassMethod method;
    method.location = current().location;
    method.accessModifier = access;
    method.isStatic = isStatic;
    method.isAsync = isAsync;
    method.isAbstract = isAbstract;

    advance(); // function
    // Method name can be an identifier or a keyword
    if (check(TokenType::Identifier) || current().isKeyword()) {
        method.name = advance().value;
    } else {
        method.name = expect(TokenType::Identifier, "Expected method name").value;
    }

    if (check(TokenType::Less)) {
        method.genericParams = parseGenericParams();
    }

    expect(TokenType::LeftParen, "Expected '(' after method name");
    method.params = parseFunctionParams();
    expect(TokenType::RightParen, "Expected ')' after parameters");

    if (match(TokenType::Colon)) {
        method.returnType = parseType();
    }

    if (isAbstract) {
        expect(TokenType::Semicolon, "Expected ';' after abstract method");
    } else if (check(TokenType::LeftBrace)) {
        auto block = parseBlock();
        auto* blockStmt = dynamic_cast<BlockStmt*>(block.get());
        if (blockStmt) {
            method.body = std::move(blockStmt->statements);
        }
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after method signature");
    }

    return method;
}

Constructor Parser::parseConstructor(const std::string& access) {
    Constructor ctor;
    ctor.location = current().location;
    ctor.accessModifier = access;

    advance(); // constructor

    expect(TokenType::LeftParen, "Expected '(' after 'constructor'");
    ctor.params = parseFunctionParams();
    expect(TokenType::RightParen, "Expected ')' after constructor parameters");

    auto block = parseBlock();
    auto* blockStmt = dynamic_cast<BlockStmt*>(block.get());
    if (blockStmt) {
        ctor.body = std::move(blockStmt->statements);
    }

    return ctor;
}

// --- Annotations ---

std::vector<Annotation> Parser::parseAnnotations() {
    std::vector<Annotation> annotations;
    while (check(TokenType::At)) {
        advance(); // @
        std::string name = expect(TokenType::Identifier, "Expected annotation name after '@'").value;
        std::vector<AnnotationArg> args;

        // Optional arguments: @Name(key: value, key2: value2) or @Name(value)
        if (match(TokenType::LeftParen)) {
            while (!check(TokenType::RightParen) && !isAtEnd()) {
                AnnotationArg arg;
                // Check if it's key: value or just a positional value
                if (check(TokenType::Identifier) || check(TokenType::StringLiteral)) {
                    std::string first = current().value;
                    TokenType firstType = current().type;
                    advance();

                    if (check(TokenType::Colon)) {
                        // key: value format
                        advance(); // consume :
                        arg.key = first;
                        // Value can be string, number, bool, identifier
                        if (check(TokenType::StringLiteral)) {
                            arg.value = advance().value;
                        } else if (check(TokenType::IntLiteral) || check(TokenType::DoubleLiteral)) {
                            arg.value = advance().value;
                        } else if (check(TokenType::BoolLiteral)) {
                            arg.value = advance().value;
                        } else if (check(TokenType::Identifier)) {
                            arg.value = advance().value;
                        } else {
                            arg.value = current().value;
                            advance();
                        }
                    } else {
                        // Positional value (no key)
                        arg.key = "value";
                        arg.value = first;
                    }
                } else if (check(TokenType::IntLiteral) || check(TokenType::DoubleLiteral)) {
                    arg.key = "value";
                    arg.value = advance().value;
                } else if (check(TokenType::BoolLiteral)) {
                    arg.key = "value";
                    arg.value = advance().value;
                } else {
                    // Skip unknown token
                    advance();
                    continue;
                }
                args.push_back(arg);
                if (check(TokenType::Comma)) advance();
            }
            expect(TokenType::RightParen, "Expected ')' after annotation arguments");
        }

        annotations.emplace_back(name, std::move(args));
    }
    return annotations;
}

// --- Error recovery ---

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        if (previous().type == TokenType::Semicolon) return;

        switch (current().type) {
            case TokenType::Class:
            case TokenType::Function:
            case TokenType::Let:
            case TokenType::Var:
            case TokenType::Const:
            case TokenType::For:
            case TokenType::Foreach:
            case TokenType::If:
            case TokenType::While:
            case TokenType::Return:
            case TokenType::Import:
            case TokenType::Export:
            case TokenType::Blockchain:
            case TokenType::Try:
                return;
            default:
                advance();
        }
    }
}

void Parser::reportError(const std::string& message) {
    reportError(message, current().location);
}

void Parser::reportError(const std::string& message, const SourceLocation& loc) {
    std::string error = loc.toString() + ": GardSyntaxError: " + message;
    errors_.push_back(error);
}

} // namespace gard
