#pragma once

#include "lexer/token.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace gard {

// Forward declarations
struct Expression;
struct Statement;
struct TypeAnnotation;

using ExprPtr = std::unique_ptr<Expression>;
using StmtPtr = std::unique_ptr<Statement>;
using TypePtr = std::unique_ptr<TypeAnnotation>;

// --- Annotation (metadata on classes, methods, fields) ---
struct AnnotationArg {
    std::string key;   // argument name (e.g., "path", "validate")
    std::string value; // argument value as string (e.g., "/api", "true")
};

struct Annotation {
    std::string name;                    // annotation name (e.g., "Route", "Test")
    std::vector<AnnotationArg> args;     // key-value arguments

    Annotation() = default;
    Annotation(const std::string& n) : name(n) {}
    Annotation(const std::string& n, std::vector<AnnotationArg> a) : name(n), args(std::move(a)) {}

    bool hasArg(const std::string& key) const {
        for (auto& a : args) if (a.key == key) return true;
        return false;
    }
    std::string getArg(const std::string& key, const std::string& defaultVal = "") const {
        for (auto& a : args) if (a.key == key) return a.value;
        return defaultVal;
    }
};

// --- Type Annotations ---

enum class TypeKind {
    Named,      // int, string, MyClass
    Generic,    // array<int>, map<string, int>
    Nullable,   // string?
    Tuple,      // (int, string)
    Function,   // (int, int) => int
};

struct TypeAnnotation {
    TypeKind kind;
    SourceLocation location;

    virtual ~TypeAnnotation() = default;

protected:
    TypeAnnotation(TypeKind kind, const SourceLocation& loc)
        : kind(kind), location(loc) {}
};

struct NamedType : TypeAnnotation {
    std::string name;

    NamedType(const std::string& name, const SourceLocation& loc)
        : TypeAnnotation(TypeKind::Named, loc), name(name) {}
};

struct GenericType : TypeAnnotation {
    std::string name;
    std::vector<TypePtr> typeArgs;

    GenericType(const std::string& name, std::vector<TypePtr> args, const SourceLocation& loc)
        : TypeAnnotation(TypeKind::Generic, loc), name(name), typeArgs(std::move(args)) {}
};

struct NullableType : TypeAnnotation {
    TypePtr inner;

    NullableType(TypePtr inner, const SourceLocation& loc)
        : TypeAnnotation(TypeKind::Nullable, loc), inner(std::move(inner)) {}
};

struct TupleType : TypeAnnotation {
    std::vector<TypePtr> elements;

    TupleType(std::vector<TypePtr> elements, const SourceLocation& loc)
        : TypeAnnotation(TypeKind::Tuple, loc), elements(std::move(elements)) {}
};

struct FunctionType : TypeAnnotation {
    std::vector<TypePtr> paramTypes;
    TypePtr returnType;

    FunctionType(std::vector<TypePtr> params, TypePtr ret, const SourceLocation& loc)
        : TypeAnnotation(TypeKind::Function, loc),
          paramTypes(std::move(params)), returnType(std::move(ret)) {}
};

// --- Generic Type Parameter ---

enum class Variance {
    Invariant,   // default: T
    Covariant,   // out T (produces T)
    Contravariant // in T (consumes T)
};

struct GenericParam {
    std::string name;
    TypePtr constraint; // optional: <T: Comparable>
    std::vector<TypePtr> constraints; // multiple bounds: <T: A & B>
    Variance variance = Variance::Invariant; // in/out variance
    bool reified = false; // reified T — type info available at runtime
    SourceLocation location;
};

// --- Expressions ---

enum class ExprKind {
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

    // Primary
    Identifier,
    This,
    Super,

    // Compound
    Binary,
    Unary,
    Ternary,
    Assignment,
    Call,
    MemberAccess,
    OptionalChain,
    IndexAccess,
    New,
    Cast,
    Is,
    Typeof,
    Lambda,
    Grouped,
    Await,
    Array,
    Map,
    Set,
    Increment,
    Decrement,
};

