#include "types/checker.h"

namespace gard {

TypeChecker::TypeChecker(SymbolTable& symbols) : symbols_(symbols) {
    pushTypeEnv(); // global scope
}

void TypeChecker::pushTypeEnv() {
    typeEnv_.emplace_back();
}

void TypeChecker::popTypeEnv() {
    if (!typeEnv_.empty()) typeEnv_.pop_back();
}

void TypeChecker::setVarType(const std::string& name, TypeRef type) {
    if (!typeEnv_.empty()) {
        typeEnv_.back()[name] = std::move(type);
    }
}

TypeRef TypeChecker::getVarType(const std::string& name) const {
    // Search from innermost scope outward
    for (int i = static_cast<int>(typeEnv_.size()) - 1; i >= 0; i--) {
        auto it = typeEnv_[i].find(name);
        if (it != typeEnv_[i].end()) return it->second;
    }
    return nullptr;
}

void TypeChecker::check(Program& program) {
    // First pass: register all class/interface types
    for (auto& stmt : program.statements) {
        if (auto* cls = dynamic_cast<ClassDeclStmt*>(stmt.get())) {
            registerClassType(cls);
        } else if (auto* iface = dynamic_cast<InterfaceDeclStmt*>(stmt.get())) {
            registerInterfaceType(iface);
        } else if (auto* exp = dynamic_cast<ExportStmt*>(stmt.get())) {
            if (exp->declaration) {
                if (auto* cls = dynamic_cast<ClassDeclStmt*>(exp->declaration.get())) {
                    registerClassType(cls);
                } else if (auto* iface = dynamic_cast<InterfaceDeclStmt*>(exp->declaration.get())) {
                    registerInterfaceType(iface);
                }
            }
        }
    }

    // Second pass: type check all statements
    for (auto& stmt : program.statements) {
        checkStatement(stmt.get());
    }
}

bool TypeChecker::hasErrors() const {
    for (auto& d : diagnostics_) {
        if (d.severity == DiagSeverity::Error) return true;
    }
    return false;
}

int TypeChecker::errorCount() const {
    int count = 0;
    for (auto& d : diagnostics_) {
        if (d.severity == DiagSeverity::Error) count++;
    }
    return count;
}

int TypeChecker::warningCount() const {
    int count = 0;
    for (auto& d : diagnostics_) {
        if (d.severity == DiagSeverity::Warning) count++;
    }
    return count;
}

// --- Type resolution ---

TypeRef TypeChecker::resolveTypeAnnotation(TypeAnnotation* annotation) {
    if (!annotation) return registry_.getUnknown();

    switch (annotation->kind) {
        case TypeKind::Named: {
            auto* named = static_cast<NamedType*>(annotation);
            TypeRef resolved = registry_.resolveTypeName(named->name);
            if (resolved) return resolved;
            // Check user types
            resolved = registry_.lookupUserType(named->name);
            if (resolved) return resolved;
            // Unknown type — might be imported or forward-declared
            return registry_.getUnknown();
        }
        case TypeKind::Generic: {
            auto* generic = static_cast<GenericType*>(annotation);
            // Handle built-in collection types
            if (generic->name == "array" && generic->typeArgs.size() == 1) {
                auto elem = resolveTypeAnnotation(generic->typeArgs[0].get());
                return registry_.makeArray(elem);
            }
            if (generic->name == "map" && generic->typeArgs.size() == 2) {
                auto key = resolveTypeAnnotation(generic->typeArgs[0].get());
                auto val = resolveTypeAnnotation(generic->typeArgs[1].get());
                return registry_.makeMap(key, val);
            }
            if (generic->name == "set" && generic->typeArgs.size() == 1) {
                auto elem = resolveTypeAnnotation(generic->typeArgs[0].get());
                return registry_.makeSet(elem);
            }
            // Generic instantiation (e.g. Result<string, Error>)
            std::vector<TypeRef> args;
            for (auto& arg : generic->typeArgs) {
                args.push_back(resolveTypeAnnotation(arg.get()));
            }
            return std::make_shared<GenericInstType>(generic->name, std::move(args));
        }
        case TypeKind::Nullable: {
            auto* nullable = static_cast<NullableType*>(annotation);
            auto inner = resolveTypeAnnotation(nullable->inner.get());
            return registry_.makeNullable(inner);
        }
        case TypeKind::Tuple: {
            auto* tuple = static_cast<TupleType*>(annotation);
            std::vector<TypeRef> elems;
            for (auto& elem : tuple->elements) {
                elems.push_back(resolveTypeAnnotation(elem.get()));
            }
            return registry_.makeTuple(std::move(elems));
        }
        case TypeKind::Function: {
            auto* func = static_cast<FunctionType*>(annotation);
            std::vector<TypeRef> params;
            for (auto& p : func->paramTypes) {
                params.push_back(resolveTypeAnnotation(p.get()));
            }
            auto ret = resolveTypeAnnotation(func->returnType.get());
            return registry_.makeFunction(std::move(params), ret);
        }
    }
    return registry_.getUnknown();
}

void TypeChecker::registerClassType(ClassDeclStmt* cls) {
    auto classType = std::make_shared<ClassType>(cls->name);
    classType->isAbstract = cls->isAbstract;
    classType->interfaces = cls->interfaces;

    for (auto& gp : cls->genericParams) {
        classType->genericParams.push_back(gp.name);
    }

    // Register fields
    for (auto& field : cls->fields) {
        if (field.type) {
            classType->fields[field.name] = resolveTypeAnnotation(field.type.get());
        }
    }

    // Register methods (return types)
    for (auto& method : cls->methods) {
        if (method.returnType) {
            classType->methods[method.name] = resolveTypeAnnotation(method.returnType.get());
        } else {
            classType->methods[method.name] = registry_.getVoid();
        }
    }

    registry_.registerClass(cls->name, classType);
}

void TypeChecker::registerInterfaceType(InterfaceDeclStmt* iface) {
    auto ifaceType = std::make_shared<InterfaceType>(iface->name);

    for (auto& method : iface->methods) {
        if (method.returnType) {
            ifaceType->methods[method.name] = resolveTypeAnnotation(method.returnType.get());
        } else {
            ifaceType->methods[method.name] = registry_.getVoid();
        }
    }

    registry_.registerInterface(iface->name, ifaceType);
}

// --- Statement type checking ---

void TypeChecker::checkStatement(Statement* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case StmtKind::Block: checkBlock(static_cast<BlockStmt*>(stmt)); break;
        case StmtKind::VarDeclaration: checkVarDeclaration(static_cast<VarDeclarationStmt*>(stmt)); break;
        case StmtKind::FunctionDeclaration: checkFunctionDecl(static_cast<FunctionDeclStmt*>(stmt)); break;
        case StmtKind::Class: checkClassDecl(static_cast<ClassDeclStmt*>(stmt)); break;
        case StmtKind::Return: checkReturn(static_cast<ReturnStmt*>(stmt)); break;
        case StmtKind::If: checkIf(static_cast<IfStmt*>(stmt)); break;
        case StmtKind::For: checkFor(static_cast<ForStmt*>(stmt)); break;
        case StmtKind::ForEach: checkForEach(static_cast<ForEachStmt*>(stmt)); break;
        case StmtKind::While: checkWhile(static_cast<WhileStmt*>(stmt)); break;
        case StmtKind::DoWhile: checkDoWhile(static_cast<DoWhileStmt*>(stmt)); break;
        case StmtKind::Switch: checkSwitch(static_cast<SwitchStmt*>(stmt)); break;
        case StmtKind::Match: checkMatch(static_cast<MatchStmt*>(stmt)); break;
        case StmtKind::Throw: checkThrow(static_cast<ThrowStmt*>(stmt)); break;
        case StmtKind::TryCatch: checkTryCatch(static_cast<TryCatchStmt*>(stmt)); break;
        case StmtKind::Expression: checkExpressionStmt(static_cast<ExpressionStmt*>(stmt)); break;
        case StmtKind::Export: checkExport(static_cast<ExportStmt*>(stmt)); break;
        case StmtKind::BlockchainContract: checkBlockchainContract(static_cast<BlockchainContractStmt*>(stmt)); break;
        default: break;
    }
}

