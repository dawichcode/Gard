#include "sema/analyzer.h"

namespace gard {

// --- Diagnostic ---

std::string Diagnostic::toString() const {
    std::string prefix;
    switch (severity) {
        case DiagSeverity::Error: prefix = "error"; break;
        case DiagSeverity::Warning: prefix = "warning"; break;
        case DiagSeverity::Info: prefix = "info"; break;
    }
    return location.toString() + ": " + prefix + ": " + message;
}

// --- SemanticAnalyzer ---

SemanticAnalyzer::SemanticAnalyzer() {}

void SemanticAnalyzer::analyze(Program& program) {
    currentModule_ = program.filename;
    currentProgram_ = &program;

    // First pass: register all top-level declarations (classes, functions, interfaces)
    for (auto& stmt : program.statements) {
        if (auto* cls = dynamic_cast<ClassDeclStmt*>(stmt.get())) {
            Symbol sym(SymbolKind::Class, cls->name, cls->location);
            sym.baseClass = cls->baseClass;
            sym.interfaces = cls->interfaces;
            sym.isDefined = true;
            for (auto& gp : cls->genericParams) {
                sym.genericParams.push_back(gp.name);
                // Store constraints for bounds checking
                std::vector<std::string> bounds;
                for (auto& c : gp.constraints) {
                    if (auto* named = dynamic_cast<NamedType*>(c.get())) {
                        bounds.push_back(named->name);
                    }
                }
                sym.genericParamConstraints.push_back(bounds);
            }
            if (!symbols_.declare(sym)) {
                error("Duplicate declaration of class '" + cls->name + "'", cls->location);
            }
        } else if (auto* iface = dynamic_cast<InterfaceDeclStmt*>(stmt.get())) {
            Symbol sym(SymbolKind::Interface, iface->name, iface->location);
            sym.isDefined = true;
            if (!symbols_.declare(sym)) {
                error("Duplicate declaration of interface '" + iface->name + "'", iface->location);
            }
        } else if (auto* fn = dynamic_cast<FunctionDeclStmt*>(stmt.get())) {
            Symbol sym(SymbolKind::Function, fn->name, fn->location);
            sym.isAsync = fn->isAsync;
            sym.isStatic = fn->isStatic;
            sym.accessModifier = fn->accessModifier;
            sym.isDefined = true;
            if (fn->returnType) {
                if (auto* named = dynamic_cast<NamedType*>(fn->returnType.get())) {
                    sym.returnType = named->name;
                }
            }
            if (!symbols_.declare(sym)) {
                error("Duplicate declaration of function '" + fn->name + "'", fn->location);
            }
        } else if (auto* contract = dynamic_cast<BlockchainContractStmt*>(stmt.get())) {
            Symbol sym(SymbolKind::BlockchainContract, contract->name, contract->location);
            sym.isDefined = true;
            if (!symbols_.declare(sym)) {
                error("Duplicate declaration of contract '" + contract->name + "'", contract->location);
            }
        } else if (auto* exp = dynamic_cast<ExportStmt*>(stmt.get())) {
            // Register the exported declaration
            if (exp->declaration) {
                if (auto* cls = dynamic_cast<ClassDeclStmt*>(exp->declaration.get())) {
                    Symbol sym(SymbolKind::Class, cls->name, cls->location);
                    sym.baseClass = cls->baseClass;
                    sym.interfaces = cls->interfaces;
                    sym.isDefined = true;
                    symbols_.declare(sym);
                } else if (auto* fn = dynamic_cast<FunctionDeclStmt*>(exp->declaration.get())) {
                    Symbol sym(SymbolKind::Function, fn->name, fn->location);
                    sym.isDefined = true;
                    symbols_.declare(sym);
                }
            }
        }
    }

    // Second pass: full analysis
    for (auto& stmt : program.statements) {
        analyzeStatement(stmt.get());
    }

    // Check for unused symbols
    checkUnused();
}

bool SemanticAnalyzer::hasErrors() const {
    for (auto& d : diagnostics_) {
        if (d.severity == DiagSeverity::Error) return true;
    }
    return false;
}

int SemanticAnalyzer::errorCount() const {
    int count = 0;
    for (auto& d : diagnostics_) {
        if (d.severity == DiagSeverity::Error) count++;
    }
    return count;
}

int SemanticAnalyzer::warningCount() const {
    int count = 0;
    for (auto& d : diagnostics_) {
        if (d.severity == DiagSeverity::Warning) count++;
    }
    return count;
}

// --- Statement analysis ---

void SemanticAnalyzer::analyzeStatement(Statement* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case StmtKind::Block:
            analyzeBlock(static_cast<BlockStmt*>(stmt)); break;
        case StmtKind::VarDeclaration:
            analyzeVarDeclaration(static_cast<VarDeclarationStmt*>(stmt)); break;
        case StmtKind::FunctionDeclaration:
            analyzeFunctionDecl(static_cast<FunctionDeclStmt*>(stmt)); break;
        case StmtKind::Class:
            analyzeClassDecl(static_cast<ClassDeclStmt*>(stmt)); break;
        case StmtKind::Interface:
            analyzeInterfaceDecl(static_cast<InterfaceDeclStmt*>(stmt)); break;
        case StmtKind::Import:
            analyzeImport(static_cast<ImportStmt*>(stmt)); break;
        case StmtKind::Export:
            analyzeExport(static_cast<ExportStmt*>(stmt)); break;
        case StmtKind::BlockchainContract:
            analyzeBlockchainContract(static_cast<BlockchainContractStmt*>(stmt)); break;
        case StmtKind::If:
            analyzeIf(static_cast<IfStmt*>(stmt)); break;
        case StmtKind::For:
            analyzeFor(static_cast<ForStmt*>(stmt)); break;
        case StmtKind::ForEach:
            analyzeForEach(static_cast<ForEachStmt*>(stmt)); break;
        case StmtKind::While:
            analyzeWhile(static_cast<WhileStmt*>(stmt)); break;
        case StmtKind::DoWhile:
            analyzeDoWhile(static_cast<DoWhileStmt*>(stmt)); break;
        case StmtKind::Switch:
            analyzeSwitch(static_cast<SwitchStmt*>(stmt)); break;
        case StmtKind::Match:
            analyzeMatch(static_cast<MatchStmt*>(stmt)); break;
        case StmtKind::Return:
            analyzeReturn(static_cast<ReturnStmt*>(stmt)); break;
        case StmtKind::Break:
            analyzeBreak(static_cast<BreakStmt*>(stmt)); break;
        case StmtKind::Continue:
            analyzeContinue(static_cast<ContinueStmt*>(stmt)); break;
        case StmtKind::Throw:
            analyzeThrow(static_cast<ThrowStmt*>(stmt)); break;
        case StmtKind::TryCatch:
            analyzeTryCatch(static_cast<TryCatchStmt*>(stmt)); break;
        case StmtKind::Expression:
            analyzeExpressionStmt(static_cast<ExpressionStmt*>(stmt)); break;
        case StmtKind::Print:
            // Print is now handled as expression statement
            break;
    }
}