struct Expression {
    ExprKind kind;
    SourceLocation location;

    virtual ~Expression() = default;

protected:
    Expression(ExprKind kind, const SourceLocation& loc)
        : kind(kind), location(loc) {}
};

// Literal expressions
struct IntLiteralExpr : Expression {
    std::string value;
    IntLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::IntLiteral, loc), value(val) {}
};

struct DoubleLiteralExpr : Expression {
    std::string value;
    DoubleLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::DoubleLiteral, loc), value(val) {}
};

struct FloatLiteralExpr : Expression {
    std::string value;
    FloatLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::FloatLiteral, loc), value(val) {}
};

struct LongLiteralExpr : Expression {
    std::string value;
    LongLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::LongLiteral, loc), value(val) {}
};

struct HexLiteralExpr : Expression {
    std::string value;
    HexLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::HexLiteral, loc), value(val) {}
};

struct BinaryLiteralExpr : Expression {
    std::string value;
    BinaryLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::BinaryLiteral, loc), value(val) {}
};

struct StringLiteralExpr : Expression {
    std::string value;
    StringLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::StringLiteral, loc), value(val) {}
};

struct TemplateLiteralExpr : Expression {
    std::string raw; // raw template content with ${} markers
    TemplateLiteralExpr(const std::string& raw, const SourceLocation& loc)
        : Expression(ExprKind::TemplateLiteral, loc), raw(raw) {}
};

struct CharLiteralExpr : Expression {
    std::string value;
    CharLiteralExpr(const std::string& val, const SourceLocation& loc)
        : Expression(ExprKind::CharLiteral, loc), value(val) {}
};

struct BoolLiteralExpr : Expression {
    bool value;
    BoolLiteralExpr(bool val, const SourceLocation& loc)
        : Expression(ExprKind::BoolLiteral, loc), value(val) {}
};

struct NullLiteralExpr : Expression {
    NullLiteralExpr(const SourceLocation& loc)
        : Expression(ExprKind::NullLiteral, loc) {}
};

// Primary expressions
struct IdentifierExpr : Expression {
    std::string name;
    IdentifierExpr(const std::string& name, const SourceLocation& loc)
        : Expression(ExprKind::Identifier, loc), name(name) {}
};

struct ThisExpr : Expression {
    ThisExpr(const SourceLocation& loc) : Expression(ExprKind::This, loc) {}
};

struct SuperExpr : Expression {
    SuperExpr(const SourceLocation& loc) : Expression(ExprKind::Super, loc) {}
};

// Compound expressions
struct BinaryExpr : Expression {
    ExprPtr left;
    TokenType op;
    ExprPtr right;

    BinaryExpr(ExprPtr left, TokenType op, ExprPtr right, const SourceLocation& loc)
        : Expression(ExprKind::Binary, loc),
          left(std::move(left)), op(op), right(std::move(right)) {}
};

struct UnaryExpr : Expression {
    TokenType op;
    ExprPtr operand;
    bool prefix; // true for prefix, false for postfix

    UnaryExpr(TokenType op, ExprPtr operand, bool prefix, const SourceLocation& loc)
        : Expression(ExprKind::Unary, loc),
          op(op), operand(std::move(operand)), prefix(prefix) {}
};

struct TernaryExpr : Expression {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;

    TernaryExpr(ExprPtr cond, ExprPtr then, ExprPtr els, const SourceLocation& loc)
        : Expression(ExprKind::Ternary, loc),
          condition(std::move(cond)), thenExpr(std::move(then)), elseExpr(std::move(els)) {}
};

struct CastExpr : Expression {
    ExprPtr expression;
    TypePtr targetType;

    CastExpr(ExprPtr expr, TypePtr type, const SourceLocation& loc)
        : Expression(ExprKind::Cast, loc),
          expression(std::move(expr)), targetType(std::move(type)) {}
};

