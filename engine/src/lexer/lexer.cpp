#include "lexer/lexer.h"
#include <stdexcept>

namespace gard {

Lexer::Lexer(const std::string& source, const std::string& filename)
    : source_(source), filename_(filename), pos_(0), line_(1), column_(1),
      tokenStartLine_(1), tokenStartColumn_(1) {}

// --- Character stream operations ---

char Lexer::current() const {
    if (isAtEndInternal()) return '\0';
    return source_[pos_];
}

char Lexer::peek() const {
    if (pos_ + 1 >= static_cast<int>(source_.size())) return '\0';
    return source_[pos_ + 1];
}

char Lexer::peekNext() const {
    if (pos_ + 2 >= static_cast<int>(source_.size())) return '\0';
    return source_[pos_ + 2];
}

char Lexer::advance() {
    char c = source_[pos_];
    pos_++;
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEndInternal()) return false;
    if (source_[pos_] != expected) return false;
    advance();
    return true;
}

bool Lexer::isAtEndInternal() const {
    return pos_ >= static_cast<int>(source_.size());
}

bool Lexer::isAtEnd() const {
    return isAtEndInternal();
}

// --- Character classification ---

bool Lexer::isDigit(char c) const {
    return c >= '0' && c <= '9';
}

bool Lexer::isAlpha(char c) const {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

bool Lexer::isAlphaNumeric(char c) const {
    return isAlpha(c) || isDigit(c);
}

bool Lexer::isHexDigit(char c) const {
    return isDigit(c) ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool Lexer::isBinaryDigit(char c) const {
    return c == '0' || c == '1';
}

// --- Whitespace and comments ---

void Lexer::skipWhitespace() {
    while (!isAtEndInternal()) {
        char c = current();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else {
            break;
        }
    }
}

void Lexer::skipSingleLineComment() {
    // Already consumed '//'
    while (!isAtEndInternal() && current() != '\n') {
        advance();
    }
}

std::string Lexer::scanDocCommentSingle() {
    // Already consumed '///', capture the rest of the line
    std::string content = "///";
    while (!isAtEndInternal() && current() != '\n') {
        content += advance();
    }
    return content;
}

void Lexer::skipMultiLineComment() {
    // Already consumed '/*'
    while (!isAtEndInternal()) {
        if (current() == '*' && peek() == '/') {
            advance(); // *
            advance(); // /
            return;
        }
        advance();
    }
    reportError("Unterminated multi-line comment");
}

std::string Lexer::scanDocCommentMulti() {
    // Already consumed '/**', capture until '*/'
    std::string content = "/**";
    while (!isAtEndInternal()) {
        if (current() == '*' && peek() == '/') {
            content += advance(); // *
            content += advance(); // /
            return content;
        }
        content += advance();
    }
    reportError("Unterminated doc comment");
    return content;
}

// --- Token scanning ---

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (!isAtEndInternal()) {
        Token tok = nextToken();
        tokens.push_back(tok);
        if (tok.type == TokenType::EndOfFile) break;
    }
    if (tokens.empty() || tokens.back().type != TokenType::EndOfFile) {
        tokens.push_back(makeToken(TokenType::EndOfFile, ""));
    }
    return tokens;
}

Token Lexer::nextToken() {
    skipWhitespace();

    if (isAtEndInternal()) {
        return makeToken(TokenType::EndOfFile, "");
    }

    tokenStartLine_ = line_;
    tokenStartColumn_ = column_;

    return scanToken();
}

Token Lexer::scanToken() {
    char c = current();

    // Comments
    if (c == '/') {
        if (peek() == '/') {
            SourceLocation loc = currentLocation();
            advance(); // first /
            advance(); // second /
            // Check for doc comment ///
            if (current() == '/') {
                advance(); // third /
                std::string content = "///";
                while (!isAtEndInternal() && current() != '\n') {
                    content += advance();
                }
                return makeToken(TokenType::DocComment, content, loc);
            }
            // Regular single-line comment, skip it
            skipSingleLineComment();
            skipWhitespace();
            if (isAtEndInternal()) {
                return makeToken(TokenType::EndOfFile, "");
            }
            tokenStartLine_ = line_;
            tokenStartColumn_ = column_;
            return scanToken();
        }
        if (peek() == '*') {
            SourceLocation loc = currentLocation();
            advance(); // /
            advance(); // *
            // Check for doc comment /**
            if (current() == '*' && peek() != '/') {
                std::string content = "/**";
                while (!isAtEndInternal()) {
                    if (current() == '*' && peek() == '/') {
                        content += advance(); // *
                        content += advance(); // /
                        return makeToken(TokenType::DocComment, content, loc);
                    }
                    content += advance();
                }
                reportError("Unterminated doc comment");
                return makeToken(TokenType::Invalid, content, loc);
            }
            // Regular multi-line comment
            skipMultiLineComment();
            skipWhitespace();
            if (isAtEndInternal()) {
                return makeToken(TokenType::EndOfFile, "");
            }
            tokenStartLine_ = line_;
            tokenStartColumn_ = column_;
            return scanToken();
        }
    }

    // Numbers
    if (isDigit(c)) {
        return scanNumber();
    }

    // Strings
    if (c == '"') {
        return scanString();
    }

    // Template strings
    if (c == '`') {
        return scanTemplateString();
    }

    // Character literals
    if (c == '\'') {
        return scanChar();
    }

    // Identifiers and keywords
    if (isAlpha(c)) {
        return scanIdentifierOrKeyword();
    }

    // Operators and punctuation
    return scanOperatorOrPunctuation();
}

Token Lexer::scanNumber() {
    SourceLocation loc = currentLocation();
    std::string value;

    // Check for hex (0x) or binary (0b)
    if (current() == '0') {
        if (peek() == 'x' || peek() == 'X') {
            value += advance(); // 0
            value += advance(); // x
            if (!isHexDigit(current())) {
                reportError("Expected hex digit after '0x'");
                return makeToken(TokenType::Invalid, value, loc);
            }
            while (!isAtEndInternal() && isHexDigit(current())) {
                value += advance();
            }
            return makeToken(TokenType::HexLiteral, value, loc);
        }
        if (peek() == 'b' || peek() == 'B') {
            value += advance(); // 0
            value += advance(); // b
            if (!isBinaryDigit(current())) {
                reportError("Expected binary digit after '0b'");
                return makeToken(TokenType::Invalid, value, loc);
            }
            while (!isAtEndInternal() && isBinaryDigit(current())) {
                value += advance();
            }
            return makeToken(TokenType::BinaryLiteral, value, loc);
        }
    }

    // Integer or floating point
    while (!isAtEndInternal() && isDigit(current())) {
        value += advance();
    }

    // Check for decimal point
    bool isFloat = false;
    if (!isAtEndInternal() && current() == '.' && isDigit(peek())) {
        isFloat = true;
        value += advance(); // .
        while (!isAtEndInternal() && isDigit(current())) {
            value += advance();
        }
    }

    // Check for exponent
    if (!isAtEndInternal() && (current() == 'e' || current() == 'E')) {
        isFloat = true;
        value += advance(); // e/E
        if (!isAtEndInternal() && (current() == '+' || current() == '-')) {
            value += advance();
        }
        if (!isAtEndInternal() && isDigit(current())) {
            while (!isAtEndInternal() && isDigit(current())) {
                value += advance();
            }
        } else {
            reportError("Expected digit in exponent");
        }
    }

    // Check for suffixes: L (long), f (float)
    if (!isAtEndInternal()) {
        if (current() == 'L' || current() == 'l') {
            value += advance();
            return makeToken(TokenType::LongLiteral, value, loc);
        }
        if (current() == 'f' || current() == 'F') {
            value += advance();
            return makeToken(TokenType::FloatLiteral, value, loc);
        }
    }

    if (isFloat) {
        return makeToken(TokenType::DoubleLiteral, value, loc);
    }
    return makeToken(TokenType::IntLiteral, value, loc);
}

Token Lexer::scanString() {
    SourceLocation loc = currentLocation();
    std::string value;
    advance(); // opening "

    while (!isAtEndInternal() && current() != '"') {
        if (current() == '\\') {
            advance(); // backslash
            if (isAtEndInternal()) {
                reportError("Unterminated string literal");
                return makeToken(TokenType::Invalid, value, loc);
            }
            char escaped = current();
            switch (escaped) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case '0': value += '\0'; break;
                default:
                    reportError(std::string("Unknown escape sequence: \\") + escaped);
                    value += escaped;
                    break;
            }
            advance();
        } else if (current() == '\n') {
            reportError("Unterminated string literal (newline in string)");
            return makeToken(TokenType::StringLiteral, value, loc);
        } else {
            value += advance();
        }
    }

    if (isAtEndInternal()) {
        reportError("Unterminated string literal");
        return makeToken(TokenType::Invalid, value, loc);
    }

    advance(); // closing "
    return makeToken(TokenType::StringLiteral, value, loc);
}