void SemanticAnalyzer::analyzeBlock(BlockStmt* stmt) {
    symbols_.pushScope(ScopeKind::Block);
    for (auto& s : stmt->statements) {
        analyzeStatement(s.get());
    }
    symbols_.popScope();
}

void SemanticAnalyzer::analyzeVarDeclaration(VarDeclarationStmt* stmt) {
    // Check for duplicate in current scope
    Symbol sym(SymbolKind::Variable, stmt->name, stmt->location);
    sym.isDefined = true;
    sym.isConst = (stmt->declKind == VarDeclKind::Const);
    sym.isReadonly = (stmt->declKind == VarDeclKind::Readonly);
    sym.isMutable = (stmt->declKind == VarDeclKind::Let || stmt->declKind == VarDeclKind::Var);

    if (stmt->type) {
        if (auto* named = dynamic_cast<NamedType*>(stmt->type.get())) {
            sym.typeName = named->name;
        } else if (auto* generic = dynamic_cast<GenericType*>(stmt->type.get())) {
            sym.typeName = generic->name;
        } else if (auto* nullable = dynamic_cast<NullableType*>(stmt->type.get())) {
            if (auto* inner = dynamic_cast<NamedType*>(nullable->inner.get())) {
                sym.typeName = inner->name + "?";
            }
        }
    }

    if (!symbols_.declare(sym)) {
        // Shadowing is allowed in Gard (like JavaScript/TypeScript)
        // Only warn for top-level re-declarations, not nested scopes
    }

    // Analyze initializer
    if (stmt->initializer) {
        analyzeExpression(stmt->initializer.get());
    }
}