struct AssignmentExpr : Expression {
    ExprPtr target;
    TokenType op; // =, +=, -=, *=, /=
    ExprPtr value;

    AssignmentExpr(ExprPtr target, TokenType op, ExprPtr value, const SourceLocation& loc)
        : Expression(ExprKind::Assignment, loc),
          target(std::move(target)), op(op), value(std::move(value)) {}
};

struct CallExpr : Expression {
    ExprPtr callee;
    std::vector<ExprPtr> arguments;

    CallExpr(ExprPtr callee, std::vector<ExprPtr> args, const SourceLocation& loc)
        : Expression(ExprKind::Call, loc),
          callee(std::move(callee)), arguments(std::move(args)) {}
};

struct MemberAccessExpr : Expression {
    ExprPtr object;
    std::string member;

    MemberAccessExpr(ExprPtr obj, const std::string& member, const SourceLocation& loc)
        : Expression(ExprKind::MemberAccess, loc),
          object(std::move(obj)), member(member) {}
};

struct OptionalChainExpr : Expression {
    ExprPtr object;
    std::string member;

    OptionalChainExpr(ExprPtr obj, const std::string& member, const SourceLocation& loc)
        : Expression(ExprKind::OptionalChain, loc),
          object(std::move(obj)), member(member) {}
};

struct IndexAccessExpr : Expression {
    ExprPtr object;
    ExprPtr index;

    IndexAccessExpr(ExprPtr obj, ExprPtr index, const SourceLocation& loc)
        : Expression(ExprKind::IndexAccess, loc),
          object(std::move(obj)), index(std::move(index)) {}
};

struct NewExpr : Expression {
    std::string className;
    std::vector<TypePtr> typeArgs;
    std::vector<ExprPtr> arguments;

    NewExpr(const std::string& cls, std::vector<TypePtr> typeArgs,
            std::vector<ExprPtr> args, const SourceLocation& loc)
        : Expression(ExprKind::New, loc),
          className(cls), typeArgs(std::move(typeArgs)), arguments(std::move(args)) {}
};

struct AwaitExpr : Expression {
    ExprPtr operand;

    AwaitExpr(ExprPtr operand, const SourceLocation& loc)
        : Expression(ExprKind::Await, loc), operand(std::move(operand)) {}
};

struct ArrayExpr : Expression {
    std::vector<ExprPtr> elements;

    ArrayExpr(std::vector<ExprPtr> elements, const SourceLocation& loc)
        : Expression(ExprKind::Array, loc), elements(std::move(elements)) {}
};

struct MapExpr : Expression {
    std::vector<std::pair<ExprPtr, ExprPtr>> entries;

    MapExpr(std::vector<std::pair<ExprPtr, ExprPtr>> entries, const SourceLocation& loc)
        : Expression(ExprKind::Map, loc), entries(std::move(entries)) {}
};

struct SetExpr : Expression {
    std::vector<ExprPtr> elements;

    SetExpr(std::vector<ExprPtr> elements, const SourceLocation& loc)
        : Expression(ExprKind::Set, loc), elements(std::move(elements)) {}
};

// Lambda parameter
struct LambdaParam {
    std::string name;
    TypePtr type; // optional
    SourceLocation location;
};

struct LambdaExpr : Expression {
    std::vector<LambdaParam> params;
    TypePtr returnType; // optional
    // Body is either a single expression or a block
    ExprPtr bodyExpr;       // for (x) => x + 1
    std::vector<StmtPtr> bodyBlock; // for (x) => { ... }
    bool hasBlockBody;

    LambdaExpr(std::vector<LambdaParam> params, TypePtr retType,
               ExprPtr body, const SourceLocation& loc)
        : Expression(ExprKind::Lambda, loc),
          params(std::move(params)), returnType(std::move(retType)),
          bodyExpr(std::move(body)), hasBlockBody(false) {}