void TypeChecker::checkBlock(BlockStmt* stmt) {
    pushTypeEnv();
    for (auto& s : stmt->statements) {
        checkStatement(s.get());
    }
    popTypeEnv();
}

void TypeChecker::checkVarDeclaration(VarDeclarationStmt* stmt) {
    TypeRef declaredType = nullptr;
    if (stmt->type) {
        declaredType = resolveTypeAnnotation(stmt->type.get());
    }

    TypeRef initType = nullptr;
    if (stmt->initializer) {
        initType = inferExpression(stmt->initializer.get());

        if (declaredType && initType && !declaredType->isUnknown() && !initType->isUnknown()) {
            if (!registry_.isAssignable(initType, declaredType) &&
                !registry_.canImplicitCoerce(initType, declaredType)) {
                error("Cannot assign value of type '" + initType->toString() +
                      "' to variable of type '" + declaredType->toString() + "'",
                      stmt->location);
            }
        }
    }

    // Register variable type: declared type takes priority, then inferred from initializer
    TypeRef varType = declaredType ? declaredType : (initType ? initType : registry_.getUnknown());
    setVarType(stmt->name, varType);
}

void TypeChecker::checkFunctionDecl(FunctionDeclStmt* stmt) {
    // Save context
    auto savedReturn = currentReturnType_;
    auto savedName = currentFunctionName_;

    currentFunctionName_ = stmt->name;
    if (stmt->returnType) {
        currentReturnType_ = resolveTypeAnnotation(stmt->returnType.get());
    } else {
        currentReturnType_ = registry_.getVoid();
    }

    pushTypeEnv();

    // Register parameter types
    for (auto& param : stmt->params) {
        TypeRef paramType = param.type ? resolveTypeAnnotation(param.type.get()) : registry_.getUnknown();
        setVarType(param.name, paramType);
    }

    for (auto& s : stmt->body) {
        checkStatement(s.get());
    }

    popTypeEnv();

    // Restore context
    currentReturnType_ = savedReturn;
    currentFunctionName_ = savedName;
}

