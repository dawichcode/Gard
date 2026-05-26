#pragma once

#include "lexer/token.h"
#include "parser/ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace gard {

// --- Symbol kinds ---

enum class SymbolKind {
    Variable,
    Function,
    Class,
    Interface,
    Parameter,
    Field,
    Method,
    Constructor,
    Module,
    BlockchainContract,
    GenericParam,
};

// --- Symbol ---

struct Symbol {
    SymbolKind kind;
    std::string name;
    SourceLocation location;

    // Type info (as string for now, will be replaced by proper Type* in 1.4)
    std::string typeName;
    bool isConst = false;
    bool isReadonly = false;
    bool isMutable = true;

    // Access
    std::string accessModifier; // "public", "private", "protected", ""
    bool isStatic = false;
    bool isAsync = false;
    bool isAbstract = false;

    // Usage tracking
    bool isDefined = false;
    bool isUsed = false;
    bool isImported = false;

    // For functions/methods
    std::vector<std::string> paramTypes;
    std::string returnType;

    // For classes
    std::string baseClass;
    std::vector<std::string> interfaces;
    std::vector<std::string> genericParams;
    // Generic param constraints: genericParamConstraints[i] = list of bound names for genericParams[i]
    std::vector<std::vector<std::string>> genericParamConstraints;

    Symbol() : kind(SymbolKind::Variable) {}
    Symbol(SymbolKind kind, const std::string& name, const SourceLocation& loc)
        : kind(kind), name(name), location(loc) {}
};

// --- Scope ---

enum class ScopeKind {
    Global,
    Module,
    Class,
    Function,
    Block,
    Loop,       // for break/continue validation
    Switch,     // for break validation
};

struct Scope {
    ScopeKind kind;
    std::string name; // e.g. class name, function name
    Scope* parent = nullptr;
    std::unordered_map<std::string, Symbol> symbols;

    // Context flags
    bool isAsync = false;       // inside async function?
    bool isClassScope = false;  // inside a class?
    std::string className;      // current class name (for 'this' validation)

    Scope(ScopeKind kind, const std::string& name = "")
        : kind(kind), name(name) {}

    bool hasSymbol(const std::string& name) const {
        return symbols.find(name) != symbols.end();
    }

    Symbol* getSymbol(const std::string& name) {
        auto it = symbols.find(name);
        if (it != symbols.end()) return &it->second;
        return nullptr;
    }

    const Symbol* getSymbol(const std::string& name) const {
        auto it = symbols.find(name);
        if (it != symbols.end()) return &it->second;
        return nullptr;
    }
};

// --- Symbol Table (hierarchical scopes) ---

class SymbolTable {
public:
    SymbolTable();

    // Scope management
    void pushScope(ScopeKind kind, const std::string& name = "");
    void popScope();
    Scope& currentScope();
    const Scope& currentScope() const;

    // Symbol operations
    bool declare(const Symbol& symbol);
    Symbol* resolve(const std::string& name);
    Symbol* resolveInCurrentScope(const std::string& name);
    Symbol* resolveInClass(const std::string& className, const std::string& memberName);

    // Context queries
    bool isInsideLoop() const;
    bool isInsideSwitch() const;
    bool isInsideAsyncFunction() const;
    bool isInsideClass() const;
    std::string currentClassName() const;
    bool isInsideFunction() const;

    // Get all scopes (for debugging/analysis)
    const std::vector<std::unique_ptr<Scope>>& getScopes() const { return scopes_; }

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    Scope* current_ = nullptr;
};

} // namespace gard