    LambdaExpr(std::vector<LambdaParam> params, TypePtr retType,
               std::vector<StmtPtr> body, const SourceLocation& loc)
        : Expression(ExprKind::Lambda, loc),
          params(std::move(params)), returnType(std::move(retType)),
          bodyBlock(std::move(body)), hasBlockBody(true) {}
};

struct GroupedExpr : Expression {
    ExprPtr inner;

    GroupedExpr(ExprPtr inner, const SourceLocation& loc)
        : Expression(ExprKind::Grouped, loc), inner(std::move(inner)) {}
};

// --- Statements ---

enum class StmtKind {
    Expression,
    Block,
    VarDeclaration,
    FunctionDeclaration,
    Return,
    If,
    For,
    ForEach,
    While,
    DoWhile,
    Switch,
    Match,
    Break,
    Continue,
    Throw,
    TryCatch,
    Class,
    Interface,
    Enum,
    Import,
    Export,
    BlockchainContract,
    Print,
};

struct Statement {
    StmtKind kind;
    SourceLocation location;

    virtual ~Statement() = default;

protected:
    Statement(StmtKind kind, const SourceLocation& loc)
        : kind(kind), location(loc) {}
};

// Expression statement
struct ExpressionStmt : Statement {
    ExprPtr expression;

    ExpressionStmt(ExprPtr expr, const SourceLocation& loc)
        : Statement(StmtKind::Expression, loc), expression(std::move(expr)) {}
};

// Block statement
struct BlockStmt : Statement {
    std::vector<StmtPtr> statements;

    BlockStmt(std::vector<StmtPtr> stmts, const SourceLocation& loc)
        : Statement(StmtKind::Block, loc), statements(std::move(stmts)) {}
};

// Variable declaration
enum class VarDeclKind { Let, Var, Const, Readonly };

struct VarDeclarationStmt : Statement {
    VarDeclKind declKind;
    std::string name;
    TypePtr type;       // optional type annotation
    ExprPtr initializer; // optional

    VarDeclarationStmt(VarDeclKind kind, const std::string& name,
                       TypePtr type, ExprPtr init, const SourceLocation& loc)
        : Statement(StmtKind::VarDeclaration, loc),
          declKind(kind), name(name), type(std::move(type)), initializer(std::move(init)) {}
};

// Function parameter
struct FunctionParam {
    std::string name;
    TypePtr type;
    ExprPtr defaultValue; // optional
    bool isRest = false;  // ...args (variadic/rest parameter)
    SourceLocation location;
};

// Function declaration
struct FunctionDeclStmt : Statement {
    std::string name;
    std::vector<GenericParam> genericParams;
    std::vector<FunctionParam> params;
    TypePtr returnType;
    std::vector<StmtPtr> body;
    bool isAsync;
    bool isStatic;
    std::string accessModifier; // "public", "private", "protected", ""

    FunctionDeclStmt(const std::string& name, const SourceLocation& loc)
        : Statement(StmtKind::FunctionDeclaration, loc),
          name(name), isAsync(false), isStatic(false) {}
};

// Return statement
struct ReturnStmt : Statement {
    ExprPtr value; // optional

    ReturnStmt(ExprPtr value, const SourceLocation& loc)
        : Statement(StmtKind::Return, loc), value(std::move(value)) {}
};

// If statement
struct IfStmt : Statement {
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch; // optional

    IfStmt(ExprPtr cond, StmtPtr then, StmtPtr els, const SourceLocation& loc)
        : Statement(StmtKind::If, loc),
          condition(std::move(cond)), thenBranch(std::move(then)), elseBranch(std::move(els)) {}
};

// For statement
struct ForStmt : Statement {
    StmtPtr initializer; // optional (var decl or expr stmt)
    ExprPtr condition;   // optional
    ExprPtr increment;   // optional
    StmtPtr body;

    ForStmt(StmtPtr init, ExprPtr cond, ExprPtr inc, StmtPtr body, const SourceLocation& loc)
        : Statement(StmtKind::For, loc),
          initializer(std::move(init)), condition(std::move(cond)),
          increment(std::move(inc)), body(std::move(body)) {}
};