void TypeChecker::checkClassDecl(ClassDeclStmt* stmt) {
    // Check field initializers
    for (auto& field : stmt->fields) {
        if (field.initializer && field.type) {
            TypeRef fieldType = resolveTypeAnnotation(field.type.get());
            TypeRef initType = inferExpression(field.initializer.get());

            if (fieldType && initType && !fieldType->isUnknown() && !initType->isUnknown()) {
                if (!registry_.isAssignable(initType, fieldType) &&
                    !registry_.canImplicitCoerce(initType, fieldType)) {
                    error("Cannot assign value of type '" + initType->toString() +
                          "' to field of type '" + fieldType->toString() + "'",
                          field.location);
                }
            }
        }
    }

    // Check methods
    for (auto& method : stmt->methods) {
        if (method.isAbstract) continue;

        auto savedReturn = currentReturnType_;
        auto savedName = currentFunctionName_;

        currentFunctionName_ = method.name;
        if (method.returnType) {
            currentReturnType_ = resolveTypeAnnotation(method.returnType.get());
        } else {
            currentReturnType_ = registry_.getVoid();
        }

        for (auto& s : method.body) {
            checkStatement(s.get());
        }

        currentReturnType_ = savedReturn;
        currentFunctionName_ = savedName;
    }

    // Check constructor
    if (stmt->constructor) {
        for (auto& s : stmt->constructor->body) {
            checkStatement(s.get());
        }
    }
}

void TypeChecker::checkReturn(ReturnStmt* stmt) {
    if (!currentReturnType_) return;

    if (stmt->value) {
        TypeRef valueType = inferExpression(stmt->value.get());
        if (currentReturnType_->isVoid()) {
            error("Cannot return a value from a void function", stmt->location);
        } else if (valueType && !currentReturnType_->isUnknown() && !valueType->isUnknown()) {
            if (!registry_.isAssignable(valueType, currentReturnType_) &&
                !registry_.canImplicitCoerce(valueType, currentReturnType_)) {
                // Allow if base type names match (generic type parameter substitution)
                // e.g., returning Result when return type is Result<T>
                std::string valBase = valueType->name;
                std::string retBase = currentReturnType_->name;
                // Strip generic suffix for comparison
                size_t valAngle = valBase.find('<');
                size_t retAngle = retBase.find('<');
                if (valAngle != std::string::npos) valBase = valBase.substr(0, valAngle);
                if (retAngle != std::string::npos) retBase = retBase.substr(0, retAngle);
                // Allow if base names match OR if return type is a wrapper (future<T>, Promise<T>)
                // and the value type matches the inner type
                bool isWrapper = (retBase == "future" || retBase == "Future" || retBase == "Promise" || retBase == "Task");
                bool innerMatch = false;
                if (isWrapper && retAngle != std::string::npos) {
                    std::string innerType = currentReturnType_->name.substr(retAngle + 1);
                    if (!innerType.empty() && innerType.back() == '>') innerType.pop_back();
                    if (innerType == valueType->name || innerType == valBase) innerMatch = true;
                }
                // Also handle when the return type is a Collection/Generic category wrapping the value type
                if (!innerMatch && currentReturnType_->category == gard::TypeCategory::Generic) {
                    // Generic return type — allow returning the inner type directly
                    // (async functions return future<T> but the body returns T)
                    innerMatch = true;
                }
                if (valBase != retBase && !innerMatch) {
                    error("Cannot return value of type '" + valueType->toString() +
                          "' from function with return type '" + currentReturnType_->toString() + "'",
                          stmt->location);
                }
            }
        }
    } else {
        if (currentReturnType_ && !currentReturnType_->isVoid() && !currentReturnType_->isUnknown()) {
            error("Expected return value of type '" + currentReturnType_->toString() + "'",
                  stmt->location);
        }
    }
}

