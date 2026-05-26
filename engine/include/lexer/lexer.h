#pragma once

#include "token.h"
#include <string>
#include <vector>

namespace gard {

// Character stream reader with lookahead
class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename = "<stdin>");

    // Tokenize the entire source
    std::vector<Token> tokenize();

    // Get next token (one at a time)
    Token nextToken();

    // Check if we've reached the end
    bool isAtEnd() const;

    // Get all errors encountered during lexing
    const std::vector<std::string>& getErrors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    // Character stream operations
    char current() const;
    char peek() const;
    char peekNext() const;
    char advance();
    bool match(char expected);
    bool isAtEndInternal() const;

    // Character classification
    bool isDigit(char c) const;
    bool isAlpha(char c) const;
    bool isAlphaNumeric(char c) const;
    bool isHexDigit(char c) const;
    bool isBinaryDigit(char c) const;

    // Token scanning
    Token scanToken();
    Token scanNumber();
    Token scanString();
    Token scanTemplateString();
    Token scanChar();
    Token scanIdentifierOrKeyword();
    Token scanOperatorOrPunctuation();

    // Comment handling
    void skipSingleLineComment();
    std::string scanDocCommentSingle();
    void skipMultiLineComment();
    std::string scanDocCommentMulti();

    // Whitespace
    void skipWhitespace();

    // Helpers
    Token makeToken(TokenType type, const std::string& value);
    Token makeToken(TokenType type, const std::string& value, const SourceLocation& loc);
    void reportError(const std::string& message);
    SourceLocation currentLocation() const;

    // State
    std::string source_;
    std::string filename_;
    int pos_;
    int line_;
    int column_;
    int tokenStartLine_;
    int tokenStartColumn_;
    std::vector<std::string> errors_;
};

} // namespace gard