// ForEach statement
struct ForEachStmt : Statement {
    std::string variable;
    TypePtr varType; // optional
    ExprPtr iterable;
    StmtPtr body;
    bool isAwait; // for await (let x of stream)

    ForEachStmt(const std::string& var, TypePtr type, ExprPtr iter,
                StmtPtr body, bool isAwait, const SourceLocation& loc)
        : Statement(StmtKind::ForEach, loc),
          variable(var), varType(std::move(type)), iterable(std::move(iter)),
          body(std::move(body)), isAwait(isAwait) {}
};

// While statement
struct WhileStmt : Statement {
    ExprPtr condition;
    StmtPtr body;

    WhileStmt(ExprPtr cond, StmtPtr body, const SourceLocation& loc)
        : Statement(StmtKind::While, loc),
          condition(std::move(cond)), body(std::move(body)) {}
};

// Do-While statement
struct DoWhileStmt : Statement {
    StmtPtr body;
    ExprPtr condition;

    DoWhileStmt(StmtPtr body, ExprPtr cond, const SourceLocation& loc)
        : Statement(StmtKind::DoWhile, loc),
          body(std::move(body)), condition(std::move(cond)) {}
};

// Switch case
struct SwitchCase {
    ExprPtr value; // nullptr for default
    std::vector<StmtPtr> body;
    bool isDefault;
};

struct SwitchStmt : Statement {
    ExprPtr discriminant;
    std::vector<SwitchCase> cases;

    SwitchStmt(ExprPtr disc, std::vector<SwitchCase> cases, const SourceLocation& loc)
        : Statement(StmtKind::Switch, loc),
          discriminant(std::move(disc)), cases(std::move(cases)) {}
};

// Match arm
struct MatchArm {
    ExprPtr pattern;  // pattern expression (literal, identifier '_', etc.)
    ExprPtr body;     // expression body
    bool isDefault = false; // true for _ or default wildcard
};

struct MatchStmt : Statement {
    ExprPtr value;
    std::vector<MatchArm> arms;

    MatchStmt(ExprPtr val, std::vector<MatchArm> arms, const SourceLocation& loc)
        : Statement(StmtKind::Match, loc),
          value(std::move(val)), arms(std::move(arms)) {}
};

// Break/Continue
struct BreakStmt : Statement {
    BreakStmt(const SourceLocation& loc) : Statement(StmtKind::Break, loc) {}
};

struct ContinueStmt : Statement {
    ContinueStmt(const SourceLocation& loc) : Statement(StmtKind::Continue, loc) {}
};

// Throw
struct ThrowStmt : Statement {
    ExprPtr value;

    ThrowStmt(ExprPtr value, const SourceLocation& loc)
        : Statement(StmtKind::Throw, loc), value(std::move(value)) {}
};

// Try/Catch/Finally
struct CatchClause {
    std::string paramName;
    TypePtr paramType; // optional
    std::vector<StmtPtr> body;
    SourceLocation location;
};

struct TryCatchStmt : Statement {
    std::vector<StmtPtr> tryBody;
    std::vector<CatchClause> catchClauses;
    std::vector<StmtPtr> finallyBody; // optional

    TryCatchStmt(std::vector<StmtPtr> tryBody, std::vector<CatchClause> catches,
                 std::vector<StmtPtr> finallyBody, const SourceLocation& loc)
        : Statement(StmtKind::TryCatch, loc),
          tryBody(std::move(tryBody)), catchClauses(std::move(catches)),
          finallyBody(std::move(finallyBody)) {}
};

// Class field
struct ClassField {
    std::string accessModifier; // "public", "private", "protected"
    bool isStatic;
    VarDeclKind declKind;
    std::string name;
    TypePtr type;
    ExprPtr initializer;
    std::vector<Annotation> annotations;
    SourceLocation location;
};