Token Lexer::scanTemplateString() {
    SourceLocation loc = currentLocation();
    std::string value;
    advance(); // opening `

    while (!isAtEndInternal() && current() != '`') {
        if (current() == '\\') {
            advance();
            if (isAtEndInternal()) {
                reportError("Unterminated template string");
                return makeToken(TokenType::Invalid, value, loc);
            }
            char escaped = current();
            switch (escaped) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '`': value += '`'; break;
                case '$': value += '$'; break;
                default:
                    value += '\\';
                    value += escaped;
                    break;
            }
            advance();
        } else if (current() == '$' && peek() == '{') {
            // Template interpolation marker - store as-is for parser to handle
            value += advance(); // $
            value += advance(); // {
            // Read until matching }
            int braceDepth = 1;
            while (!isAtEndInternal() && braceDepth > 0) {
                if (current() == '{') braceDepth++;
                if (current() == '}') braceDepth--;
                if (braceDepth > 0) {
                    value += advance();
                }
            }
            if (!isAtEndInternal()) {
                value += advance(); // closing }
            } else {
                reportError("Unterminated interpolation in template string");
            }
        } else {
            value += advance();
        }
    }

    if (isAtEndInternal()) {
        reportError("Unterminated template string");
        return makeToken(TokenType::Invalid, value, loc);
    }

    advance(); // closing `
    return makeToken(TokenType::TemplateLiteral, value, loc);
}