void TypeChecker::checkIf(IfStmt* stmt) {
    checkConditionType(stmt->condition.get());
    checkStatement(stmt->thenBranch.get());
    if (stmt->elseBranch) checkStatement(stmt->elseBranch.get());
}

void TypeChecker::checkFor(ForStmt* stmt) {
    if (stmt->initializer) checkStatement(stmt->initializer.get());
    if (stmt->condition) checkConditionType(stmt->condition.get());
    if (stmt->increment) inferExpression(stmt->increment.get());
    if (stmt->body) checkStatement(stmt->body.get());
}

void TypeChecker::checkForEach(ForEachStmt* stmt) {
    if (stmt->iterable) inferExpression(stmt->iterable.get());
    if (stmt->body) checkStatement(stmt->body.get());
}

void TypeChecker::checkWhile(WhileStmt* stmt) {
    checkConditionType(stmt->condition.get());
    if (stmt->body) checkStatement(stmt->body.get());
}

void TypeChecker::checkDoWhile(DoWhileStmt* stmt) {
    if (stmt->body) checkStatement(stmt->body.get());
    checkConditionType(stmt->condition.get());
}

void TypeChecker::checkSwitch(SwitchStmt* stmt) {
    inferExpression(stmt->discriminant.get());
    for (auto& sc : stmt->cases) {
        if (sc.value) inferExpression(sc.value.get());
        for (auto& s : sc.body) checkStatement(s.get());
    }
}

void TypeChecker::checkMatch(MatchStmt* stmt) {
    inferExpression(stmt->value.get());
    for (auto& arm : stmt->arms) {
        if (arm.pattern) inferExpression(arm.pattern.get());
        if (arm.body) inferExpression(arm.body.get());
    }
}

void TypeChecker::checkThrow(ThrowStmt* stmt) {
    if (stmt->value) inferExpression(stmt->value.get());
}

void TypeChecker::checkTryCatch(TryCatchStmt* stmt) {
    for (auto& s : stmt->tryBody) checkStatement(s.get());
    for (auto& clause : stmt->catchClauses) {
        for (auto& s : clause.body) checkStatement(s.get());
    }
    for (auto& s : stmt->finallyBody) checkStatement(s.get());
}

void TypeChecker::checkExpressionStmt(ExpressionStmt* stmt) {
    if (stmt->expression) inferExpression(stmt->expression.get());
}

void TypeChecker::checkExport(ExportStmt* stmt) {
    if (stmt->declaration) checkStatement(stmt->declaration.get());
}

void TypeChecker::checkBlockchainContract(BlockchainContractStmt* stmt) {
    for (auto& method : stmt->methods) {
        auto savedReturn = currentReturnType_;
        auto savedName = currentFunctionName_;
        currentFunctionName_ = method.name;
        if (method.returnType) {
            currentReturnType_ = resolveTypeAnnotation(method.returnType.get());
        } else {
            currentReturnType_ = registry_.getVoid();
        }
        for (auto& s : method.body) checkStatement(s.get());
        currentReturnType_ = savedReturn;
        currentFunctionName_ = savedName;
    }
}

// --- Expression type inference ---