// Class method (reuses FunctionDeclStmt)
struct ClassMethod {
    std::string accessModifier;
    bool isStatic;
    bool isAsync;
    bool isAbstract;
    std::string name;
    std::vector<GenericParam> genericParams;
    std::vector<FunctionParam> params;
    TypePtr returnType;
    std::vector<StmtPtr> body; // empty if abstract
    std::vector<Annotation> annotations;
    SourceLocation location;
};

// Constructor
struct Constructor {
    std::string accessModifier;
    std::vector<FunctionParam> params;
    std::vector<StmtPtr> body;
    SourceLocation location;
};

// Class declaration
struct ClassDeclStmt : Statement {
    std::string name;
    bool isAbstract;
    std::vector<GenericParam> genericParams;
    std::string baseClass;                    // extends
    std::vector<std::string> interfaces;      // implements
    std::vector<ClassField> fields;
    std::vector<ClassMethod> methods;
    std::optional<Constructor> constructor;
    std::vector<Annotation> annotations;

    ClassDeclStmt(const std::string& name, const SourceLocation& loc)
        : Statement(StmtKind::Class, loc), name(name), isAbstract(false) {}
};

// Interface declaration
struct InterfaceMethod {
    std::string name;
    std::vector<GenericParam> genericParams;
    std::vector<FunctionParam> params;
    TypePtr returnType;
    SourceLocation location;
};

struct InterfaceDeclStmt : Statement {
    std::string name;
    std::vector<GenericParam> genericParams;
    std::vector<std::string> extends; // interfaces can extend other interfaces
    std::vector<InterfaceMethod> methods;

    InterfaceDeclStmt(const std::string& name, const SourceLocation& loc)
        : Statement(StmtKind::Interface, loc), name(name) {}
};

// Enum variant (Java-style)
struct EnumVariant {
    std::string name;
    std::vector<ExprPtr> args; // constructor args for variant: RED(255, 0, 0)
    SourceLocation location;
};

// Enum declaration: enum Color { RED, GREEN, BLUE }
// Java-style: enum Planet { MERCURY(3.303e+23, 2.4397e6), ... }
struct EnumDeclStmt : Statement {
    std::string name;
    std::vector<EnumVariant> variants;
    std::vector<ClassField> fields;       // enum can have fields
    std::vector<ClassMethod> methods;     // enum can have methods
    std::optional<Constructor> constructor; // enum constructor for variant args
    std::vector<Annotation> annotations;

    EnumDeclStmt(const std::string& name, const SourceLocation& loc)
        : Statement(StmtKind::Enum, loc), name(name) {}
};

// Import statement
struct ImportStmt : Statement {
    std::vector<std::string> names; // imported symbols
    std::string path;               // module path

    ImportStmt(std::vector<std::string> names, const std::string& path, const SourceLocation& loc)
        : Statement(StmtKind::Import, loc), names(std::move(names)), path(path) {}
};

// Export statement
struct ExportStmt : Statement {
    StmtPtr declaration; // the exported declaration

    ExportStmt(StmtPtr decl, const SourceLocation& loc)
        : Statement(StmtKind::Export, loc), declaration(std::move(decl)) {}
};

// Blockchain contract
struct BlockchainContractStmt : Statement {
    std::string name;
    std::vector<ClassField> fields;
    std::vector<ClassMethod> methods;
    std::optional<Constructor> constructor;

    BlockchainContractStmt(const std::string& name, const SourceLocation& loc)
        : Statement(StmtKind::BlockchainContract, loc), name(name) {}
};

// Print statement (built-in)
struct PrintStmt : Statement {
    ExprPtr argument;

    PrintStmt(ExprPtr arg, const SourceLocation& loc)
        : Statement(StmtKind::Print, loc), argument(std::move(arg)) {}
};

// --- Program (top-level) ---

struct Program {
    std::vector<StmtPtr> statements;
    std::string filename;
};

} // namespace gard