Token Lexer::scanChar() {
    SourceLocation loc = currentLocation();
    advance(); // opening '

    std::string value;
    if (isAtEndInternal()) {
        reportError("Unterminated character literal");
        return makeToken(TokenType::Invalid, "", loc);
    }

    if (current() == '\\') {
        advance(); // backslash
        if (isAtEndInternal()) {
            reportError("Unterminated character literal");
            return makeToken(TokenType::Invalid, value, loc);
        }
        char escaped = current();
        switch (escaped) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '\\': value += '\\'; break;
            case '\'': value += '\''; break;
            case '0': value += '\0'; break;
            default:
                reportError(std::string("Unknown escape sequence: \\") + escaped);
                value += escaped;
                break;
        }
        advance();
    } else {
        value += advance();
    }

    if (isAtEndInternal() || current() != '\'') {
        reportError("Unterminated character literal, expected closing '");
        return makeToken(TokenType::Invalid, value, loc);
    }

    advance(); // closing '
    return makeToken(TokenType::CharLiteral, value, loc);
}

Token Lexer::scanIdentifierOrKeyword() {
    SourceLocation loc = currentLocation();
    std::string value;

    while (!isAtEndInternal() && isAlphaNumeric(current())) {
        value += advance();
    }

    // Check if it's a keyword
    const auto& keywords = getKeywords();
    auto it = keywords.find(value);
    if (it != keywords.end()) {
        // Special handling for true/false as BoolLiteral
        if (it->second == TokenType::True || it->second == TokenType::False) {
            return makeToken(TokenType::BoolLiteral, value, loc);
        }
        // null as NullLiteral
        if (it->second == TokenType::Null) {
            return makeToken(TokenType::NullLiteral, value, loc);
        }
        return makeToken(it->second, value, loc);
    }

    return makeToken(TokenType::Identifier, value, loc);
}