TypeRef TypeChecker::inferExpression(Expression* expr) {
    if (!expr) return registry_.getUnknown();

    switch (expr->kind) {
        case ExprKind::IntLiteral: return registry_.getInt();
        case ExprKind::DoubleLiteral: return registry_.getDouble();
        case ExprKind::FloatLiteral: return registry_.getFloat();
        case ExprKind::LongLiteral: return registry_.getLong();
        case ExprKind::HexLiteral: return registry_.getInt();
        case ExprKind::BinaryLiteral: return registry_.getInt();
        case ExprKind::StringLiteral: return registry_.getString();
        case ExprKind::TemplateLiteral: return registry_.getString();
        case ExprKind::CharLiteral: return registry_.getChar();
        case ExprKind::BoolLiteral: return registry_.getBoolean();
        case ExprKind::NullLiteral: return registry_.getNull();
        case ExprKind::This: return registry_.getUnknown(); // resolved by class context
        case ExprKind::Super: return registry_.getUnknown();

        case ExprKind::Identifier: return inferIdentifier(static_cast<IdentifierExpr*>(expr));
        case ExprKind::Binary: return inferBinary(static_cast<BinaryExpr*>(expr));
        case ExprKind::Unary: return inferUnary(static_cast<UnaryExpr*>(expr));
        case ExprKind::Call: return inferCall(static_cast<CallExpr*>(expr));
        case ExprKind::MemberAccess: return inferMemberAccess(static_cast<MemberAccessExpr*>(expr));
        case ExprKind::IndexAccess: return inferIndexAccess(static_cast<IndexAccessExpr*>(expr));
        case ExprKind::Assignment: return inferAssignment(static_cast<AssignmentExpr*>(expr));
        case ExprKind::Ternary: return inferTernary(static_cast<TernaryExpr*>(expr));
        case ExprKind::New: return inferNew(static_cast<NewExpr*>(expr));
        case ExprKind::Await: return inferAwait(static_cast<AwaitExpr*>(expr));
        case ExprKind::Lambda: return inferLambda(static_cast<LambdaExpr*>(expr));
        case ExprKind::Array: return inferArray(static_cast<ArrayExpr*>(expr));
        case ExprKind::Map: return inferMap(static_cast<MapExpr*>(expr));

        case ExprKind::OptionalChain: {
            auto* oc = static_cast<OptionalChainExpr*>(expr);
            auto objType = inferExpression(oc->object.get());
            // Result of optional chain is always nullable
            return registry_.makeNullable(registry_.getUnknown());
        }
        case ExprKind::Grouped: {
            auto* grouped = static_cast<GroupedExpr*>(expr);
            return inferExpression(grouped->inner.get());
        }
        default:
            return registry_.getUnknown();
    }
}

TypeRef TypeChecker::inferIdentifier(IdentifierExpr* expr) {
    // Check local type environment first
    TypeRef localType = getVarType(expr->name);
    if (localType) return localType;

    // Fall back to symbol table
    Symbol* sym = symbols_.resolve(expr->name);
    if (!sym) return registry_.getUnknown();

    // Try to resolve the type from the symbol's type name
    if (!sym->typeName.empty()) {
        std::string typeName = sym->typeName;
        bool nullable = false;
        if (typeName.back() == '?') {
            nullable = true;
            typeName = typeName.substr(0, typeName.size() - 1);
        }
        TypeRef resolved = registry_.resolveTypeName(typeName);
        if (resolved) {
            return nullable ? registry_.makeNullable(resolved) : resolved;
        }
        resolved = registry_.lookupUserType(typeName);
        if (resolved) {
            return nullable ? registry_.makeNullable(resolved) : resolved;
        }
    }

    return registry_.getUnknown();
}

TypeRef TypeChecker::inferBinary(BinaryExpr* expr) {
    TypeRef leftType = inferExpression(expr->left.get());
    TypeRef rightType = inferExpression(expr->right.get());

    std::string op = tokenTypeToString(expr->op);

    // Type check operands for arithmetic
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (op == "+" && (leftType->isString() || rightType->isString())) {
            // String concatenation is always valid
            return registry_.getString();
        }
        if (!leftType->isNumeric() && !leftType->isDynamic() && !leftType->isUnknown() && leftType->category != gard::TypeCategory::Class) {
            error("Operator '" + op + "' requires numeric operands, got '" + leftType->toString() + "'",
                  expr->location);
            return registry_.getError();
        }
        if (!rightType->isNumeric() && !rightType->isDynamic() && !rightType->isUnknown() && rightType->category != gard::TypeCategory::Class) {
            error("Operator '" + op + "' requires numeric operands, got '" + rightType->toString() + "'",
                  expr->location);
            return registry_.getError();
        }
    }

    // Bitwise operators require integral types
    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>" || op == ">>>") {
        if (!leftType->isIntegral() && !leftType->isDynamic() && !leftType->isUnknown()) {
            error("Bitwise operator '" + op + "' requires integer operands, got '" + leftType->toString() + "'",
                  expr->location);
        }
        if (!rightType->isIntegral() && !rightType->isDynamic() && !rightType->isUnknown()) {
            error("Bitwise operator '" + op + "' requires integer operands, got '" + rightType->toString() + "'",
                  expr->location);
        }
    }

    return registry_.binaryOpResult(op, leftType, rightType);
}

