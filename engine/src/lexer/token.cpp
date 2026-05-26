#include "lexer/token.h"

namespace gard {

bool Token::isLiteral() const {
    switch (type) {
        case TokenType::IntLiteral:
        case TokenType::DoubleLiteral:
        case TokenType::FloatLiteral:
        case TokenType::LongLiteral:
        case TokenType::HexLiteral:
        case TokenType::BinaryLiteral:
        case TokenType::StringLiteral:
        case TokenType::TemplateLiteral:
        case TokenType::CharLiteral:
        case TokenType::BoolLiteral:
        case TokenType::NullLiteral:
            return true;
        default:
            return false;
    }
}

bool Token::isKeyword() const {
    switch (type) {
        case TokenType::Let:
        case TokenType::Var:
        case TokenType::Const:
        case TokenType::Readonly:
        case TokenType::If:
        case TokenType::Else:
        case TokenType::For:
        case TokenType::Foreach:
        case TokenType::While:
        case TokenType::Do:
        case TokenType::Switch:
        case TokenType::Case:
        case TokenType::Default:
        case TokenType::Match:
        case TokenType::Break:
        case TokenType::Continue:
        case TokenType::Return:
        case TokenType::Throw:
        case TokenType::Rethrow:
        case TokenType::Try:
        case TokenType::Catch:
        case TokenType::Finally:
        case TokenType::Function:
        case TokenType::Async:
        case TokenType::Await:
        case TokenType::Sync:
        case TokenType::Task:
        case TokenType::Future:
        case TokenType::Stream:
        case TokenType::Class:
        case TokenType::Abstract:
        case TokenType::Extends:
        case TokenType::Implements:
        case TokenType::New:
        case TokenType::This:
        case TokenType::Super:
        case TokenType::Public:
        case TokenType::Private:
        case TokenType::Protected:
        case TokenType::Static:
        case TokenType::Interface:
        case TokenType::Enum:
        case TokenType::Import:
        case TokenType::Export:
        case TokenType::From:
        case TokenType::Blockchain:
        case TokenType::Contract:
        case TokenType::Ledger:
        case TokenType::Validate:
        case TokenType::Mine:
        case TokenType::Sign:
        case TokenType::Block:
        case TokenType::Hash:
        case TokenType::Transaction:
        case TokenType::Lock:
        case TokenType::Unlock:
        case TokenType::Mutex:
        case TokenType::Semaphore:
        case TokenType::Wait:
        case TokenType::Signal:
        case TokenType::Barrier:
        case TokenType::Print:
        case TokenType::Null:
        case TokenType::Void:
        case TokenType::True:
        case TokenType::False:
        case TokenType::In:
        case TokenType::Of:
        case TokenType::Is:
        case TokenType::As:
        case TokenType::Typeof:
        case TokenType::Int:
        case TokenType::Double:
        case TokenType::Float:
        case TokenType::Long:
        case TokenType::Short:
        case TokenType::Boolean:
        case TokenType::String:
        case TokenType::Char:
        case TokenType::Uint:
        case TokenType::Bigint:
        case TokenType::Dynamic:
        case TokenType::MemoryType:
        case TokenType::Array:
        case TokenType::Map:
        case TokenType::Set:
            return true;
        default:
            return false;
    }
}

bool Token::isOperator() const {
    switch (type) {
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Assign:
        case TokenType::PlusAssign:
        case TokenType::MinusAssign:
        case TokenType::StarAssign:
        case TokenType::SlashAssign:
        case TokenType::Equal:
        case TokenType::NotEqual:
        case TokenType::Less:
        case TokenType::Greater:
        case TokenType::LessEqual:
        case TokenType::GreaterEqual:
        case TokenType::And:
        case TokenType::Or:
        case TokenType::Not:
        case TokenType::BitAnd:
        case TokenType::BitOr:
        case TokenType::BitXor:
        case TokenType::BitNot:
        case TokenType::ShiftLeft:
        case TokenType::ShiftRight:
        case TokenType::UnsignedShiftRight:
        case TokenType::Arrow:
        case TokenType::OptionalChain:
        case TokenType::NullCoalesce:
        case TokenType::Increment:
        case TokenType::Decrement:
            return true;
        default:
            return false;
    }
}

std::string Token::typeName() const {
    return tokenTypeToString(type);
}

const std::unordered_map<std::string, TokenType>& getKeywords() {
    static const std::unordered_map<std::string, TokenType> keywords = {
        // Variable declarations
        {"let", TokenType::Let},
        {"var", TokenType::Var},
        {"const", TokenType::Const},
        {"readonly", TokenType::Readonly},

        // Control flow
        {"if", TokenType::If},
        {"else", TokenType::Else},
        {"for", TokenType::For},
        {"foreach", TokenType::Foreach},
        {"while", TokenType::While},
        {"do", TokenType::Do},
        {"switch", TokenType::Switch},
        {"case", TokenType::Case},
        {"default", TokenType::Default},
        {"match", TokenType::Match},
        {"break", TokenType::Break},
        {"continue", TokenType::Continue},
        {"return", TokenType::Return},
        {"throw", TokenType::Throw},
        {"rethrow", TokenType::Rethrow},
        {"try", TokenType::Try},
        {"catch", TokenType::Catch},
        {"finally", TokenType::Finally},

        // Functions & Async
        {"function", TokenType::Function},
        {"async", TokenType::Async},
        {"await", TokenType::Await},
        {"sync", TokenType::Sync},
        {"task", TokenType::Task},
        {"future", TokenType::Future},
        {"stream", TokenType::Stream},

        // OOP
        {"class", TokenType::Class},
        {"abstract", TokenType::Abstract},
        {"extends", TokenType::Extends},
        {"implements", TokenType::Implements},
        {"new", TokenType::New},
        {"this", TokenType::This},
        {"super", TokenType::Super},
        {"public", TokenType::Public},
        {"private", TokenType::Private},
        {"protected", TokenType::Protected},
        {"static", TokenType::Static},
        {"interface", TokenType::Interface},
        {"enum", TokenType::Enum},

        // Modules
        {"import", TokenType::Import},
        {"export", TokenType::Export},
        {"from", TokenType::From},

        // Blockchain
        {"blockchain", TokenType::Blockchain},
        {"contract", TokenType::Contract},
        {"ledger", TokenType::Ledger},
        {"validate", TokenType::Validate},
        {"mine", TokenType::Mine},
        {"sign", TokenType::Sign},
        {"block", TokenType::Block},
        {"hash", TokenType::Hash},
        {"transaction", TokenType::Transaction},

        // Concurrency
        {"lock", TokenType::Lock},
        {"unlock", TokenType::Unlock},
        {"mutex", TokenType::Mutex},
        {"semaphore", TokenType::Semaphore},
        {"wait", TokenType::Wait},
        {"signal", TokenType::Signal},
        {"barrier", TokenType::Barrier},

        // Built-in
        {"print", TokenType::Print},
        {"null", TokenType::Null},
        {"void", TokenType::Void},
        {"true", TokenType::True},
        {"false", TokenType::False},
        {"in", TokenType::In},
        {"of", TokenType::Of},
        {"is", TokenType::Is},
        {"as", TokenType::As},
        {"typeof", TokenType::Typeof},

        // Type keywords
        {"int", TokenType::Int},
        {"double", TokenType::Double},
        {"float", TokenType::Float},
        {"long", TokenType::Long},
        {"short", TokenType::Short},
        {"boolean", TokenType::Boolean},
        {"string", TokenType::String},
        {"char", TokenType::Char},
        {"uint", TokenType::Uint},
        {"bigint", TokenType::Bigint},
        {"dynamic", TokenType::Dynamic},
        {"memory", TokenType::MemoryType},

        // Collection keywords
        {"array", TokenType::Array},
        {"map", TokenType::Map},
        {"set", TokenType::Set},
    };
    return keywords;
}

std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::IntLiteral: return "IntLiteral";
        case TokenType::DoubleLiteral: return "DoubleLiteral";
        case TokenType::FloatLiteral: return "FloatLiteral";
        case TokenType::LongLiteral: return "LongLiteral";
        case TokenType::HexLiteral: return "HexLiteral";
        case TokenType::BinaryLiteral: return "BinaryLiteral";
        case TokenType::StringLiteral: return "StringLiteral";
        case TokenType::TemplateLiteral: return "TemplateLiteral";
        case TokenType::CharLiteral: return "CharLiteral";
        case TokenType::BoolLiteral: return "BoolLiteral";
        case TokenType::NullLiteral: return "NullLiteral";
        case TokenType::Identifier: return "Identifier";
        case TokenType::Let: return "let";
        case TokenType::Var: return "var";
        case TokenType::Const: return "const";
        case TokenType::Readonly: return "readonly";
        case TokenType::If: return "if";
        case TokenType::Else: return "else";
        case TokenType::For: return "for";
        case TokenType::Foreach: return "foreach";
        case TokenType::While: return "while";
        case TokenType::Do: return "do";
        case TokenType::Switch: return "switch";
        case TokenType::Case: return "case";
        case TokenType::Default: return "default";
        case TokenType::Match: return "match";
        case TokenType::Break: return "break";
        case TokenType::Continue: return "continue";
        case TokenType::Return: return "return";
        case TokenType::Throw: return "throw";
        case TokenType::Rethrow: return "rethrow";
        case TokenType::Try: return "try";
        case TokenType::Catch: return "catch";
        case TokenType::Finally: return "finally";
        case TokenType::Function: return "function";
        case TokenType::Async: return "async";
        case TokenType::Await: return "await";
        case TokenType::Sync: return "sync";
        case TokenType::Task: return "task";
        case TokenType::Future: return "future";
        case TokenType::Stream: return "stream";
        case TokenType::Class: return "class";
        case TokenType::Abstract: return "abstract";
        case TokenType::Extends: return "extends";
        case TokenType::Implements: return "implements";
        case TokenType::New: return "new";
        case TokenType::This: return "this";
        case TokenType::Super: return "super";
        case TokenType::Public: return "public";
        case TokenType::Private: return "private";
        case TokenType::Protected: return "protected";
        case TokenType::Static: return "static";
        case TokenType::Interface: return "interface";
        case TokenType::Enum: return "enum";
        case TokenType::Import: return "import";
        case TokenType::Export: return "export";
        case TokenType::From: return "from";
        case TokenType::Blockchain: return "blockchain";
        case TokenType::Contract: return "contract";
        case TokenType::Ledger: return "ledger";
        case TokenType::Validate: return "validate";
        case TokenType::Mine: return "mine";
        case TokenType::Sign: return "sign";
        case TokenType::Block: return "block";
        case TokenType::Hash: return "hash";
        case TokenType::Transaction: return "transaction";
        case TokenType::Lock: return "lock";
        case TokenType::Unlock: return "unlock";
        case TokenType::Mutex: return "mutex";
        case TokenType::Semaphore: return "semaphore";
        case TokenType::Wait: return "wait";
        case TokenType::Signal: return "signal";
        case TokenType::Barrier: return "barrier";
        case TokenType::Print: return "print";
        case TokenType::Null: return "null";
        case TokenType::Void: return "void";
        case TokenType::True: return "true";
        case TokenType::False: return "false";
        case TokenType::In: return "in";
        case TokenType::Of: return "of";
        case TokenType::Is: return "is";
        case TokenType::As: return "as";
        case TokenType::Typeof: return "typeof";
        case TokenType::Int: return "int";
        case TokenType::Double: return "double";
        case TokenType::Float: return "float";
        case TokenType::Long: return "long";
        case TokenType::Short: return "short";
        case TokenType::Boolean: return "boolean";
        case TokenType::String: return "string";
        case TokenType::Char: return "char";
        case TokenType::Uint: return "uint";
        case TokenType::Bigint: return "bigint";
        case TokenType::Dynamic: return "dynamic";
        case TokenType::MemoryType: return "memory";
        case TokenType::Array: return "array";
        case TokenType::Map: return "map";
        case TokenType::Set: return "set";
        case TokenType::Plus: return "+";
        case TokenType::Minus: return "-";
        case TokenType::Star: return "*";
        case TokenType::Slash: return "/";
        case TokenType::Percent: return "%";
        case TokenType::Assign: return "=";
        case TokenType::PlusAssign: return "+=";
        case TokenType::MinusAssign: return "-=";
        case TokenType::StarAssign: return "*=";
        case TokenType::SlashAssign: return "/=";
        case TokenType::Equal: return "==";
        case TokenType::NotEqual: return "!=";
        case TokenType::Less: return "<";
        case TokenType::Greater: return ">";
        case TokenType::LessEqual: return "<=";
        case TokenType::GreaterEqual: return ">=";
        case TokenType::And: return "&&";
        case TokenType::Or: return "||";
        case TokenType::Not: return "!";
        case TokenType::BitAnd: return "&";
        case TokenType::BitOr: return "|";
        case TokenType::BitXor: return "^";
        case TokenType::BitNot: return "~";
        case TokenType::ShiftLeft: return "<<";
        case TokenType::ShiftRight: return ">>";
        case TokenType::UnsignedShiftRight: return ">>>";
        case TokenType::Arrow: return "=>";
        case TokenType::OptionalChain: return "?.";
        case TokenType::NullCoalesce: return "??";
        case TokenType::Increment: return "++";
        case TokenType::Decrement: return "--";
        case TokenType::LeftParen: return "(";
        case TokenType::RightParen: return ")";
        case TokenType::LeftBrace: return "{";
        case TokenType::RightBrace: return "}";
        case TokenType::LeftBracket: return "[";
        case TokenType::RightBracket: return "]";
        case TokenType::Semicolon: return ";";
        case TokenType::Colon: return ":";
        case TokenType::Comma: return ",";
        case TokenType::Dot: return ".";
        case TokenType::At: return "@";
        case TokenType::QuestionMark: return "?";
        case TokenType::DocComment: return "DocComment";
        case TokenType::EndOfFile: return "EOF";
        case TokenType::Invalid: return "Invalid";
    }
    return "Unknown";
}

} // namespace gard