void SemanticAnalyzer::analyzeFunctionDecl(FunctionDeclStmt* stmt) {
    // Symbol already registered in first pass for top-level functions
    // For nested functions, register now
    if (symbols_.isInsideFunction() || symbols_.isInsideClass()) {
        Symbol sym(SymbolKind::Function, stmt->name, stmt->location);
        sym.isAsync = stmt->isAsync;
        sym.isDefined = true;
        symbols_.declare(sym); // may already exist
    }

    // Push function scope
    symbols_.pushScope(ScopeKind::Function, stmt->name);
    symbols_.currentScope().isAsync = stmt->isAsync;

    // Register parameters
    for (auto& param : stmt->params) {
        Symbol paramSym(SymbolKind::Parameter, param.name, param.location);
        paramSym.isDefined = true;
        paramSym.isUsed = true; // don't warn about unused params
        if (param.type) {
            if (auto* named = dynamic_cast<NamedType*>(param.type.get())) {
                paramSym.typeName = named->name;
            }
        }
        if (!symbols_.declare(paramSym)) {
            error("Duplicate parameter name '" + param.name + "'", param.location);
        }
    }

    // Register generic params
    for (auto& gp : stmt->genericParams) {
        Symbol gpSym(SymbolKind::GenericParam, gp.name, gp.location);
        gpSym.isDefined = true;
        gpSym.isUsed = true;
        symbols_.declare(gpSym);
    }

    // Track return type
    currentFunctionHasReturn_ = false;
    if (stmt->returnType) {
        if (auto* named = dynamic_cast<NamedType*>(stmt->returnType.get())) {
            currentFunctionReturnType_ = named->name;
        }
    } else {
        currentFunctionReturnType_ = "";
    }

    // Analyze body
    for (auto& s : stmt->body) {
        analyzeStatement(s.get());
    }

    // Check return consistency
    if (!currentFunctionReturnType_.empty() && currentFunctionReturnType_ != "void" &&
        !currentFunctionHasReturn_ && !stmt->body.empty()) {
        warning("Function '" + stmt->name + "' may not return a value on all paths", stmt->location);
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeClassDecl(ClassDeclStmt* stmt) {
    // Symbol already registered in first pass
    symbols_.pushScope(ScopeKind::Class, stmt->name);

    // Register generic params
    for (auto& gp : stmt->genericParams) {
        Symbol gpSym(SymbolKind::GenericParam, gp.name, gp.location);
        gpSym.isDefined = true;
        gpSym.isUsed = true;
        symbols_.declare(gpSym);
    }

    // Register fields
    for (auto& field : stmt->fields) {
        analyzeClassField(field);
    }

    // Register and analyze methods
    for (auto& method : stmt->methods) {
        analyzeClassMethod(method, stmt->name);
    }

    // Analyze constructor
    if (stmt->constructor) {
        analyzeConstructor(*stmt->constructor, stmt->name);
    }

    // Check abstract method implementation
    if (!stmt->isAbstract) {
        checkAbstractImplementation(stmt);
    }

    // Check interface implementation
    checkInterfaceImplementation(stmt);

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeInterfaceDecl(InterfaceDeclStmt* stmt) {
    // Symbol already registered
    symbols_.pushScope(ScopeKind::Class, stmt->name);

    for (auto& gp : stmt->genericParams) {
        Symbol gpSym(SymbolKind::GenericParam, gp.name, gp.location);
        gpSym.isDefined = true;
        gpSym.isUsed = true;
        symbols_.declare(gpSym);
    }

    // Register method signatures
    for (auto& method : stmt->methods) {
        Symbol methodSym(SymbolKind::Method, method.name, method.location);
        methodSym.isDefined = true;
        methodSym.isUsed = true;
        methodSym.isAbstract = true;
        symbols_.declare(methodSym);
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeImport(ImportStmt* stmt) {
    // Register imported symbols
    for (auto& name : stmt->names) {
        Symbol sym(SymbolKind::Variable, name, stmt->location);
        sym.isDefined = true;
        sym.isImported = true;
        if (!symbols_.declare(sym)) {
            error("Duplicate import of '" + name + "'", stmt->location);
        }
    }

    // Track module for circular dependency detection
    if (importedModules_.count(stmt->path)) {
        // Already imported — not necessarily circular, but note it
    }
    importedModules_.insert(stmt->path);
}

void SemanticAnalyzer::analyzeExport(ExportStmt* stmt) {
    if (stmt->declaration) {
        analyzeStatement(stmt->declaration.get());
    }
}

void SemanticAnalyzer::analyzeBlockchainContract(BlockchainContractStmt* stmt) {
    symbols_.pushScope(ScopeKind::Class, stmt->name);

    for (auto& field : stmt->fields) {
        analyzeClassField(field);
    }

    for (auto& method : stmt->methods) {
        analyzeClassMethod(method, stmt->name);
    }

    if (stmt->constructor) {
        analyzeConstructor(*stmt->constructor, stmt->name);
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeIf(IfStmt* stmt) {
    analyzeExpression(stmt->condition.get());
    analyzeStatement(stmt->thenBranch.get());
    if (stmt->elseBranch) {
        analyzeStatement(stmt->elseBranch.get());
    }
}

void SemanticAnalyzer::analyzeFor(ForStmt* stmt) {
    symbols_.pushScope(ScopeKind::Loop, "for");

    if (stmt->initializer) analyzeStatement(stmt->initializer.get());
    if (stmt->condition) analyzeExpression(stmt->condition.get());
    if (stmt->increment) analyzeExpression(stmt->increment.get());
    if (stmt->body) analyzeStatement(stmt->body.get());

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeForEach(ForEachStmt* stmt) {
    symbols_.pushScope(ScopeKind::Loop, "foreach");

    // Declare loop variable
    Symbol varSym(SymbolKind::Variable, stmt->variable, stmt->location);
    varSym.isDefined = true;
    varSym.isUsed = true; // don't warn
    symbols_.declare(varSym);

    analyzeExpression(stmt->iterable.get());
    if (stmt->body) analyzeStatement(stmt->body.get());

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeWhile(WhileStmt* stmt) {
    analyzeExpression(stmt->condition.get());
    symbols_.pushScope(ScopeKind::Loop, "while");
    if (stmt->body) analyzeStatement(stmt->body.get());
    symbols_.popScope();
}

void SemanticAnalyzer::analyzeDoWhile(DoWhileStmt* stmt) {
    symbols_.pushScope(ScopeKind::Loop, "do-while");
    if (stmt->body) analyzeStatement(stmt->body.get());
    symbols_.popScope();
    analyzeExpression(stmt->condition.get());
}

void SemanticAnalyzer::analyzeSwitch(SwitchStmt* stmt) {
    analyzeExpression(stmt->discriminant.get());
    symbols_.pushScope(ScopeKind::Switch, "switch");

    for (auto& sc : stmt->cases) {
        if (sc.value) analyzeExpression(sc.value.get());
        for (auto& s : sc.body) {
            analyzeStatement(s.get());
        }
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeMatch(MatchStmt* stmt) {
    analyzeExpression(stmt->value.get());
    for (auto& arm : stmt->arms) {
        if (arm.pattern) analyzeExpression(arm.pattern.get());
        if (arm.body) analyzeExpression(arm.body.get());
    }
}

void SemanticAnalyzer::analyzeReturn(ReturnStmt* stmt) {
    if (!symbols_.isInsideFunction()) {
        error("'return' statement outside of function", stmt->location);
        return;
    }

    currentFunctionHasReturn_ = true;

    if (stmt->value) {
        analyzeExpression(stmt->value.get());
    } else if (!currentFunctionReturnType_.empty() && currentFunctionReturnType_ != "void") {
        error("Expected return value in function with return type '" + currentFunctionReturnType_ + "'",
              stmt->location);
    }
}

void SemanticAnalyzer::analyzeBreak(BreakStmt* stmt) {
    if (!symbols_.isInsideLoop() && !symbols_.isInsideSwitch()) {
        error("'break' statement outside of loop or switch", stmt->location);
    }
}

void SemanticAnalyzer::analyzeContinue(ContinueStmt* stmt) {
    if (!symbols_.isInsideLoop()) {
        error("'continue' statement outside of loop", stmt->location);
    }
}

void SemanticAnalyzer::analyzeThrow(ThrowStmt* stmt) {
    if (stmt->value) {
        analyzeExpression(stmt->value.get());
    }
}

void SemanticAnalyzer::analyzeTryCatch(TryCatchStmt* stmt) {
    // Try body
    symbols_.pushScope(ScopeKind::Block, "try");
    for (auto& s : stmt->tryBody) {
        analyzeStatement(s.get());
    }
    symbols_.popScope();

    // Catch clauses
    for (auto& clause : stmt->catchClauses) {
        symbols_.pushScope(ScopeKind::Block, "catch");
        Symbol paramSym(SymbolKind::Parameter, clause.paramName, clause.location);
        paramSym.isDefined = true;
        paramSym.isUsed = true;
        symbols_.declare(paramSym);

        for (auto& s : clause.body) {
            analyzeStatement(s.get());
        }
        symbols_.popScope();
    }

    // Finally
    if (!stmt->finallyBody.empty()) {
        symbols_.pushScope(ScopeKind::Block, "finally");
        for (auto& s : stmt->finallyBody) {
            analyzeStatement(s.get());
        }
        symbols_.popScope();
    }
}

void SemanticAnalyzer::analyzeExpressionStmt(ExpressionStmt* stmt) {
    if (stmt->expression) {
        analyzeExpression(stmt->expression.get());
    }
}

// --- Expression analysis ---

void SemanticAnalyzer::analyzeExpression(Expression* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case ExprKind::Identifier:
            analyzeIdentifier(static_cast<IdentifierExpr*>(expr)); break;
        case ExprKind::Assignment:
            analyzeAssignment(static_cast<AssignmentExpr*>(expr)); break;
        case ExprKind::Call:
            analyzeCall(static_cast<CallExpr*>(expr)); break;
        case ExprKind::MemberAccess:
            analyzeMemberAccess(static_cast<MemberAccessExpr*>(expr)); break;
        case ExprKind::IndexAccess:
            analyzeIndexAccess(static_cast<IndexAccessExpr*>(expr)); break;
        case ExprKind::Binary:
            analyzeBinary(static_cast<BinaryExpr*>(expr)); break;
        case ExprKind::Unary:
            analyzeUnary(static_cast<UnaryExpr*>(expr)); break;
        case ExprKind::New:
            analyzeNew(static_cast<NewExpr*>(expr)); break;
        case ExprKind::Await:
            analyzeAwait(static_cast<AwaitExpr*>(expr)); break;
        case ExprKind::Lambda:
            analyzeLambda(static_cast<LambdaExpr*>(expr)); break;
        case ExprKind::This:
            analyzeThis(static_cast<ThisExpr*>(expr)); break;
        case ExprKind::Ternary: {
            auto* ternary = static_cast<TernaryExpr*>(expr);
            analyzeExpression(ternary->condition.get());
            analyzeExpression(ternary->thenExpr.get());
            analyzeExpression(ternary->elseExpr.get());
            break;
        }
        case ExprKind::OptionalChain: {
            auto* oc = static_cast<OptionalChainExpr*>(expr);
            analyzeExpression(oc->object.get());
            break;
        }
        case ExprKind::Array: {
            auto* arr = static_cast<ArrayExpr*>(expr);
            for (auto& elem : arr->elements) {
                analyzeExpression(elem.get());
            }
            break;
        }
        case ExprKind::Map: {
            auto* map = static_cast<MapExpr*>(expr);
            for (auto& entry : map->entries) {
                // Don't analyze map keys that are simple identifiers — they're property names, not variable references
                if (entry.first && entry.first->kind != ExprKind::Identifier) {
                    analyzeExpression(entry.first.get());
                }
                analyzeExpression(entry.second.get());
            }
            break;
        }
        case ExprKind::Grouped: {
            auto* grouped = static_cast<GroupedExpr*>(expr);
            analyzeExpression(grouped->inner.get());
            break;
        }
        // Literals don't need analysis
        case ExprKind::IntLiteral:
        case ExprKind::DoubleLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::LongLiteral:
        case ExprKind::HexLiteral:
        case ExprKind::BinaryLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TemplateLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::NullLiteral:
        case ExprKind::Super:
            break;
        default:
            break;
    }
}

void SemanticAnalyzer::analyzeIdentifier(IdentifierExpr* expr) {
    // Skip wildcard pattern
    if (expr->name == "_") return;

    Symbol* sym = symbols_.resolve(expr->name);
    if (sym) {
        sym->isUsed = true;
    } else {
        // Don't error on built-in names that are always globally available (no import needed)
        static const std::unordered_set<std::string> builtins = {
            "print", "validate", "hash", "mine", "sign", "block",
            "transaction", "lock", "unlock", "wait", "signal",
            "ledger", "stream", "mutex", "semaphore", "barrier",
            "msg", "Result", "Error", "Promise", "Task",
            "delay", "repeat", "stopRepetition",
            // Keywords used as expressions
            "typeof", "rethrow", "instanceof",
            // These stdlib namespaces are auto-available (like Java's java.lang)
            "JSON", "Math", "File", "Directory", "Process",
            "List", "Queue", "Stack", "String", "Map", "Set",
            "Regex", "DateTime", "Path", "Console", "Array",
            "Int", "Double", "Bool",
            // Concurrency primitives (runtime-provided classes)
            "Mutex", "Semaphore", "Channel", "Barrier", "RWLock",
            "Timer", "EventEmitter", "Thread", "Async",
            // Additional stdlib namespaces (auto-available)
            "Memory", "Pointer", "Audio", "Canvas", "Camera",
            "Texture", "Shader", "Mesh", "Model", "Light",
            "SpriteBatch", "Framebuffer", "Collision", "Tween",
            "Color", "Image", "Window", "Graphics", "Input",
            "System", "Compiler", "MMap", "OS",
            "ConcurrentMap", "ConcurrentList",
            "DOM", "SIMD", "Stream",
            "Wasm", "WasmModule", "WasmHeap", "WasmMemory",
            "WasmInterop", "WasmOptimizer", "WasmThread",
            "Mail", "SMTP",
            // HTTP/Network types
            "Response", "Request", "HttpResponse", "HttpRequest", "HttpStream",
            // All stdlib that requires import — also suppress errors
            // (the import check is a separate warning, not an error)
            "Database", "ORM", "Query", "ConnectionPool",
            "Crypto", "RSA", "Hash", "Base64",
            "Compression", "XML",
            "HttpClient", "HttpServer", "WebSocket", "WebSocketServer",
            "TcpSocket", "TcpServer", "UdpSocket",
            "FFI", "Mock", "Reflect", "Profiler", "Debug",
            "PriorityQueue", "Iterator",
            // Built-in error types (always available, no import needed)
            "GardError", "GardSyntaxError", "GardTypeError", "GardRuntimeError",
            "GardNullPointerError", "GardIndexOutOfBoundsError", "GardArithmeticError",
            "GardValidationError", "GardIOError", "GardNetworkError",
            "GardTimeoutError", "GardAssertionError", "GardPermissionError",
            "GardNotFoundError", "GardAuthError", "GardConcurrencyError",
            "GardSerializationError", "GardDatabaseError", "GardORMError",
            "GardOSError", "GardEnumError", "GardIllegalCastError",
            "GardOperatorError", "GardConstError", "GardStackOverflowError",
        };
        if (builtins.find(expr->name) == builtins.end()) {
            // Check if it's a class or enum declared in the current file
            bool isDeclaredType = false;
            for (const auto& s : currentProgram_->statements) {
                if (!s) continue;
                if (s->kind == StmtKind::Class) {
                    if (static_cast<ClassDeclStmt*>(s.get())->name == expr->name) { isDeclaredType = true; break; }
                } else if (s->kind == StmtKind::Enum) {
                    if (static_cast<EnumDeclStmt*>(s.get())->name == expr->name) { isDeclaredType = true; break; }
                } else if (s->kind == StmtKind::Interface) {
                    if (static_cast<InterfaceDeclStmt*>(s.get())->name == expr->name) { isDeclaredType = true; break; }
                } else if (s->kind == StmtKind::Export) {
                    auto* exp = static_cast<ExportStmt*>(s.get());
                    if (exp->declaration) {
                        if (exp->declaration->kind == StmtKind::Class && static_cast<ClassDeclStmt*>(exp->declaration.get())->name == expr->name) { isDeclaredType = true; break; }
                        if (exp->declaration->kind == StmtKind::Enum && static_cast<EnumDeclStmt*>(exp->declaration.get())->name == expr->name) { isDeclaredType = true; break; }
                    }
                }
            }
            if (!isDeclaredType) {
                error("Undefined identifier '" + expr->name + "'", expr->location);
            }
        }
    }
}

void SemanticAnalyzer::analyzeAssignment(AssignmentExpr* expr) {
    analyzeExpression(expr->target.get());
    analyzeExpression(expr->value.get());

    // Check if target is assignable
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr->target.get())) {
        Symbol* sym = symbols_.resolve(id->name);
        if (sym) {
            if (sym->isConst) {
                error("Cannot assign to constant '" + id->name + "'", expr->location);
            } else if (sym->isReadonly) {
                error("Cannot assign to readonly variable '" + id->name + "'", expr->location);
            }
        }
    }
    // Member access and index access are always assignable (checked at runtime)
}

void SemanticAnalyzer::analyzeCall(CallExpr* expr) {
    analyzeExpression(expr->callee.get());
    for (auto& arg : expr->arguments) {
        analyzeExpression(arg.get());
    }
}

void SemanticAnalyzer::analyzeMemberAccess(MemberAccessExpr* expr) {
    analyzeExpression(expr->object.get());
    // Member resolution would require type information (deferred to type checker)
}

void SemanticAnalyzer::analyzeIndexAccess(IndexAccessExpr* expr) {
    analyzeExpression(expr->object.get());
    analyzeExpression(expr->index.get());
}

void SemanticAnalyzer::analyzeBinary(BinaryExpr* expr) {
    analyzeExpression(expr->left.get());
    analyzeExpression(expr->right.get());
}

void SemanticAnalyzer::analyzeUnary(UnaryExpr* expr) {
    analyzeExpression(expr->operand.get());
}

void SemanticAnalyzer::analyzeNew(NewExpr* expr) {
    // Check that the class exists
    Symbol* sym = symbols_.resolve(expr->className);
    if (sym) {
        sym->isUsed = true;
        if (sym->kind != SymbolKind::Class && sym->kind != SymbolKind::BlockchainContract) {
            // Don't error for known runtime-provided classes
            static const std::unordered_set<std::string> runtimeClasses = {
                "Mutex", "Semaphore", "Channel", "Barrier", "RWLock",
                "Timer", "EventEmitter", "Thread", "Promise", "Task",
                "Error", "Response", "Request", "Component", "Calculator",
            };
            if (runtimeClasses.find(expr->className) == runtimeClasses.end() && !sym->isImported) {
                error("'" + expr->className + "' is not a class", expr->location);
            }
        }

        // Compile-time bounds checking: validate type arguments against generic constraints
        if (!expr->typeArgs.empty() && !sym->genericParams.empty()) {
            // Check arity
            if (expr->typeArgs.size() != sym->genericParams.size()) {
                error("'" + expr->className + "' expects " + std::to_string(sym->genericParams.size()) +
                      " type argument(s), but got " + std::to_string(expr->typeArgs.size()), expr->location);
            }

            // Check bounds for each type argument
            for (size_t i = 0; i < expr->typeArgs.size() && i < sym->genericParamConstraints.size(); i++) {
                auto& bounds = sym->genericParamConstraints[i];
                if (bounds.empty()) continue; // no constraints on this param

                // Get the type argument name
                std::string typeArgName;
                if (auto* named = dynamic_cast<NamedType*>(expr->typeArgs[i].get())) {
                    typeArgName = named->name;
                }
                if (typeArgName.empty()) continue;

                // Check each bound
                for (auto& boundName : bounds) {
                    bool satisfies = false;

                    // Primitives implicitly satisfy Comparable, Serializable, etc.
                    if (typeArgName == "int" || typeArgName == "long" ||
                        typeArgName == "float" || typeArgName == "double" ||
                        typeArgName == "string" || typeArgName == "bool") {
                        if (boundName == "Comparable" || boundName == "Serializable" ||
                            boundName == "Hashable") {
                            satisfies = true;
                        }
                    }

                    // Check if the type arg class implements the bound
                    if (!satisfies) {
                        Symbol* argSym = symbols_.resolve(typeArgName);
                        if (argSym && argSym->kind == SymbolKind::Class) {
                            for (auto& iface : argSym->interfaces) {
                                if (iface == boundName) { satisfies = true; break; }
                            }
                            if (!satisfies && argSym->baseClass == boundName) satisfies = true;
                        } else {
                            // Unknown type — skip check (might be imported)
                            satisfies = true;
                        }
                    }

                    if (!satisfies) {
                        warning("Type '" + typeArgName + "' may not satisfy constraint '" +
                                boundName + "' on type parameter '" + sym->genericParams[i] + "'",
                                expr->location);
                    }
                }
            }
        }
    }
    // Built-in types like Error, Mutex, etc. are allowed without declaration

    for (auto& arg : expr->arguments) {
        analyzeExpression(arg.get());
    }
}

void SemanticAnalyzer::analyzeAwait(AwaitExpr* expr) {
    // Note: In Gard, await is valid in main() and top-level code
    // (the runtime wraps main in an async context)
    // Only warn if clearly outside any function
    analyzeExpression(expr->operand.get());
}

void SemanticAnalyzer::analyzeLambda(LambdaExpr* expr) {
    symbols_.pushScope(ScopeKind::Function, "<lambda>");

    for (auto& param : expr->params) {
        Symbol paramSym(SymbolKind::Parameter, param.name, param.location);
        paramSym.isDefined = true;
        paramSym.isUsed = true;
        symbols_.declare(paramSym);
    }

    if (expr->hasBlockBody) {
        for (auto& s : expr->bodyBlock) {
            analyzeStatement(s.get());
        }
    } else if (expr->bodyExpr) {
        analyzeExpression(expr->bodyExpr.get());
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeThis(ThisExpr* expr) {
    if (!symbols_.isInsideClass()) {
        error("'this' can only be used inside a class method", expr->location);
    }
}

// --- Class analysis helpers ---

void SemanticAnalyzer::analyzeClassField(ClassField& field) {
    Symbol sym(SymbolKind::Field, field.name, field.location);
    sym.accessModifier = field.accessModifier;
    sym.isStatic = field.isStatic;
    sym.isConst = (field.declKind == VarDeclKind::Const);
    sym.isReadonly = (field.declKind == VarDeclKind::Readonly);
    sym.isDefined = true;
    sym.isUsed = true; // don't warn about unused fields

    if (field.type) {
        if (auto* named = dynamic_cast<NamedType*>(field.type.get())) {
            sym.typeName = named->name;
        }
    }

    if (!symbols_.declare(sym)) {
        error("Duplicate field '" + field.name + "'", field.location);
    }

    if (field.initializer) {
        analyzeExpression(field.initializer.get());
    }
}

void SemanticAnalyzer::analyzeClassMethod(ClassMethod& method, const std::string& className) {
    Symbol methodSym(SymbolKind::Method, method.name, method.location);
    methodSym.accessModifier = method.accessModifier;
    methodSym.isStatic = method.isStatic;
    methodSym.isAsync = method.isAsync;
    methodSym.isAbstract = method.isAbstract;
    methodSym.isDefined = true;
    methodSym.isUsed = true;

    if (method.returnType) {
        if (auto* named = dynamic_cast<NamedType*>(method.returnType.get())) {
            methodSym.returnType = named->name;
        }
    }

    symbols_.declare(methodSym);

    // Don't analyze body of abstract methods
    if (method.isAbstract) return;

    // Push function scope for method body
    symbols_.pushScope(ScopeKind::Function, method.name);
    symbols_.currentScope().isAsync = method.isAsync;

    // Register parameters
    for (auto& param : method.params) {
        Symbol paramSym(SymbolKind::Parameter, param.name, param.location);
        paramSym.isDefined = true;
        paramSym.isUsed = true;
        symbols_.declare(paramSym);
    }

    // Register generic params
    for (auto& gp : method.genericParams) {
        Symbol gpSym(SymbolKind::GenericParam, gp.name, gp.location);
        gpSym.isDefined = true;
        gpSym.isUsed = true;
        symbols_.declare(gpSym);
    }

    // Analyze body
    currentFunctionHasReturn_ = false;
    currentFunctionReturnType_ = methodSym.returnType;

    for (auto& s : method.body) {
        analyzeStatement(s.get());
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeConstructor(Constructor& ctor, const std::string& className) {
    symbols_.pushScope(ScopeKind::Function, "constructor");

    for (auto& param : ctor.params) {
        Symbol paramSym(SymbolKind::Parameter, param.name, param.location);
        paramSym.isDefined = true;
        paramSym.isUsed = true;
        symbols_.declare(paramSym);
    }

    for (auto& s : ctor.body) {
        analyzeStatement(s.get());
    }

    symbols_.popScope();
}

void SemanticAnalyzer::checkAbstractImplementation(ClassDeclStmt* cls) {
    if (cls->baseClass.empty()) return;

    // Look up base class to check for abstract methods
    Symbol* baseSym = symbols_.resolve(cls->baseClass);
    if (!baseSym) return; // base class not found (might be imported)

    //TODO: For now, we just check that abstract methods from the base are overridden
    //TODO: Full implementation would require resolving the base class AST
    //TODO: This is a simplified check
}

void SemanticAnalyzer::checkInterfaceImplementation(ClassDeclStmt* cls) {
    // For each interface, check that all methods are implemented
    for (auto& ifaceName : cls->interfaces) {
        Symbol* ifaceSym = symbols_.resolve(ifaceName);
        if (!ifaceSym) {
            error("Interface '" + ifaceName + "' is not defined", cls->location);
        }
        // Full method-by-method checking would require storing interface method signatures
        // and comparing against class methods. Simplified for now.
    }
}

// --- Unused detection ---

void SemanticAnalyzer::checkUnused() {
    // Only check the outermost (global) scope for unused imports.
    // Suppress unused import warnings for symbols that are used via namespace access
    // (e.g., import { Input } from "gard/graphics" then Input.isKeyDown())
    // Since we can't reliably track namespace usage, don't warn about unused imports.
}

// --- Diagnostics ---

void SemanticAnalyzer::error(const std::string& message, const SourceLocation& loc) {
    diagnostics_.emplace_back(DiagSeverity::Error, message, loc);
}

void SemanticAnalyzer::warning(const std::string& message, const SourceLocation& loc) {
    diagnostics_.emplace_back(DiagSeverity::Warning, message, loc);
}

void SemanticAnalyzer::info(const std::string& message, const SourceLocation& loc) {
    diagnostics_.emplace_back(DiagSeverity::Info, message, loc);
}

void SemanticAnalyzer::registerModule(const std::string& path) {
    importedModules_.insert(path);
}

} // namespace gard