TypeRef TypeChecker::inferUnary(UnaryExpr* expr) {
    TypeRef operandType = inferExpression(expr->operand.get());
    std::string op = tokenTypeToString(expr->op);

    if (op == "-") {
        if (!operandType->isNumeric() && !operandType->isDynamic() && !operandType->isUnknown()) {
            error("Unary '-' requires numeric operand, got '" + operandType->toString() + "'",
                  expr->location);
        }
    }
    if (op == "~") {
        if (!operandType->isIntegral() && !operandType->isDynamic() && !operandType->isUnknown()) {
            error("Bitwise NOT '~' requires integer operand, got '" + operandType->toString() + "'",
                  expr->location);
        }
    }
    if (op == "++" || op == "--") {
        if (!operandType->isNumeric() && !operandType->isDynamic() && !operandType->isUnknown()) {
            error("Operator '" + op + "' requires numeric operand, got '" + operandType->toString() + "'",
                  expr->location);
        }
    }

    return registry_.unaryOpResult(op, operandType);
}

TypeRef TypeChecker::inferCall(CallExpr* expr) {
    TypeRef calleeType = inferExpression(expr->callee.get());

    // Infer argument types (for future overload resolution)
    for (auto& arg : expr->arguments) {
        inferExpression(arg.get());
    }

    // If callee is a function type, return its return type
    if (auto* funcType = dynamic_cast<FunctionTypeInfo*>(calleeType.get())) {
        // Check argument count
        if (expr->arguments.size() != funcType->paramTypes.size()) {
            // Allow for default params — just warn if too many
            if (expr->arguments.size() > funcType->paramTypes.size()) {
                error("Too many arguments in function call", expr->location);
            }
        }
        return funcType->returnType;
    }

    // For class method calls, try to resolve from class type
    return registry_.getUnknown();
}

TypeRef TypeChecker::inferMemberAccess(MemberAccessExpr* expr) {
    TypeRef objType = inferExpression(expr->object.get());

    // Try to resolve member from class type
    if (auto* classType = dynamic_cast<ClassType*>(objType.get())) {
        auto fieldIt = classType->fields.find(expr->member);
        if (fieldIt != classType->fields.end()) {
            return fieldIt->second;
        }
        auto methodIt = classType->methods.find(expr->member);
        if (methodIt != classType->methods.end()) {
            return methodIt->second;
        }
    }

    // String has .length, etc.
    if (objType->isString()) {
        if (expr->member == "length") return registry_.getInt();
    }

    return registry_.getUnknown();
}

TypeRef TypeChecker::inferIndexAccess(IndexAccessExpr* expr) {
    TypeRef objType = inferExpression(expr->object.get());
    TypeRef indexType = inferExpression(expr->index.get());

    // Array index
    if (auto* arrType = dynamic_cast<ArrayType*>(objType.get())) {
        if (!indexType->isIntegral() && !indexType->isDynamic() && !indexType->isUnknown()) {
            error("Array index must be an integer, got '" + indexType->toString() + "'",
                  expr->location);
        }
        return arrType->elementType;
    }

    // Map index
    if (auto* mapType = dynamic_cast<MapType*>(objType.get())) {
        return mapType->valueType;
    }

    // String index
    if (objType->isString()) {
        return registry_.getChar();
    }

    return registry_.getUnknown();
}

TypeRef TypeChecker::inferAssignment(AssignmentExpr* expr) {
    TypeRef targetType = inferExpression(expr->target.get());
    TypeRef valueType = inferExpression(expr->value.get());

    if (targetType && valueType && !targetType->isUnknown() && !valueType->isUnknown()) {
        if (!registry_.isAssignable(valueType, targetType) &&
            !registry_.canImplicitCoerce(valueType, targetType)) {
            error("Cannot assign value of type '" + valueType->toString() +
                  "' to target of type '" + targetType->toString() + "'",
                  expr->location);
        }
    }

    return targetType;
}