Token Lexer::scanOperatorOrPunctuation() {
    SourceLocation loc = currentLocation();
    char c = advance();

    switch (c) {
        // Single-character punctuation
        case '(': return makeToken(TokenType::LeftParen, "(", loc);
        case ')': return makeToken(TokenType::RightParen, ")", loc);
        case '{': return makeToken(TokenType::LeftBrace, "{", loc);
        case '}': return makeToken(TokenType::RightBrace, "}", loc);
        case '[': return makeToken(TokenType::LeftBracket, "[", loc);
        case ']': return makeToken(TokenType::RightBracket, "]", loc);
        case ';': return makeToken(TokenType::Semicolon, ";", loc);
        case ':': return makeToken(TokenType::Colon, ":", loc);
        case ',': return makeToken(TokenType::Comma, ",", loc);
        case '@': return makeToken(TokenType::At, "@", loc);
        case '~': return makeToken(TokenType::BitNot, "~", loc);

        // Dot or Spread (...)
        case '.':
            if (current() == '.' && peek() == '.') { advance(); advance(); return makeToken(TokenType::Spread, "...", loc); }
            return makeToken(TokenType::Dot, ".", loc);

        // Operators that can be multi-character
        case '+':
            if (match('+')) return makeToken(TokenType::Increment, "++", loc);
            if (match('=')) return makeToken(TokenType::PlusAssign, "+=", loc);
            return makeToken(TokenType::Plus, "+", loc);

        case '-':
            if (match('-')) return makeToken(TokenType::Decrement, "--", loc);
            if (match('=')) return makeToken(TokenType::MinusAssign, "-=", loc);
            return makeToken(TokenType::Minus, "-", loc);

        case '*':
            if (match('=')) return makeToken(TokenType::StarAssign, "*=", loc);
            return makeToken(TokenType::Star, "*", loc);

        case '/':
            if (match('=')) return makeToken(TokenType::SlashAssign, "/=", loc);
            return makeToken(TokenType::Slash, "/", loc);

        case '%':
            return makeToken(TokenType::Percent, "%", loc);

        case '=':
            if (match('=')) return makeToken(TokenType::Equal, "==", loc);
            if (match('>')) return makeToken(TokenType::Arrow, "=>", loc);
            return makeToken(TokenType::Assign, "=", loc);

        case '!':
            if (match('=')) return makeToken(TokenType::NotEqual, "!=", loc);
            return makeToken(TokenType::Not, "!", loc);

        case '<':
            if (match('=')) return makeToken(TokenType::LessEqual, "<=", loc);
            if (match('<')) return makeToken(TokenType::ShiftLeft, "<<", loc);
            return makeToken(TokenType::Less, "<", loc);

        case '>':
            if (match('=')) return makeToken(TokenType::GreaterEqual, ">=", loc);
            if (match('>')) {
                if (match('>')) return makeToken(TokenType::UnsignedShiftRight, ">>>", loc);
                return makeToken(TokenType::ShiftRight, ">>", loc);
            }
            return makeToken(TokenType::Greater, ">", loc);

        case '&':
            if (match('&')) return makeToken(TokenType::And, "&&", loc);
            return makeToken(TokenType::BitAnd, "&", loc);

        case '|':
            if (match('|')) return makeToken(TokenType::Or, "||", loc);
            return makeToken(TokenType::BitOr, "|", loc);

        case '^':
            return makeToken(TokenType::BitXor, "^", loc);

        case '?':
            if (match('.')) return makeToken(TokenType::OptionalChain, "?.", loc);
            if (match('?')) return makeToken(TokenType::NullCoalesce, "??", loc);
            return makeToken(TokenType::QuestionMark, "?", loc);

        default:
            reportError(std::string("Unexpected character: '") + c + "'");
            return makeToken(TokenType::Invalid, std::string(1, c), loc);
    }
}

// --- Helpers ---

Token Lexer::makeToken(TokenType type, const std::string& value) {
    return Token(type, value, SourceLocation(filename_, tokenStartLine_, tokenStartColumn_));
}

Token Lexer::makeToken(TokenType type, const std::string& value, const SourceLocation& loc) {
    return Token(type, value, loc);
}

void Lexer::reportError(const std::string& message) {
    std::string error = filename_ + ":" + std::to_string(line_) + ":" +
                        std::to_string(column_) + ": GardLexError: " + message;
    errors_.push_back(error);
}

SourceLocation Lexer::currentLocation() const {
    return SourceLocation(filename_, line_, column_);
}

} // namespace gard
