#include "sema/symbol.h"

namespace gard {

SymbolTable::SymbolTable() {
    // Create global scope
    auto global = std::make_unique<Scope>(ScopeKind::Global, "global");
    current_ = global.get();
    scopes_.push_back(std::move(global));
}

void SymbolTable::pushScope(ScopeKind kind, const std::string& name) {
    auto scope = std::make_unique<Scope>(kind, name);
    scope->parent = current_;

    // Inherit context from parent
    if (current_) {
        scope->isAsync = current_->isAsync;
        scope->isClassScope = current_->isClassScope;
        scope->className = current_->className;
    }

    // Override context based on scope kind
    if (kind == ScopeKind::Class) {
        scope->isClassScope = true;
        scope->className = name;
    }

    current_ = scope.get();
    scopes_.push_back(std::move(scope));
}

void SymbolTable::popScope() {
    if (current_ && current_->parent) {
        current_ = current_->parent;
    }
}

Scope& SymbolTable::currentScope() {
    return *current_;
}

const Scope& SymbolTable::currentScope() const {
    return *current_;
}

bool SymbolTable::declare(const Symbol& symbol) {
    if (current_->hasSymbol(symbol.name)) {
        return false; // duplicate
    }
    current_->symbols[symbol.name] = symbol;
    return true;
}

Symbol* SymbolTable::resolve(const std::string& name) {
    Scope* scope = current_;
    while (scope) {
        Symbol* sym = scope->getSymbol(name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return nullptr;
}

Symbol* SymbolTable::resolveInCurrentScope(const std::string& name) {
    return current_->getSymbol(name);
}

Symbol* SymbolTable::resolveInClass(const std::string& className, const std::string& memberName) {
    // Search all scopes for the class scope
    for (auto& scope : scopes_) {
        if (scope->kind == ScopeKind::Class && scope->name == className) {
            return scope->getSymbol(memberName);
        }
    }
    return nullptr;
}

bool SymbolTable::isInsideLoop() const {
    const Scope* scope = current_;
    while (scope) {
        if (scope->kind == ScopeKind::Loop) return true;
        if (scope->kind == ScopeKind::Function) return false; // don't cross function boundary
        scope = scope->parent;
    }
    return false;
}

bool SymbolTable::isInsideSwitch() const {
    const Scope* scope = current_;
    while (scope) {
        if (scope->kind == ScopeKind::Switch) return true;
        if (scope->kind == ScopeKind::Function) return false;
        scope = scope->parent;
    }
    return false;
}

bool SymbolTable::isInsideAsyncFunction() const {
    const Scope* scope = current_;
    while (scope) {
        if (scope->kind == ScopeKind::Function && scope->isAsync) return true;
        scope = scope->parent;
    }
    return false;
}

bool SymbolTable::isInsideClass() const {
    const Scope* scope = current_;
    while (scope) {
        if (scope->kind == ScopeKind::Class) return true;
        scope = scope->parent;
    }
    return false;
}

std::string SymbolTable::currentClassName() const {
    const Scope* scope = current_;
    while (scope) {
        if (scope->kind == ScopeKind::Class) return scope->name;
        scope = scope->parent;
    }
    return "";
}

bool SymbolTable::isInsideFunction() const {
    const Scope* scope = current_;
    while (scope) {
        if (scope->kind == ScopeKind::Function) return true;
        scope = scope->parent;
    }
    return false;
}

} // namespace gard