TypeRef TypeChecker::inferTernary(TernaryExpr* expr) {
    checkConditionType(expr->condition.get());
    TypeRef thenType = inferExpression(expr->thenExpr.get());
    TypeRef elseType = inferExpression(expr->elseExpr.get());
    return registry_.commonType(thenType, elseType);
}

TypeRef TypeChecker::inferNew(NewExpr* expr) {
    // Look up the class type
    TypeRef classType = registry_.lookupUserType(expr->className);
    if (classType) {
        // Infer argument types
        for (auto& arg : expr->arguments) {
            inferExpression(arg.get());
        }
        return classType;
    }

    // Built-in or unknown class
    for (auto& arg : expr->arguments) {
        inferExpression(arg.get());
    }
    return registry_.getUnknown();
}

TypeRef TypeChecker::inferAwait(AwaitExpr* expr) {
    TypeRef operandType = inferExpression(expr->operand.get());
    // await unwraps a Future/Promise — for now return the inner type or unknown
    return registry_.getUnknown();
}

TypeRef TypeChecker::inferLambda(LambdaExpr* expr) {
    std::vector<TypeRef> paramTypes;
    for (auto& param : expr->params) {
        if (param.type) {
            paramTypes.push_back(resolveTypeAnnotation(param.type.get()));
        } else {
            paramTypes.push_back(registry_.getUnknown());
        }
    }

    TypeRef returnType = registry_.getUnknown();
    if (expr->returnType) {
        returnType = resolveTypeAnnotation(expr->returnType.get());
    } else if (expr->bodyExpr) {
        returnType = inferExpression(expr->bodyExpr.get());
    }

    return registry_.makeFunction(std::move(paramTypes), returnType);
}

TypeRef TypeChecker::inferArray(ArrayExpr* expr) {
    if (expr->elements.empty()) {
        return registry_.makeArray(registry_.getUnknown());
    }

    // Infer element type from first element
    TypeRef elemType = inferExpression(expr->elements[0].get());

    // Check all elements are compatible
    for (size_t i = 1; i < expr->elements.size(); i++) {
        TypeRef t = inferExpression(expr->elements[i].get());
        if (t && elemType && !t->isUnknown() && !elemType->isUnknown()) {
            if (!registry_.isAssignable(t, elemType) && !registry_.canImplicitCoerce(t, elemType)) {
                elemType = registry_.commonType(elemType, t);
            }
        }
    }

    return registry_.makeArray(elemType);
}

TypeRef TypeChecker::inferMap(MapExpr* expr) {
    if (expr->entries.empty()) {
        return registry_.makeMap(registry_.getUnknown(), registry_.getUnknown());
    }

    TypeRef keyType = inferExpression(expr->entries[0].first.get());
    TypeRef valType = inferExpression(expr->entries[0].second.get());

    for (size_t i = 1; i < expr->entries.size(); i++) {
        inferExpression(expr->entries[i].first.get());
        inferExpression(expr->entries[i].second.get());
    }

    return registry_.makeMap(keyType, valType);
}

// --- Compatibility checks ---

void TypeChecker::checkAssignability(const TypeRef& from, const TypeRef& to,
                                     const SourceLocation& loc, const std::string& context) {
    if (!from || !to) return;
    if (from->isUnknown() || to->isUnknown()) return;
    if (from->isDynamic() || to->isDynamic()) return;

    if (!registry_.isAssignable(from, to) && !registry_.canImplicitCoerce(from, to)) {
        error(context + ": cannot convert '" + from->toString() + "' to '" + to->toString() + "'", loc);
    }
}

void TypeChecker::checkConditionType(Expression* expr) {
    if (!expr) return;
    TypeRef condType = inferExpression(expr);
    // Conditions should be boolean or truthy — warn if clearly wrong type
    // In Gard, we allow any type in conditions (truthy/falsy semantics)
    // but warn on void
    if (condType && condType->isVoid()) {
        warning("Condition expression has type 'void'", expr->location);
    }
}

// --- Diagnostics ---

void TypeChecker::error(const std::string& message, const SourceLocation& loc) {
    diagnostics_.emplace_back(DiagSeverity::Error, message, loc);
}

void TypeChecker::warning(const std::string& message, const SourceLocation& loc) {
    diagnostics_.emplace_back(DiagSeverity::Warning, message, loc);
}

} // namespace gard
