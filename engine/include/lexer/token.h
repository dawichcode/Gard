#pragma once

#include <string>
#include <unordered_map>

namespace gard {

// Source location tracking (file, line, column)
struct SourceLocation {
    std::string file;
    int line;
    int column;

    SourceLocation() : file(""), line(1), column(1) {}
    SourceLocation(const std::string& file, int line, int col)
        : file(file), line(line), column(col) {}

    std::string toString() const {
        return file + ":" + std::to_string(line) + ":" + std::to_string(column);
    }
};

// Token types enum for all Gard tokens
enum class TokenType {
    // Literals
    IntLiteral,
    DoubleLiteral,
    FloatLiteral,
    LongLiteral,
    HexLiteral,
    BinaryLiteral,
    StringLiteral,
    TemplateLiteral,
    CharLiteral,
    BoolLiteral,
    NullLiteral,

    // Identifier
    Identifier,

    // Keywords - Variable declarations
    Let,
    Var,
    Const,
    Readonly,

    // Keywords - Control flow
    If,
    Else,
    For,
    Foreach,
    While,
    Do,
    Switch,
    Case,
    Default,
    Match,
    Break,
    Continue,
    Return,
    Throw,
    Rethrow,
    Try,
    Catch,
    Finally,

    // Keywords - Functions & Async
    Function,
    Async,
    Await,
    Sync,
    Task,
    Future,
    Stream,

    // Keywords - OOP
    Class,
    Abstract,
    Extends,
    Implements,
    New,
    This,
    Super,
    Public,
    Private,
    Protected,
    Static,
    Interface,
    Enum,

    // Keywords - Modules
    Import,
    Export,
    From,

    // Keywords - Blockchain
    Blockchain,
    Contract,
    Ledger,
    Validate,
    Mine,
    Sign,
    Block,
    Hash,
    Transaction,

    // Keywords - Concurrency
    Lock,
    Unlock,
    Mutex,
    Semaphore,
    Wait,
    Signal,
    Barrier,

    // Keywords - Built-in
    Print,
    Null,
    Void,
    True,
    False,
    In,
    Of,
    Is,
    As,
    Typeof,

    // Type keywords
    Int,
    Double,
    Float,
    Long,
    Short,
    Boolean,
    String,
    Char,
    Uint,
    Bigint,
    Dynamic,
    MemoryType,     // memory (pointer type)

    // Collection keywords
    Array,
    Map,
    Set,

    // Operators - Arithmetic
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Percent,        // %

    // Operators - Assignment
    Assign,         // =
    PlusAssign,     // +=
    MinusAssign,    // -=
    StarAssign,     // *=
    SlashAssign,    // /=

    // Operators - Comparison
    Equal,          // ==
    NotEqual,       // !=
    Less,           // <
    Greater,        // >
    LessEqual,      // <=
    GreaterEqual,   // >=

    // Operators - Logical
    And,            // &&
    Or,             // ||
    Not,            // !

    // Operators - Bitwise
    BitAnd,         // &
    BitOr,          // |
    BitXor,         // ^
    BitNot,         // ~
    ShiftLeft,      // <<
    ShiftRight,     // >>
    UnsignedShiftRight, // >>>

    // Operators - Special
    Arrow,          // =>
    OptionalChain,  // ?.
    NullCoalesce,   // ??
    Increment,      // ++
    Decrement,      // --

    // Punctuation
    LeftParen,      // (
    RightParen,     // )
    LeftBrace,      // {
    RightBrace,     // }
    LeftBracket,    // [
    RightBracket,   // ]
    Semicolon,      // ;
    Colon,          // :
    Comma,          // ,
    Dot,            // .
    Spread,         // ...
    At,             // @
    QuestionMark,   // ?

    // Comments (preserved for doc comments)
    DocComment,

    // Special
    EndOfFile,
    Invalid,
};

// Token structure
struct Token {
    TokenType type;
    std::string value;
    SourceLocation location;

    Token() : type(TokenType::Invalid), value(""), location() {}
    Token(TokenType type, const std::string& value, const SourceLocation& loc)
        : type(type), value(value), location(loc) {}

    bool is(TokenType t) const { return type == t; }
    bool isNot(TokenType t) const { return type != t; }
    bool isLiteral() const;
    bool isKeyword() const;
    bool isOperator() const;

    std::string typeName() const;
};

// Keyword lookup table
const std::unordered_map<std::string, TokenType>& getKeywords();

// Token type to string conversion
std::string tokenTypeToString(TokenType type);

} // namespace gard
