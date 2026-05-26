#include "ir/irgen.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <fstream>
#include <filesystem>
#include <set>

namespace gard {
namespace ir {

IRGenerator::IRGenerator() {}

std::unique_ptr<Module> IRGenerator::generate(Program& program) {
    module_ = std::make_unique<Module>(program.filename);
    pushScope();

    for (auto& stmt : program.statements) {
        lowerStatement(stmt.get());
    }

    popScope();
    return std::move(module_);
}

// --- Helpers ---

int IRGenerator::nextId() { return idCounter_++; }

std::string IRGenerator::nextLabel(const std::string& prefix) {
    return prefix + std::to_string(labelCounter_++);
}

IRType IRGenerator::mapType(const std::string& typeName) {
    if (typeName == "int" || typeName == "uint") return IRType::Int32;
    if (typeName == "long" || typeName == "bigint") return IRType::Int64;
    if (typeName == "short") return IRType::Int16;
    if (typeName == "double") return IRType::Float64;
    if (typeName == "float") return IRType::Float32;
    if (typeName == "boolean" || typeName == "bool") return IRType::Bool;
    if (typeName == "char") return IRType::Char;
    if (typeName == "string") return IRType::String;
    if (typeName == "void") return IRType::Void;
    if (typeName == "null") return IRType::Null;
    return IRType::Pointer; // classes, interfaces, etc.
}

IRType IRGenerator::mapTypeAnnotation(TypeAnnotation* annotation) {
    if (!annotation) return IRType::Int32;
    if (auto* named = dynamic_cast<NamedType*>(annotation)) {
        return mapType(named->name);
    }
    if (auto* generic = dynamic_cast<GenericType*>(annotation)) {
        if (generic->name == "array") return IRType::Array;
        if (generic->name == "map") return IRType::Map;
        if (generic->name == "set") return IRType::Pointer;
        // future<T> and stream<T> — the runtime value is the inner type or pointer
        if (generic->name == "future" || generic->name == "stream") {
            if (!generic->typeArgs.empty()) {
                IRType inner = mapTypeAnnotation(generic->typeArgs[0].get());
                // If inner is a primitive, the future holds that primitive (sync mode)
                return inner;
            }
            return IRType::Pointer;
        }
    }
    return IRType::Pointer;
}

BasicBlock* IRGenerator::createBlock(const std::string& label) {
    if (!currentFunction_) return nullptr;
    return currentFunction_->createBlock(label);
}

void IRGenerator::setInsertPoint(BasicBlock* block) {
    currentBlock_ = block;
}

ValueRef IRGenerator::emit(Opcode op, IRType type, std::vector<ValueRef> operands) {
    auto inst = std::make_shared<Instruction>(op, type, "t" + std::to_string(nextId()), nextId());
    for (auto& operand : operands) {
        inst->addOperand(std::move(operand));
    }
    inst->parent = currentBlock_;
    inst->sourceLine = currentSourceLine_;
    inst->sourceColumn = currentSourceColumn_;
    if (currentBlock_) {
        currentBlock_->instructions.push_back(inst);
    }
    return inst;
}

ValueRef IRGenerator::emitCall(const std::string& funcName, std::vector<ValueRef> args, IRType retType) {
    auto inst = std::make_shared<Instruction>(Opcode::Call, retType, "t" + std::to_string(nextId()), nextId());
    inst->targetName = funcName;
    for (auto& arg : args) {
        inst->addOperand(std::move(arg));
    }
    inst->parent = currentBlock_;
    inst->sourceLine = currentSourceLine_;
    inst->sourceColumn = currentSourceColumn_;
    if (currentBlock_) {
        currentBlock_->instructions.push_back(inst);
    }
    return inst;
}

void IRGenerator::emitBranch(BasicBlock* target) {
    if (!currentBlock_ || currentBlock_->isTerminated()) return;
    auto inst = std::make_shared<Instruction>(Opcode::Branch, IRType::Void, "br" + std::to_string(nextId()), nextId());
    inst->jumpTarget = target;
    inst->parent = currentBlock_;
    currentBlock_->instructions.push_back(inst);
    if (target) {
        currentBlock_->successors.push_back(target);
        target->predecessors.push_back(currentBlock_);
    }
}

void IRGenerator::emitCondBranch(ValueRef cond, BasicBlock* trueBlock, BasicBlock* falseBlock) {
    if (!currentBlock_ || currentBlock_->isTerminated()) return;
    auto inst = std::make_shared<Instruction>(Opcode::CondBranch, IRType::Void, "cbr" + std::to_string(nextId()), nextId());
    inst->addOperand(std::move(cond));
    inst->trueBlock = trueBlock;
    inst->falseBlock = falseBlock;
    inst->parent = currentBlock_;
    currentBlock_->instructions.push_back(inst);
    currentBlock_->successors.push_back(trueBlock);
    currentBlock_->successors.push_back(falseBlock);
    trueBlock->predecessors.push_back(currentBlock_);
    falseBlock->predecessors.push_back(currentBlock_);
}

void IRGenerator::emitReturn(ValueRef value) {
    if (!currentBlock_ || currentBlock_->isTerminated()) return;
    auto inst = std::make_shared<Instruction>(Opcode::Return, IRType::Void, "ret" + std::to_string(nextId()), nextId());
    inst->addOperand(std::move(value));
    inst->parent = currentBlock_;
    currentBlock_->instructions.push_back(inst);
}

void IRGenerator::emitReturnVoid() {
    if (!currentBlock_ || currentBlock_->isTerminated()) return;
    auto inst = std::make_shared<Instruction>(Opcode::ReturnVoid, IRType::Void, "ret" + std::to_string(nextId()), nextId());
    inst->parent = currentBlock_;
    currentBlock_->instructions.push_back(inst);
}

void IRGenerator::emitStore(ValueRef addr, ValueRef value) {
    auto inst = std::make_shared<Instruction>(Opcode::Store, IRType::Void, "st" + std::to_string(nextId()), nextId());
    inst->addOperand(std::move(addr));
    inst->addOperand(std::move(value));
    inst->parent = currentBlock_;
    if (currentBlock_) currentBlock_->instructions.push_back(inst);
}

ValueRef IRGenerator::emitLoad(ValueRef addr, IRType type) {
    auto inst = std::make_shared<Instruction>(Opcode::Load, type, "t" + std::to_string(nextId()), nextId());
    inst->addOperand(std::move(addr));
    inst->parent = currentBlock_;
    if (currentBlock_) currentBlock_->instructions.push_back(inst);
    return inst;
}

ValueRef IRGenerator::emitAlloca(IRType type, const std::string& name) {
    // The alloca's type field stores what type of value it holds (for codegen)
    auto inst = std::make_shared<Instruction>(Opcode::Alloca, type, name, nextId());
    inst->parent = currentBlock_;
    if (currentBlock_) currentBlock_->instructions.push_back(inst);
    return inst;
}

ValueRef IRGenerator::makeConstInt(int64_t val, IRType type) {
    return std::make_shared<ConstantInt>(val, type, nextId());
}

ValueRef IRGenerator::makeConstFloat(double val, IRType type) {
    return std::make_shared<ConstantFloat>(val, type, nextId());
}

ValueRef IRGenerator::makeConstBool(bool val) {
    return std::make_shared<ConstantBool>(val, nextId());
}

ValueRef IRGenerator::makeConstString(const std::string& val) {
    auto cs = std::make_shared<ConstantString>(val, nextId());
    module_->stringPool.push_back(cs);
    return cs;
}

ValueRef IRGenerator::makeConstNull() {
    return std::make_shared<ConstantNull>(nextId());
}

void IRGenerator::pushScope() {
    varScopes_.emplace_back();
}

void IRGenerator::popScope() {
    if (!varScopes_.empty()) varScopes_.pop_back();
}

void IRGenerator::declareVar(const std::string& name, ValueRef addr) {
    if (!varScopes_.empty()) {
        varScopes_.back()[name] = std::move(addr);
    }
}

ValueRef IRGenerator::lookupVar(const std::string& name) {
    for (int i = static_cast<int>(varScopes_.size()) - 1; i >= 0; i--) {
        auto it = varScopes_[i].find(name);
        if (it != varScopes_[i].end()) return it->second;
    }
    return nullptr;
}

// --- Statement lowering ---

void IRGenerator::lowerStatement(Statement* stmt) {
    if (!stmt) return;
    currentSourceLine_ = stmt->location.line;
    currentSourceColumn_ = stmt->location.column;

    switch (stmt->kind) {
        case StmtKind::Block: lowerBlock(static_cast<BlockStmt*>(stmt)); break;
        case StmtKind::VarDeclaration: lowerVarDeclaration(static_cast<VarDeclarationStmt*>(stmt)); break;
        case StmtKind::FunctionDeclaration: lowerFunctionDecl(static_cast<FunctionDeclStmt*>(stmt)); break;
        case StmtKind::Class: lowerClassDecl(static_cast<ClassDeclStmt*>(stmt)); break;
        case StmtKind::Enum: lowerEnumDecl(static_cast<EnumDeclStmt*>(stmt)); break;
        case StmtKind::Return: lowerReturn(static_cast<ReturnStmt*>(stmt)); break;
        case StmtKind::If: lowerIf(static_cast<IfStmt*>(stmt)); break;
        case StmtKind::For: lowerFor(static_cast<ForStmt*>(stmt)); break;
        case StmtKind::ForEach: lowerForEach(static_cast<ForEachStmt*>(stmt)); break;
        case StmtKind::While: lowerWhile(static_cast<WhileStmt*>(stmt)); break;
        case StmtKind::DoWhile: lowerDoWhile(static_cast<DoWhileStmt*>(stmt)); break;
        case StmtKind::Switch: lowerSwitch(static_cast<SwitchStmt*>(stmt)); break;
        case StmtKind::Match: lowerMatch(static_cast<MatchStmt*>(stmt)); break;
        case StmtKind::Throw: lowerThrow(static_cast<ThrowStmt*>(stmt)); break;
        case StmtKind::TryCatch: lowerTryCatch(static_cast<TryCatchStmt*>(stmt)); break;
        case StmtKind::Expression: lowerExpressionStmt(static_cast<ExpressionStmt*>(stmt)); break;
        case StmtKind::Export: lowerExport(static_cast<ExportStmt*>(stmt)); break;
        case StmtKind::Import: lowerImport(static_cast<ImportStmt*>(stmt)); break;
        case StmtKind::BlockchainContract: lowerBlockchainContract(static_cast<BlockchainContractStmt*>(stmt)); break;
        case StmtKind::Break:
            if (!loopStack_.empty()) emitBranch(loopStack_.back().breakTarget);
            break;
        case StmtKind::Continue:
            if (!loopStack_.empty()) emitBranch(loopStack_.back().continueTarget);
            break;
        default: break;
    }
}

void IRGenerator::lowerBlock(BlockStmt* stmt) {
    pushScope();
    for (auto& s : stmt->statements) {
        lowerStatement(s.get());
    }
    popScope();
}

void IRGenerator::lowerVarDeclaration(VarDeclarationStmt* stmt) {
    IRType type = IRType::Int32; // default to int
    if (stmt->type) {
        type = mapTypeAnnotation(stmt->type.get());
    } else if (stmt->initializer) {
        // Infer type from initializer
        if (dynamic_cast<NewExpr*>(stmt->initializer.get())) {
            type = IRType::Pointer;
        } else if (dynamic_cast<StringLiteralExpr*>(stmt->initializer.get()) ||
                   dynamic_cast<TemplateLiteralExpr*>(stmt->initializer.get())) {
            type = IRType::String;
        } else if (dynamic_cast<ArrayExpr*>(stmt->initializer.get())) {
            type = IRType::Array;
        } else if (dynamic_cast<MapExpr*>(stmt->initializer.get())) {
            type = IRType::Map;
        } else if (dynamic_cast<NullLiteralExpr*>(stmt->initializer.get())) {
            type = IRType::Pointer;
        } else if (dynamic_cast<BoolLiteralExpr*>(stmt->initializer.get())) {
            type = IRType::Bool;
        } else if (dynamic_cast<FloatLiteralExpr*>(stmt->initializer.get()) ||
                   dynamic_cast<DoubleLiteralExpr*>(stmt->initializer.get())) {
            type = IRType::Float64;
        } else if (auto* call = dynamic_cast<CallExpr*>(stmt->initializer.get())) {
            // If calling a member like Promise.all, result is a pointer/array
            if (auto* member = dynamic_cast<MemberAccessExpr*>(call->callee.get())) {
                if (member->member == "all" || member->member == "create") {
                    type = IRType::Pointer;
                }
            }
        }
        // CallExpr results could be int or pointer — default to int
    }

    auto addr = emitAlloca(type, stmt->name);
    declareVar(stmt->name, addr);

    // Track const variables
    if (stmt->declKind == VarDeclKind::Const || stmt->declKind == VarDeclKind::Readonly) {
        constVars_.insert(stmt->name);
    }

    if (stmt->initializer) {
        lastLambdaName_.clear();
        auto val = lowerExpression(stmt->initializer.get());
        if (val) emitStore(addr, val);
        // Track lambda assignments: const hello = function() { ... } or () => { ... }
        if (!lastLambdaName_.empty()) {
            lambdaNames_[stmt->name] = lastLambdaName_;
        }
    }
}

void IRGenerator::lowerFunctionDecl(FunctionDeclStmt* stmt) {
    IRType retType = IRType::Void;
    if (stmt->returnType) retType = mapTypeAnnotation(stmt->returnType.get());

    auto* fn = module_->createFunction(stmt->name, retType);
    fn->isAsync = stmt->isAsync;

    // Parameters
    for (size_t i = 0; i < stmt->params.size(); i++) {
        auto& param = stmt->params[i];
        IRType pType = param.type ? mapTypeAnnotation(param.type.get()) : IRType::Pointer;
        if (param.isRest) pType = IRType::Array;
        auto p = std::make_shared<Parameter>(pType, param.name, static_cast<int>(fn->params.size()), nextId());
        fn->params.push_back(p);
        if (param.isRest) {
            fn->hasRestParam = true;
            fn->restParamIndex = (uint8_t)i;
        }
    }

    // Save state
    auto* savedFn = currentFunction_;
    auto* savedBlock = currentBlock_;

    currentFunction_ = fn;
    auto* entry = createBlock("entry");
    setInsertPoint(entry);
    pushScope();

    // Declare params as local variables
    for (auto& p : fn->params) {
        auto addr = emitAlloca(p->type, p->name);
        emitStore(addr, p);
        declareVar(p->name, addr);
    }

    // Lower body
    for (auto& s : stmt->body) {
        lowerStatement(s.get());
    }

    // Ensure function is terminated
    if (currentBlock_ && !currentBlock_->isTerminated()) {
        if (retType == IRType::Void) {
            emitReturnVoid();
        } else {
            emitReturn(makeConstInt(0)); // default return
        }
    }

    popScope();

    // Restore state
    currentFunction_ = savedFn;
    currentBlock_ = savedBlock;
}

void IRGenerator::lowerClassDecl(ClassDeclStmt* stmt) {
    Module::ClassInfo info;
    info.name = stmt->name;
    info.baseClass = stmt->baseClass;

    for (auto& field : stmt->fields) {
        info.fieldNames.push_back(field.name);
        info.fieldTypes.push_back(field.type ? mapTypeAnnotation(field.type.get()) : IRType::Pointer);
        // Store original type name for enum detection in table creation
        std::string origTypeName = "";
        if (field.type) {
            if (auto* named = dynamic_cast<NamedType*>(field.type.get())) {
                origTypeName = named->name;
            } else if (auto* generic = dynamic_cast<GenericType*>(field.type.get())) {
                origTypeName = generic->name;
            }
        }
        info.fieldTypeNames.push_back(origTypeName);
    }

    // Detect overloaded methods (same name, different arity)
    std::unordered_map<std::string, int> methodNameCount;
    for (auto& method : stmt->methods) methodNameCount[method.name]++;

    // Push class info early so methods can look up field names during lowering
    module_->classes.push_back(info);
    size_t classIdx = module_->classes.size() - 1;

    for (auto& method : stmt->methods) {
        info.methodNames.push_back(method.name);

        // Generate method as a function: ClassName.methodName or ClassName.methodName$arity
        std::string methodFnName = stmt->name + "." + method.name;
        // If overloaded, mangle with arity (param count excluding 'this')
        if (methodNameCount[method.name] > 1) {
            methodFnName += "$" + std::to_string(method.params.size());
        }
        IRType retType = method.returnType ? mapTypeAnnotation(method.returnType.get()) : IRType::Void;

        auto* fn = module_->createFunction(methodFnName, retType);
        fn->isAsync = method.isAsync;

        // 'this' as first parameter
        auto thisParam = std::make_shared<Parameter>(IRType::Pointer, "this", 0, nextId());
        fn->params.push_back(thisParam);

        for (auto& param : method.params) {
            IRType pType = param.type ? mapTypeAnnotation(param.type.get()) : IRType::Pointer;
            if (param.isRest) pType = IRType::Array;
            auto p = std::make_shared<Parameter>(pType, param.name, static_cast<int>(fn->params.size()), nextId());
            fn->params.push_back(p);
            if (param.isRest) {
                fn->hasRestParam = true;
                fn->restParamIndex = (uint8_t)(fn->params.size() - 1); // account for 'this'
            }
        }

        if (!method.isAbstract) {
            auto* savedFn = currentFunction_;
            auto* savedBlock = currentBlock_;
            currentFunction_ = fn;
            auto* entry = createBlock("entry");
            setInsertPoint(entry);
            pushScope();

            for (auto& p : fn->params) {
                auto addr = emitAlloca(p->type, p->name);
                emitStore(addr, p);
                declareVar(p->name, addr);
            }

            for (auto& s : method.body) {
                lowerStatement(s.get());
            }

            if (currentBlock_ && !currentBlock_->isTerminated()) {
                if (retType == IRType::Void) emitReturnVoid();
                else emitReturn(makeConstInt(0));
            }

            popScope();
            currentFunction_ = savedFn;
            currentBlock_ = savedBlock;
        }
    }

    // Store annotation metadata for runtime reflection
    for (auto& ann : stmt->annotations) {
        Module::ClassInfo::AnnotationMeta meta;
        meta.target = ""; // class-level
        meta.name = ann.name;
        for (auto& arg : ann.args) meta.args.push_back({arg.key, arg.value});
        info.annotations.push_back(std::move(meta));
    }
    for (auto& method : stmt->methods) {
        for (auto& ann : method.annotations) {
            Module::ClassInfo::AnnotationMeta meta;
            meta.target = method.name; // method-level
            meta.name = ann.name;
            for (auto& arg : ann.args) meta.args.push_back({arg.key, arg.value});
            info.annotations.push_back(std::move(meta));
        }
    }
    for (auto& field : stmt->fields) {
        for (auto& ann : field.annotations) {
            Module::ClassInfo::AnnotationMeta meta;
            meta.target = field.name; // field-level
            meta.name = ann.name;
            for (auto& arg : ann.args) meta.args.push_back({arg.key, arg.value});
            info.annotations.push_back(std::move(meta));
        }
    }

    // Store interface implementation metadata for 'is' type checking
    if (!stmt->interfaces.empty()) {
        Module::ClassInfo::AnnotationMeta implMeta;
        implMeta.target = "";
        implMeta.name = "_implements";
        for (auto& iface : stmt->interfaces) {
            implMeta.args.push_back({iface, "true"});
        }
        info.annotations.push_back(std::move(implMeta));
    }

    // Store base class for inheritance-based 'is' checking
    if (!stmt->baseClass.empty()) {
        Module::ClassInfo::AnnotationMeta extMeta;
        extMeta.target = "";
        extMeta.name = "_extends";
        extMeta.args.push_back({stmt->baseClass, "true"});
        info.annotations.push_back(std::move(extMeta));
    }

    // Update the early-pushed class info with complete data (methods, annotations)
    module_->classes[classIdx] = std::move(info);

    // Generate constructor function: ClassName.constructor
    if (stmt->constructor) {
        std::string ctorName = stmt->name + ".constructor";
        auto* fn = module_->createFunction(ctorName, IRType::Void);

        auto thisParam = std::make_shared<Parameter>(IRType::Pointer, "this", 0, nextId());
        fn->params.push_back(thisParam);

        for (auto& param : stmt->constructor->params) {
            IRType pType = param.type ? mapTypeAnnotation(param.type.get()) : IRType::Pointer;
            auto p = std::make_shared<Parameter>(pType, param.name, static_cast<int>(fn->params.size()), nextId());
            fn->params.push_back(p);
        }

        auto* savedFn = currentFunction_;
        auto* savedBlock = currentBlock_;
        currentFunction_ = fn;
        auto* entry = createBlock("entry");
        setInsertPoint(entry);
        pushScope();

        for (auto& p : fn->params) {
            auto addr = emitAlloca(p->type, p->name);
            emitStore(addr, p);
            declareVar(p->name, addr);
        }

        // Initialize fields with default values (before constructor body)
        for (auto& field : stmt->fields) {
            if (field.initializer) {
                auto thisAddr = lookupVar("this");
                if (thisAddr) {
                    auto thisVal = emitLoad(thisAddr, IRType::Pointer);
                    auto initVal = lowerExpression(field.initializer.get());
                    auto setInst = std::make_shared<Instruction>(Opcode::SetField, IRType::Void, "sf" + std::to_string(nextId()), nextId());
                    setInst->addOperand(thisVal);
                    setInst->addOperand(initVal);
                    setInst->targetName = field.name;
                    if (currentBlock_) currentBlock_->instructions.push_back(setInst);
                }
            }
        }

        for (auto& s : stmt->constructor->body) {
            lowerStatement(s.get());
        }

        if (currentBlock_ && !currentBlock_->isTerminated()) emitReturnVoid();

        popScope();
        currentFunction_ = savedFn;
        currentBlock_ = savedBlock;
    }
}

void IRGenerator::lowerEnumDecl(EnumDeclStmt* stmt) {
    // Enums are compiled as a class with static final fields for each variant.
    // Each variant is an object with _name (string), _ordinal (int), and any custom fields.
    // The enum type itself is registered as a class so `new EnumName()` is blocked at runtime.

    Module::ClassInfo info;
    info.name = stmt->name;

    // Add internal fields: _name, _ordinal
    info.fieldNames.push_back("_name");
    info.fieldTypes.push_back(IRType::String);
    info.fieldNames.push_back("_ordinal");
    info.fieldTypes.push_back(IRType::Int32);

    // Add user-defined fields
    for (auto& field : stmt->fields) {
        info.fieldNames.push_back(field.name);
        info.fieldTypes.push_back(field.type ? mapTypeAnnotation(field.type.get()) : IRType::Pointer);
    }

    // Store annotations
    for (auto& ann : stmt->annotations) {
        Module::ClassInfo::AnnotationMeta meta;
        meta.target = "";
        meta.name = ann.name;
        for (auto& arg : ann.args) meta.args.push_back({arg.key, arg.value});
        info.annotations.push_back(std::move(meta));
    }

    // Add a special _Enum annotation so runtime knows this is an enum
    Module::ClassInfo::AnnotationMeta enumMeta;
    enumMeta.target = "";
    enumMeta.name = "_Enum";
    // Store variant names as args
    for (size_t i = 0; i < stmt->variants.size(); i++) {
        enumMeta.args.push_back({stmt->variants[i].name, std::to_string(i)});
    }
    info.annotations.push_back(std::move(enumMeta));

    // Generate methods
    for (auto& method : stmt->methods) {
        info.methodNames.push_back(method.name);
        std::string methodFnName = stmt->name + "." + method.name;
        IRType retType = method.returnType ? mapTypeAnnotation(method.returnType.get()) : IRType::Void;
        auto* fn = module_->createFunction(methodFnName, retType);
        fn->isAsync = method.isAsync;

        auto thisParam = std::make_shared<Parameter>(IRType::Pointer, "this", 0, nextId());
        fn->params.push_back(thisParam);
        for (auto& param : method.params) {
            IRType pType = param.type ? mapTypeAnnotation(param.type.get()) : IRType::Pointer;
            auto p = std::make_shared<Parameter>(pType, param.name, static_cast<int>(fn->params.size()), nextId());
            fn->params.push_back(p);
        }

        auto* savedFn = currentFunction_;
        auto* savedBlock = currentBlock_;
        currentFunction_ = fn;
        auto* entry = createBlock("entry");
        setInsertPoint(entry);
        pushScope();
        for (auto& p : fn->params) {
            auto addr = emitAlloca(p->type, p->name);
            emitStore(addr, p);
            declareVar(p->name, addr);
        }
        for (auto& s : method.body) lowerStatement(s.get());
        if (currentBlock_ && !currentBlock_->isTerminated()) {
            if (retType == IRType::Void) emitReturnVoid();
            else emitReturn(makeConstInt(0));
        }
        popScope();
        currentFunction_ = savedFn;
        currentBlock_ = savedBlock;
    }

    module_->classes.push_back(std::move(info));
}

void IRGenerator::lowerReturn(ReturnStmt* stmt) {
    if (stmt->value) {
        auto val = lowerExpression(stmt->value.get());
        emitReturn(val);
    } else {
        emitReturnVoid();
    }
}

void IRGenerator::lowerIf(IfStmt* stmt) {
    auto cond = lowerExpression(stmt->condition.get());

    auto* thenBlock = createBlock(nextLabel("if.then"));
    auto* elseBlock = stmt->elseBranch ? createBlock(nextLabel("if.else")) : nullptr;
    auto* mergeBlock = createBlock(nextLabel("if.end"));

    emitCondBranch(cond, thenBlock, elseBlock ? elseBlock : mergeBlock);

    // Then
    setInsertPoint(thenBlock);
    lowerStatement(stmt->thenBranch.get());
    if (!currentBlock_->isTerminated()) emitBranch(mergeBlock);

    // Else
    if (stmt->elseBranch) {
        setInsertPoint(elseBlock);
        lowerStatement(stmt->elseBranch.get());
        if (!currentBlock_->isTerminated()) emitBranch(mergeBlock);
    }

    setInsertPoint(mergeBlock);
}

void IRGenerator::lowerFor(ForStmt* stmt) {
    auto* condBlock = createBlock(nextLabel("for.cond"));
    auto* bodyBlock = createBlock(nextLabel("for.body"));
    auto* incBlock = createBlock(nextLabel("for.inc"));
    auto* endBlock = createBlock(nextLabel("for.end"));

    pushScope();
    loopStack_.push_back({endBlock, incBlock});

    // Init
    if (stmt->initializer) lowerStatement(stmt->initializer.get());
    emitBranch(condBlock);

    // Condition
    setInsertPoint(condBlock);
    if (stmt->condition) {
        auto cond = lowerExpression(stmt->condition.get());
        emitCondBranch(cond, bodyBlock, endBlock);
    } else {
        emitBranch(bodyBlock);
    }

    // Body
    setInsertPoint(bodyBlock);
    if (stmt->body) lowerStatement(stmt->body.get());
    if (!currentBlock_->isTerminated()) emitBranch(incBlock);

    // Increment
    setInsertPoint(incBlock);
    if (stmt->increment) lowerExpression(stmt->increment.get());
    emitBranch(condBlock);

    loopStack_.pop_back();
    popScope();
    setInsertPoint(endBlock);
}

void IRGenerator::lowerForEach(ForEachStmt* stmt) {
    // Lower for-each as index-based while loop:
    // let __idx_N = 0;
    // let __len_N = List.length(iterable);
    // while (__idx_N < __len_N) {
    //     let <var> = List.get(iterable, __idx_N);
    //     <body>
    //     __idx_N = __idx_N + 1;
    // }

    // Unique names for nested loops
    int loopId = nextId();
    std::string idxName = "__idx_" + std::to_string(loopId);
    std::string lenName = "__len_" + std::to_string(loopId);

    auto* condBlock = createBlock(nextLabel("foreach.cond"));
    auto* bodyBlock = createBlock(nextLabel("foreach.body"));
    auto* incBlock = createBlock(nextLabel("foreach.inc"));
    auto* endBlock = createBlock(nextLabel("foreach.end"));

    pushScope();
    loopStack_.push_back({endBlock, incBlock});

    // Evaluate iterable (the array/collection)
    auto iterVal = lowerExpression(stmt->iterable.get());

    // __idx_N = 0
    auto idxAddr = emitAlloca(IRType::Int32, idxName);
    auto zero = makeConstInt(0);
    emitStore(idxAddr, zero);

    // __len_N = List.length(iterable)
    auto lenInst = std::make_shared<ir::Instruction>(ir::Opcode::CallVirtual, IRType::Int32, "t" + std::to_string(nextId()), nextId());
    lenInst->targetName = "List.length";
    lenInst->addOperand(makeConstString("List"));
    lenInst->addOperand(iterVal);
    currentBlock_->instructions.push_back(lenInst);
    auto lenVal = std::static_pointer_cast<ir::Value>(lenInst);

    auto lenAddr = emitAlloca(IRType::Int32, lenName);
    emitStore(lenAddr, lenVal);

    // Branch to condition
    emitBranch(condBlock);
    setInsertPoint(condBlock);

    // __idx_N < __len_N
    auto idx = emitLoad(idxAddr, IRType::Int32);
    auto len = emitLoad(lenAddr, IRType::Int32);
    auto cond = emit(ir::Opcode::CmpLt, IRType::Bool, {idx, len});
    emitCondBranch(cond, bodyBlock, endBlock);

    // Body
    setInsertPoint(bodyBlock);

    // let <var> = List.get(iterable, __idx_N)
    auto bodyIdx = emitLoad(idxAddr, IRType::Int32);
    auto getInst = std::make_shared<ir::Instruction>(ir::Opcode::CallVirtual, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
    getInst->targetName = "List.get";
    getInst->addOperand(makeConstString("List"));
    getInst->addOperand(iterVal);
    getInst->addOperand(bodyIdx);
    currentBlock_->instructions.push_back(getInst);
    auto elemVal = std::static_pointer_cast<ir::Value>(getInst);

    // Declare the loop variable
    auto varAddr = emitAlloca(IRType::Pointer, stmt->variable);
    emitStore(varAddr, elemVal);
    declareVar(stmt->variable, varAddr);

    // Execute body
    if (stmt->body) lowerStatement(stmt->body.get());
    if (!currentBlock_->isTerminated()) emitBranch(incBlock);

    // Increment
    setInsertPoint(incBlock);
    auto curIdx = emitLoad(idxAddr, IRType::Int32);
    auto one = makeConstInt(1);
    auto nextIdx = emit(ir::Opcode::Add, IRType::Int32, {curIdx, one});
    emitStore(idxAddr, nextIdx);
    emitBranch(condBlock);

    loopStack_.pop_back();
    popScope();
    setInsertPoint(endBlock);
}

void IRGenerator::lowerWhile(WhileStmt* stmt) {
    auto* condBlock = createBlock(nextLabel("while.cond"));
    auto* bodyBlock = createBlock(nextLabel("while.body"));
    auto* endBlock = createBlock(nextLabel("while.end"));

    loopStack_.push_back({endBlock, condBlock});
    emitBranch(condBlock);

    setInsertPoint(condBlock);
    auto cond = lowerExpression(stmt->condition.get());
    emitCondBranch(cond, bodyBlock, endBlock);

    setInsertPoint(bodyBlock);
    if (stmt->body) lowerStatement(stmt->body.get());
    if (!currentBlock_->isTerminated()) emitBranch(condBlock);

    loopStack_.pop_back();
    setInsertPoint(endBlock);
}

void IRGenerator::lowerDoWhile(DoWhileStmt* stmt) {
    auto* bodyBlock = createBlock(nextLabel("do.body"));
    auto* condBlock = createBlock(nextLabel("do.cond"));
    auto* endBlock = createBlock(nextLabel("do.end"));

    loopStack_.push_back({endBlock, condBlock});
    emitBranch(bodyBlock);

    setInsertPoint(bodyBlock);
    if (stmt->body) lowerStatement(stmt->body.get());
    if (!currentBlock_->isTerminated()) emitBranch(condBlock);

    setInsertPoint(condBlock);
    auto cond = lowerExpression(stmt->condition.get());
    emitCondBranch(cond, bodyBlock, endBlock);

    loopStack_.pop_back();
    setInsertPoint(endBlock);
}

void IRGenerator::lowerSwitch(SwitchStmt* stmt) {
    auto disc = lowerExpression(stmt->discriminant.get());
    auto* endBlock = createBlock(nextLabel("switch.end"));

    loopStack_.push_back({endBlock, nullptr}); // break target

    BasicBlock* defaultBlock = endBlock;
    std::vector<std::pair<ValueRef, BasicBlock*>> cases;

    for (auto& sc : stmt->cases) {
        auto* caseBlock = createBlock(nextLabel("switch.case"));
        if (sc.isDefault) {
            defaultBlock = caseBlock;
        } else {
            auto val = lowerExpression(sc.value.get());
            cases.push_back({val, caseBlock});
        }
    }

    // Emit cascading conditional branches
    for (auto& [val, block] : cases) {
        auto cmp = emit(Opcode::CmpEq, IRType::Bool, {disc, val});
        auto* nextCheck = createBlock(nextLabel("switch.next"));
        emitCondBranch(cmp, block, nextCheck);
        setInsertPoint(nextCheck);
    }
    emitBranch(defaultBlock);

    // Lower case bodies
    size_t caseIdx = 0;
    for (auto& sc : stmt->cases) {
        BasicBlock* caseBlock;
        if (sc.isDefault) {
            caseBlock = defaultBlock;
        } else {
            caseBlock = cases[caseIdx++].second;
        }
        setInsertPoint(caseBlock);
        for (auto& s : sc.body) {
            lowerStatement(s.get());
        }
        if (!currentBlock_->isTerminated()) emitBranch(endBlock);
    }

    loopStack_.pop_back();
    setInsertPoint(endBlock);
}

void IRGenerator::lowerMatch(MatchStmt* stmt) {
    auto val = lowerExpression(stmt->value.get());
    auto* endBlock = createBlock(nextLabel("match.end"));

    for (auto& arm : stmt->arms) {
        if (arm.isDefault) {
            // Default/wildcard arm — always executes (no comparison)
            lowerExpression(arm.body.get());
            if (!currentBlock_->isTerminated()) emitBranch(endBlock);
            break; // default must be last
        }

        auto pattern = lowerExpression(arm.pattern.get());
        auto cmp = emit(Opcode::CmpEq, IRType::Bool, {val, pattern});
        auto* armBlock = createBlock(nextLabel("match.arm"));
        auto* nextBlock = createBlock(nextLabel("match.next"));
        emitCondBranch(cmp, armBlock, nextBlock);

        setInsertPoint(armBlock);
        lowerExpression(arm.body.get());
        if (!currentBlock_->isTerminated()) emitBranch(endBlock);

        setInsertPoint(nextBlock);
    }
    if (!currentBlock_->isTerminated()) emitBranch(endBlock);
    setInsertPoint(endBlock);
}

void IRGenerator::lowerThrow(ThrowStmt* stmt) {
    ValueRef throwVal = makeConstNull();
    if (stmt->value) throwVal = lowerExpression(stmt->value.get());
    // Emit throw instruction
    emit(Opcode::Throw, IRType::Void, {throwVal});
}

void IRGenerator::lowerTryCatch(TryCatchStmt* stmt) {
    auto* catchBlock = createBlock(nextLabel("catch"));
    auto* finallyBlock = !stmt->finallyBody.empty() ? createBlock(nextLabel("finally")) : nullptr;
    auto* endBlock = createBlock(nextLabel("try.end"));

    // Emit TryBegin — register catch block as exception handler
    auto tryBeginInst = std::make_shared<Instruction>(Opcode::TryBegin, IRType::Void, "try" + std::to_string(nextId()), nextId());
    tryBeginInst->jumpTarget = catchBlock;
    tryBeginInst->parent = currentBlock_;
    if (currentBlock_) currentBlock_->instructions.push_back(tryBeginInst);

    // Try body
    for (auto& s : stmt->tryBody) lowerStatement(s.get());

    // TryEnd — pop exception handler (try completed normally)
    emit(Opcode::TryEnd, IRType::Void, {});
    emitBranch(finallyBlock ? finallyBlock : endBlock);

    // Catch block
    setInsertPoint(catchBlock);
    pushScope();
    for (auto& clause : stmt->catchClauses) {
        // The exception value is on the stack (pushed by VM's throw handler)
        auto addr = emitAlloca(IRType::Pointer, clause.paramName);
        declareVar(clause.paramName, addr);
        // Load the caught exception value and store it in the catch variable
        auto catchInst = std::make_shared<Instruction>(Opcode::CatchException, IRType::Pointer, clause.paramName, nextId());
        catchInst->parent = currentBlock_;
        if (currentBlock_) currentBlock_->instructions.push_back(catchInst);
        // Store the exception value into the catch variable's alloca
        emitStore(addr, catchInst);
        for (auto& s : clause.body) lowerStatement(s.get());
    }
    popScope();
    if (!currentBlock_->isTerminated()) emitBranch(finallyBlock ? finallyBlock : endBlock);

    // Finally block
    if (finallyBlock) {
        setInsertPoint(finallyBlock);
        for (auto& s : stmt->finallyBody) lowerStatement(s.get());
        if (!currentBlock_->isTerminated()) emitBranch(endBlock);
    }

    setInsertPoint(endBlock);
}

void IRGenerator::lowerExpressionStmt(ExpressionStmt* stmt) {
    if (stmt->expression) lowerExpression(stmt->expression.get());
}

void IRGenerator::lowerExport(ExportStmt* stmt) {
    if (stmt->declaration) {
        lowerStatement(stmt->declaration.get());
        // Mark the generated function as exported
        if (auto* fn = dynamic_cast<FunctionDeclStmt*>(stmt->declaration.get())) {
            for (auto& f : module_->functions) {
                if (f->name == fn->name) { f->isExported = true; break; }
            }
        }
    }
}

void IRGenerator::lowerImport(ImportStmt* stmt) {
    // Skip stdlib imports (e.g., "gard/core", "gard/net") — handled by runtime linking
    if (stmt->path.find("gard/") == 0 || stmt->path.find("\"gard/") == 0) return;

    // Resolve relative path
    std::string importPath = stmt->path;
    // Remove quotes if present
    if (!importPath.empty() && importPath.front() == '"') importPath = importPath.substr(1);
    if (!importPath.empty() && importPath.back() == '"') importPath.pop_back();

    // Skip stdlib paths
    if (importPath.find("gard/") == 0) return;

    // Resolve relative to current module's directory
    std::filesystem::path currentDir;
    if (!module_->name.empty() && module_->name != "<stdin>") {
        currentDir = std::filesystem::path(module_->name).parent_path();
    } else {
        currentDir = std::filesystem::current_path();
    }

    std::filesystem::path resolvedPath = currentDir / importPath;
    // Add .gard extension if not present
    if (resolvedPath.extension() != ".gard") {
        resolvedPath += ".gard";
    }

    std::string absPath = std::filesystem::absolute(resolvedPath).string();

    // Prevent circular imports
    if (importedFiles_.count(absPath)) return;
    importedFiles_.insert(absPath);

    // Read the file
    std::ifstream file(absPath);
    if (!file.is_open()) {
        errors_.push_back("Cannot open imported file: " + absPath);
        return;
    }
    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // Lex and parse the imported file
    Lexer lexer(source, absPath);
    auto tokens = lexer.tokenize();
    if (lexer.hasErrors()) {
        for (auto& err : lexer.getErrors()) errors_.push_back("Import error: " + err);
        return;
    }

    Parser parser(tokens, absPath);
    auto program = parser.parse();
    if (parser.hasErrors()) {
        for (auto& err : parser.getErrors()) errors_.push_back("Import error: " + err);
        return;
    }

    // Generate IR for the imported module
    IRGenerator importGen;
    importGen.importedFiles_ = importedFiles_; // share circular import tracking
    auto importedModule = importGen.generate(program);

    // Update our circular import set with any files the imported module found
    for (auto& f : importGen.importedFiles_) importedFiles_.insert(f);

    if (!importedModule) return;

    // Merge imported module into current module
    // Only merge exported functions and classes that match requested symbols
    std::unordered_set<std::string> requestedNames(stmt->names.begin(), stmt->names.end());
    bool importAll = requestedNames.empty(); // empty names = import all (wildcard)

    // Merge functions
    // Import all functions from the imported module — the linker will strip unused ones.
    // We need transitive dependencies (e.g., if we import sumOfSquares which calls square,
    // we need square too).
    for (auto& fn : importedModule->functions) {
        // Check if this function matches a requested name or is a dependency
        bool nameMatches = importAll;
        if (!nameMatches) {
            // Direct name match (exported functions)
            if (fn->isExported) {
                for (auto& name : requestedNames) {
                    if (fn->name == name || fn->name.find(name + ".") == 0 ||
                        fn->name.find(name + "$") != std::string::npos) {
                        nameMatches = true;
                        break;
                    }
                }
            }
            // Also import if this is a method/constructor of a requested class
            if (!nameMatches) {
                size_t dotPos = fn->name.find('.');
                if (dotPos != std::string::npos) {
                    std::string className = fn->name.substr(0, dotPos);
                    if (requestedNames.count(className)) nameMatches = true;
                }
                // Also check for arity-mangled names: ClassName.method$N
                size_t dollarPos = fn->name.find('$');
                if (!nameMatches && dollarPos != std::string::npos) {
                    std::string beforeDollar = fn->name.substr(0, dollarPos);
                    size_t dp = beforeDollar.find('.');
                    if (dp != std::string::npos) {
                        std::string cn = beforeDollar.substr(0, dp);
                        if (requestedNames.count(cn)) nameMatches = true;
                    }
                }
            }
        }
        if (!nameMatches) continue;

        // Check for duplicate function names
        bool exists = false;
        for (auto& existing : module_->functions) {
            if (existing->name == fn->name) { exists = true; break; }
        }
        if (!exists) {
            module_->functions.push_back(std::move(fn));
        }
    }

    // Second pass: import any remaining functions that are dependencies of already-imported ones
    // (functions called by imported functions that weren't directly requested)
    for (auto& fn : importedModule->functions) {
        if (!fn) continue; // already moved
        bool exists = false;
        for (auto& existing : module_->functions) {
            if (existing->name == fn->name) { exists = true; break; }
        }
        if (!exists) {
            module_->functions.push_back(std::move(fn));
        }
    }

    // Merge classes
    for (auto& cls : importedModule->classes) {
        bool nameMatches = importAll;
        if (!nameMatches) {
            nameMatches = requestedNames.count(cls.name) > 0;
        }
        if (!nameMatches) continue;

        bool exists = false;
        for (auto& existing : module_->classes) {
            if (existing.name == cls.name) { exists = true; break; }
        }
        if (!exists) {
            module_->classes.push_back(std::move(cls));
            // Also import all methods of this class
            // (they were already moved with functions above if exported)
        }
    }

    // Merge string pool
    for (auto& str : importedModule->stringPool) {
        module_->stringPool.push_back(std::move(str));
    }
}

void IRGenerator::lowerBlockchainContract(BlockchainContractStmt* stmt) {
    // Lower as a class
    Module::ClassInfo info;
    info.name = stmt->name;
    for (auto& field : stmt->fields) {
        info.fieldNames.push_back(field.name);
        info.fieldTypes.push_back(field.type ? mapTypeAnnotation(field.type.get()) : IRType::Pointer);
    }
    for (auto& method : stmt->methods) {
        info.methodNames.push_back(method.name);
        // Generate method function
        std::string methodFnName = stmt->name + "." + method.name;
        IRType retType = method.returnType ? mapTypeAnnotation(method.returnType.get()) : IRType::Void;
        auto* fn = module_->createFunction(methodFnName, retType);

        auto* savedFn = currentFunction_;
        auto* savedBlock = currentBlock_;
        currentFunction_ = fn;
        auto* entry = createBlock("entry");
        setInsertPoint(entry);
        pushScope();

        for (auto& s : method.body) lowerStatement(s.get());
        if (currentBlock_ && !currentBlock_->isTerminated()) {
            if (retType == IRType::Void) emitReturnVoid();
            else emitReturn(makeConstInt(0));
        }

        popScope();
        currentFunction_ = savedFn;
        currentBlock_ = savedBlock;
    }
    module_->classes.push_back(std::move(info));
}

// --- Expression lowering ---

ValueRef IRGenerator::lowerExpression(Expression* expr) {
    if (!expr) return makeConstNull();

    // Update source location from expression for debug/error reporting
    if (expr->location.line > 0) {
        currentSourceLine_ = expr->location.line;
        currentSourceColumn_ = expr->location.column;
    }

    switch (expr->kind) {
        case ExprKind::IntLiteral: {
            auto* lit = static_cast<IntLiteralExpr*>(expr);
            return makeConstInt(std::stoll(lit->value));
        }
        case ExprKind::DoubleLiteral: {
            auto* lit = static_cast<DoubleLiteralExpr*>(expr);
            return makeConstFloat(std::stod(lit->value));
        }
        case ExprKind::FloatLiteral: {
            auto* lit = static_cast<FloatLiteralExpr*>(expr);
            std::string val = lit->value;
            if (val.back() == 'f' || val.back() == 'F') val.pop_back();
            return makeConstFloat(std::stod(val), IRType::Float32);
        }
        case ExprKind::LongLiteral: {
            auto* lit = static_cast<LongLiteralExpr*>(expr);
            std::string val = lit->value;
            if (val.back() == 'L' || val.back() == 'l') val.pop_back();
            return makeConstInt(std::stoll(val), IRType::Int64);
        }
        case ExprKind::HexLiteral: {
            auto* lit = static_cast<HexLiteralExpr*>(expr);
            return makeConstInt(std::stoll(lit->value, nullptr, 16));
        }
        case ExprKind::BinaryLiteral: {
            auto* lit = static_cast<BinaryLiteralExpr*>(expr);
            std::string val = lit->value.substr(2); // skip 0b
            return makeConstInt(std::stoll(val, nullptr, 2));
        }
        case ExprKind::StringLiteral: {
            auto* lit = static_cast<StringLiteralExpr*>(expr);
            return makeConstString(lit->value);
        }
        case ExprKind::TemplateLiteral: {
            auto* lit = static_cast<TemplateLiteralExpr*>(expr);
            const std::string& raw = lit->raw;
            ValueRef result = nullptr;

            size_t pos = 0;
            while (pos < raw.size()) {
                // Look for $ (either ${expr} or $name)
                size_t dollarPos = raw.find('$', pos);
                if (dollarPos == std::string::npos) {
                    // Rest is plain text
                    std::string text = raw.substr(pos);
                    if (!text.empty()) {
                        auto part = makeConstString(text);
                        result = result ? emit(Opcode::Concat, IRType::String, {result, part}) : part;
                    }
                    break;
                }

                // Plain text before $
                if (dollarPos > pos) {
                    std::string text = raw.substr(pos, dollarPos - pos);
                    auto part = makeConstString(text);
                    result = result ? emit(Opcode::Concat, IRType::String, {result, part}) : part;
                }

                if (dollarPos + 1 >= raw.size()) {
                    // Trailing $
                    auto part = makeConstString("$");
                    result = result ? emit(Opcode::Concat, IRType::String, {result, part}) : part;
                    pos = dollarPos + 1;
                    continue;
                }

                if (raw[dollarPos + 1] == '{') {
                    // ${expression} — find matching }
                    size_t exprEnd = raw.find('}', dollarPos + 2);
                    if (exprEnd == std::string::npos) {
                        // Unterminated — treat as literal
                        auto part = makeConstString(raw.substr(dollarPos));
                        result = result ? emit(Opcode::Concat, IRType::String, {result, part}) : part;
                        break;
                    }

                    std::string exprStr = raw.substr(dollarPos + 2, exprEnd - dollarPos - 2);

                    // Try to evaluate: first check if it's a simple variable name
                    ValueRef exprVal = nullptr;
                    auto addr = lookupVar(exprStr);
                    if (addr) {
                        exprVal = emitLoad(addr, IRType::String);
                    } else {
                        // Try to parse as an expression (e.g. "25 + 45")
                        // Lex and parse the expression
                        gard::Lexer exprLexer(exprStr + ";", "<template>");
                        auto exprTokens = exprLexer.tokenize();
                        if (!exprLexer.hasErrors() && exprTokens.size() > 1) {
                            gard::Parser exprParser(exprTokens, "<template>");
                            auto exprProgram = exprParser.parse();
                            if (!exprParser.hasErrors() && !exprProgram.statements.empty()) {
                                if (auto* exprStmt = dynamic_cast<gard::ExpressionStmt*>(exprProgram.statements[0].get())) {
                                    exprVal = lowerExpression(exprStmt->expression.get());
                                }
                            }
                        }
                        if (!exprVal) {
                            // Fallback: use as literal string
                            exprVal = makeConstString(exprStr);
                        }
                    }

                    result = result ? emit(Opcode::Concat, IRType::String, {result, exprVal}) : exprVal;
                    pos = exprEnd + 1;
                } else {
                    // $name — simple variable interpolation (identifier chars: a-z, A-Z, 0-9, _)
                    size_t nameStart = dollarPos + 1;
                    size_t nameEnd = nameStart;
                    while (nameEnd < raw.size() &&
                           (std::isalnum(raw[nameEnd]) || raw[nameEnd] == '_')) {
                        nameEnd++;
                    }

                    if (nameEnd > nameStart) {
                        std::string varName = raw.substr(nameStart, nameEnd - nameStart);
                        auto addr = lookupVar(varName);
                        ValueRef varVal;
                        if (addr) {
                            varVal = emitLoad(addr, IRType::String);
                        } else {
                            varVal = makeConstString("$" + varName); // unresolved: keep literal
                        }
                        result = result ? emit(Opcode::Concat, IRType::String, {result, varVal}) : varVal;
                        pos = nameEnd;
                    } else {
                        // Just a $ followed by non-identifier
                        auto part = makeConstString("$");
                        result = result ? emit(Opcode::Concat, IRType::String, {result, part}) : part;
                        pos = dollarPos + 1;
                    }
                }
            }

            return result ? result : makeConstString("");
        }
        case ExprKind::CharLiteral: {
            auto* lit = static_cast<CharLiteralExpr*>(expr);
            return makeConstInt(lit->value.empty() ? 0 : lit->value[0], IRType::Char);
        }
        case ExprKind::BoolLiteral: {
            auto* lit = static_cast<BoolLiteralExpr*>(expr);
            return makeConstBool(lit->value);
        }
        case ExprKind::NullLiteral:
            return makeConstNull();
        case ExprKind::This:
            return lookupVar("this") ? emitLoad(lookupVar("this"), IRType::Pointer) : makeConstNull();
        case ExprKind::Super:
            return lookupVar("this") ? emitLoad(lookupVar("this"), IRType::Pointer) : makeConstNull();
        case ExprKind::Identifier:
            return lowerIdentifier(static_cast<IdentifierExpr*>(expr));
        case ExprKind::Binary:
            return lowerBinary(static_cast<BinaryExpr*>(expr));
        case ExprKind::Unary:
            return lowerUnary(static_cast<UnaryExpr*>(expr));
        case ExprKind::Call:
            return lowerCall(static_cast<CallExpr*>(expr));
        case ExprKind::MemberAccess:
            return lowerMemberAccess(static_cast<MemberAccessExpr*>(expr));
        case ExprKind::IndexAccess:
            return lowerIndexAccess(static_cast<IndexAccessExpr*>(expr));
        case ExprKind::Assignment:
            return lowerAssignment(static_cast<AssignmentExpr*>(expr));
        case ExprKind::Ternary:
            return lowerTernary(static_cast<TernaryExpr*>(expr));
        case ExprKind::New:
            return lowerNew(static_cast<NewExpr*>(expr));
        case ExprKind::Cast:
            return lowerCast(static_cast<CastExpr*>(expr));
        case ExprKind::Await:
            return lowerAwait(static_cast<AwaitExpr*>(expr));
        case ExprKind::Lambda:
            return lowerLambda(static_cast<LambdaExpr*>(expr));
        case ExprKind::Array:
            return lowerArray(static_cast<ArrayExpr*>(expr));
        case ExprKind::Map:
            return lowerMap(static_cast<MapExpr*>(expr));
        case ExprKind::Set:
            return lowerSet(static_cast<SetExpr*>(expr));
        case ExprKind::Grouped: {
            auto* grouped = static_cast<GroupedExpr*>(expr);
            return lowerExpression(grouped->inner.get());
        }
        case ExprKind::OptionalChain: {
            auto* oc = static_cast<OptionalChainExpr*>(expr);
            auto obj = lowerExpression(oc->object.get());
            auto inst = std::make_shared<Instruction>(Opcode::GetField, IRType::Int32, "t" + std::to_string(nextId()), nextId());
            inst->addOperand(obj);
            inst->targetName = oc->member;
            if (currentBlock_) currentBlock_->instructions.push_back(inst);
            return inst;
        }
        default:
            return makeConstNull();
    }
}

ValueRef IRGenerator::lowerIdentifier(IdentifierExpr* expr) {
    auto addr = lookupVar(expr->name);
    if (addr) {
        return emitLoad(addr, addr->type);
    }
    // Check if this is a class field (accessed via 'this')
    auto thisAddr = lookupVar("this");
    if (thisAddr && module_ && currentFunction_) {
        // Determine class name from function name (format: "ClassName.methodName")
        std::string fnName = currentFunction_->name;
        auto dotPos = fnName.find('.');
        if (dotPos != std::string::npos) {
            std::string className = fnName.substr(0, dotPos);
            for (auto& cls : module_->classes) {
                if (cls.name == className) {
                    for (size_t fi = 0; fi < cls.fieldNames.size(); fi++) {
                        if (cls.fieldNames[fi] == expr->name) {
                            // Emit: this.fieldName (GetField)
                            auto thisVal = emitLoad(thisAddr, IRType::Pointer);
                            auto inst = std::make_shared<Instruction>(Opcode::GetField, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
                            inst->addOperand(thisVal);
                            inst->targetName = expr->name;
                            inst->parent = currentBlock_;
                            if (currentBlock_) currentBlock_->instructions.push_back(inst);
                            return inst;
                        }
                    }
                    break;
                }
            }
        }
    }
    // Check if this is a function name — return function pointer (CreateClosure)
    if (module_) {
        for (auto& fn : module_->functions) {
            if (fn->name == expr->name) {
                auto closureInst = std::make_shared<Instruction>(Opcode::CreateClosure, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
                closureInst->targetName = expr->name;
                closureInst->parent = currentBlock_;
                if (currentBlock_) currentBlock_->instructions.push_back(closureInst);
                return closureInst;
            }
        }
    }
    // Could be a class/enum name used as namespace (e.g., Color.RED, Math.PI)
    // Emit a string constant with the name so GET_FIELD can resolve it
    return makeConstString(expr->name);
}

ValueRef IRGenerator::lowerBinary(BinaryExpr* expr) {
    auto left = lowerExpression(expr->left.get());
    auto right = lowerExpression(expr->right.get());

    Opcode op;
    IRType resultType = IRType::Int32;

    switch (expr->op) {
        case TokenType::Plus: op = Opcode::Add; break;
        case TokenType::Minus: op = Opcode::Sub; break;
        case TokenType::Star: op = Opcode::Mul; break;
        case TokenType::Slash: op = Opcode::Div; break;
        case TokenType::Percent: op = Opcode::Mod; break;
        case TokenType::BitAnd: op = Opcode::BitAnd; break;
        case TokenType::BitOr: op = Opcode::BitOr; break;
        case TokenType::BitXor: op = Opcode::BitXor; break;
        case TokenType::ShiftLeft: op = Opcode::Shl; break;
        case TokenType::ShiftRight: op = Opcode::Shr; break;
        case TokenType::UnsignedShiftRight: op = Opcode::UShr; break;
        case TokenType::Equal: op = Opcode::CmpEq; resultType = IRType::Bool; break;
        case TokenType::NotEqual: op = Opcode::CmpNe; resultType = IRType::Bool; break;
        case TokenType::Less: op = Opcode::CmpLt; resultType = IRType::Bool; break;
        case TokenType::Greater: op = Opcode::CmpGt; resultType = IRType::Bool; break;
        case TokenType::LessEqual: op = Opcode::CmpLe; resultType = IRType::Bool; break;
        case TokenType::GreaterEqual: op = Opcode::CmpGe; resultType = IRType::Bool; break;
        case TokenType::And: op = Opcode::LogAnd; resultType = IRType::Bool; break;
        case TokenType::Or: op = Opcode::LogOr; resultType = IRType::Bool; break;
        case TokenType::NullCoalesce: op = Opcode::LogOr; break; // simplified
        default: op = Opcode::Nop; break;
    }

    // String concatenation
    if (expr->op == TokenType::Plus &&
        (left->type == IRType::String || right->type == IRType::String)) {
        return emit(Opcode::Concat, IRType::String, {left, right});
    }

    // Type check: obj is TypeName
    if (expr->op == TokenType::Is) {
        return emit(Opcode::IsType, IRType::Bool, {left, right});
    }

    // Operator overloading: only for arithmetic on known class instances
    // Trigger when both operands are pointers, OR left is pointer and right is scalar (for __mul etc.)
    bool maybeClassOp = (left->type == IRType::Pointer && right->type == IRType::Pointer);
    // Also check: left is ptr, right is int, and a __mul/__div method exists
    if (!maybeClassOp && left->type == IRType::Pointer && 
        (right->type == IRType::Int32 || right->type == IRType::Int64) &&
        (expr->op == TokenType::Star || expr->op == TokenType::Slash || expr->op == TokenType::Percent)) {
        maybeClassOp = true;
    }
    if (maybeClassOp) {
        bool isClassOp = false;
        std::string opMethod;
        switch (expr->op) {
            case TokenType::Plus: opMethod = "__add"; break;
            case TokenType::Minus: opMethod = "__sub"; break;
            case TokenType::Star: opMethod = "__mul"; break;
            case TokenType::Slash: opMethod = "__div"; break;
            case TokenType::Percent: opMethod = "__mod"; break;
            case TokenType::Equal: opMethod = "__eq"; break;
            case TokenType::NotEqual: opMethod = "__ne"; break;
            case TokenType::Less: opMethod = "__lt"; break;
            case TokenType::Greater: opMethod = "__gt"; break;
            default: break;
        }
        if (!opMethod.empty()) {
            if (module_) {
                for (auto& fn : module_->functions) {
                    if (fn->name.find("." + opMethod) != std::string::npos) {
                        isClassOp = true;
                        break;
                    }
                }
            }
            if (isClassOp && left->type == IRType::Pointer) {
                auto inst = std::make_shared<Instruction>(Opcode::CallVirtual, resultType, "t" + std::to_string(nextId()), nextId());
                inst->targetName = opMethod;
                inst->addOperand(left);
                inst->addOperand(right);
                inst->parent = currentBlock_;
                inst->sourceLine = currentSourceLine_;
                inst->sourceColumn = currentSourceColumn_;
                if (currentBlock_) currentBlock_->instructions.push_back(inst);
                return inst;
            }
        }
    }

    // Propagate float/double type from operands for arithmetic ops
    if (resultType == IRType::Int32) {
        if (left->type == IRType::Float64 || right->type == IRType::Float64 ||
            left->type == IRType::Float32 || right->type == IRType::Float32) {
            resultType = IRType::Float64; // always use double for float math
        } else if (left->type == IRType::Int64 || right->type == IRType::Int64) {
            resultType = IRType::Int64;
        }
    }

    return emit(op, resultType, {left, right});
}

ValueRef IRGenerator::lowerUnary(UnaryExpr* expr) {
    auto operand = lowerExpression(expr->operand.get());

    switch (expr->op) {
        case TokenType::Minus:
            return emit(Opcode::Neg, operand->type, {operand});
        case TokenType::Not:
            return emit(Opcode::LogNot, IRType::Bool, {operand});
        case TokenType::BitNot:
            return emit(Opcode::BitNot, operand->type, {operand});
        case TokenType::BitAnd: {
            // Address-of: &variable — returns the alloca address (pointer to the variable)
            // Don't use the loaded operand — we need the address itself
            if (auto* id = dynamic_cast<IdentifierExpr*>(expr->operand.get())) {
                auto addr = lookupVar(id->name);
                if (addr) return addr; // Return the alloca pointer directly
            }
            // For non-identifier expressions, wrap in a temp alloca
            auto tmp = emitAlloca(operand->type, "__addr_tmp_" + std::to_string(nextId()));
            emitStore(tmp, operand);
            return tmp;
        }
        case TokenType::Star: {
            // Dereference: *ptr — load the value from the pointed-to address
            // The operand is the pointer value (address). Load an i32 from it.
            return emitLoad(operand, IRType::Int32);
        }
        case TokenType::Increment: {
            auto result = emit(Opcode::Add, operand->type, {operand, makeConstInt(1)});
            // Store back to the variable if operand is an identifier
            if (auto* id = dynamic_cast<IdentifierExpr*>(expr->operand.get())) {
                auto addr = lookupVar(id->name);
                if (addr) emitStore(addr, result);
            }
            return result;
        }
        case TokenType::Decrement: {
            auto result = emit(Opcode::Sub, operand->type, {operand, makeConstInt(1)});
            if (auto* id = dynamic_cast<IdentifierExpr*>(expr->operand.get())) {
                auto addr = lookupVar(id->name);
                if (addr) emitStore(addr, result);
            }
            return result;
        }
        default:
            return operand;
    }
}

ValueRef IRGenerator::lowerCall(CallExpr* expr) {
    std::vector<ValueRef> args;
    for (auto& arg : expr->arguments) {
        args.push_back(lowerExpression(arg.get()));
    }

    // Determine function name
    std::string funcName = "<unknown>";
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr->callee.get())) {
        funcName = id->name;
        // Check if this identifier is a lambda variable
        auto lit = lambdaNames_.find(funcName);
        if (lit != lambdaNames_.end()) {
            funcName = lit->second;
        }
    } else if (dynamic_cast<SuperExpr*>(expr->callee.get())) {
        // super(...) call — resolve to parent class constructor
        if (currentFunction_ && module_) {
            std::string fnName = currentFunction_->name;
            auto dotPos = fnName.find('.');
            if (dotPos != std::string::npos) {
                std::string className = fnName.substr(0, dotPos);
                for (auto& cls : module_->classes) {
                    if (cls.name == className && !cls.baseClass.empty()) {
                        funcName = cls.baseClass + ".constructor";
                        // Add 'this' as first arg
                        auto thisAddr = lookupVar("this");
                        if (thisAddr) {
                            auto thisVal = emitLoad(thisAddr, IRType::Pointer);
                            args.insert(args.begin(), thisVal);
                        }
                        break;
                    }
                }
            }
        }
    } else if (auto* member = dynamic_cast<MemberAccessExpr*>(expr->callee.get())) {
        // Check for enum static methods: EnumName.values(), EnumName.valueOf("X")
        if (auto* objId = dynamic_cast<IdentifierExpr*>(member->object.get())) {
            auto addr = lookupVar(objId->name);
            if (!addr && module_) {
                for (auto& cls : module_->classes) {
                    if (cls.name == objId->name) {
                        for (auto& ann : cls.annotations) {
                            if (ann.name == "_Enum") {
                                if (member->member == "values") {
                                    // Return an array of ordinal values [0, 1, 2, ...]
                                    auto arr = emit(Opcode::NewArray, IRType::Array, {});
                                    for (size_t i = 0; i < ann.args.size(); i++) {
                                        auto val = makeConstInt(static_cast<int>(i));
                                        auto addInst = std::make_shared<Instruction>(Opcode::CallVirtual, IRType::Void, "t" + std::to_string(nextId()), nextId());
                                        addInst->targetName = "List.add";
                                        addInst->addOperand(makeConstString("List"));
                                        addInst->addOperand(arr);
                                        addInst->addOperand(val);
                                        addInst->parent = currentBlock_;
                                        if (currentBlock_) currentBlock_->instructions.push_back(addInst);
                                    }
                                    return arr;
                                } else if (member->member == "valueOf" && !args.empty()) {
                                    // valueOf(name) — compare string arg against variant names
                                    // For AOT, emit a chain of comparisons
                                    auto* endBlock = createBlock(nextLabel("valueof.end"));
                                    auto resultAddr = emitAlloca(IRType::Int32, "__valueof_result");
                                    emitStore(resultAddr, makeConstInt(-1)); // default: not found

                                    for (size_t i = 0; i < ann.args.size(); i++) {
                                        auto nameStr = makeConstString(ann.args[i].first);
                                        auto cmp = emit(Opcode::CmpEq, IRType::Bool, {args[0], nameStr});
                                        auto* matchBlock = createBlock(nextLabel("valueof.match"));
                                        auto* nextBlock = createBlock(nextLabel("valueof.next"));
                                        emitCondBranch(cmp, matchBlock, nextBlock);

                                        setInsertPoint(matchBlock);
                                        emitStore(resultAddr, makeConstInt(static_cast<int>(i)));
                                        emitBranch(endBlock);

                                        setInsertPoint(nextBlock);
                                    }
                                    emitBranch(endBlock);
                                    setInsertPoint(endBlock);
                                    return emitLoad(resultAddr, IRType::Int32);
                                } else if (member->member == "fromOrdinal" && !args.empty()) {
                                    // fromOrdinal(ordinal) — return the ordinal directly (it IS the value)
                                    // In AOT, enum variants are just their ordinal values
                                    return args[0];
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }

        auto obj = lowerExpression(member->object.get());
        args.insert(args.begin(), obj); // 'this' as first arg
        funcName = member->member;

        // If the object is a simple identifier, encode qualified name for better dispatch
        // e.g., http.get → "http.get", Math.sqrt → "Math.sqrt"
        std::string qualifiedName = funcName;
        if (auto* objId = dynamic_cast<IdentifierExpr*>(member->object.get())) {
            // Check if this identifier is NOT a local variable (i.e., it's a namespace/module)
            auto addr = lookupVar(objId->name);
            if (!addr) {
                qualifiedName = objId->name + "." + funcName;
            }
        }

        // Virtual dispatch
        auto inst = std::make_shared<Instruction>(Opcode::CallVirtual, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
        inst->targetName = qualifiedName;
        for (auto& a : args) inst->addOperand(a);
        inst->sourceLine = currentSourceLine_;
        inst->sourceColumn = currentSourceColumn_;
        if (currentBlock_) currentBlock_->instructions.push_back(inst);
        return inst;
    }

    // Check if it's print
    if (funcName == "print") {
        if (args.empty()) {
            return emit(Opcode::Print, IRType::Void, {makeConstString("")});
        } else if (args.size() == 1) {
            return emit(Opcode::Print, IRType::Void, args);
        } else {
            // Multi-arg print: concatenate with spaces
            ValueRef combined = args[0];
            for (size_t i = 1; i < args.size(); i++) {
                auto space = makeConstString(" ");
                combined = emit(Opcode::Concat, IRType::String, {combined, space});
                combined = emit(Opcode::Concat, IRType::String, {combined, args[i]});
            }
            return emit(Opcode::Print, IRType::Void, {combined});
        }
    }

    // typeof(x) — returns the type name as a string at compile time
    if (funcName == "typeof" && args.size() == 1) {
        auto& arg = args[0];
        std::string typeName = "object";
        switch (arg->type) {
            case IRType::Int32: typeName = "int"; break;
            case IRType::Int64: typeName = "long"; break;
            case IRType::Int16: typeName = "short"; break;
            case IRType::Float32: typeName = "float"; break;
            case IRType::Float64: typeName = "double"; break;
            case IRType::Bool: typeName = "boolean"; break;
            case IRType::Char: typeName = "char"; break;
            case IRType::String: typeName = "string"; break;
            case IRType::Array: typeName = "array"; break;
            case IRType::Map: typeName = "map"; break;
            case IRType::Null: typeName = "null"; break;
            case IRType::Pointer: typeName = "object"; break;
            default: typeName = "unknown"; break;
        }
        return makeConstString(typeName);
    }

    // Look up the function's return type from the module
    IRType retType = IRType::Pointer;
    if (module_) {
        for (auto& fn : module_->functions) {
            if (fn->name == funcName) {
                retType = fn->returnType;
                break;
            }
        }
    }

    return emitCall(funcName, args, retType);
}

ValueRef IRGenerator::lowerMemberAccess(MemberAccessExpr* expr) {
    // Handle enum .name / .ordinal: Color.RED.name, Color.RED.ordinal
    if (auto* innerMember = dynamic_cast<MemberAccessExpr*>(expr->object.get())) {
        if (auto* enumId = dynamic_cast<IdentifierExpr*>(innerMember->object.get())) {
            // Check if this is an enum access pattern: EnumName.VARIANT.name/ordinal
            if (module_) {
                for (auto& cls : module_->classes) {
                    if (cls.name == enumId->name) {
                        // Check if it's an enum (has _Enum annotation)
                        for (auto& ann : cls.annotations) {
                            if (ann.name == "_Enum") {
                                std::string variantName = innerMember->member;
                                if (expr->member == "name") {
                                    return makeConstString(variantName);
                                } else if (expr->member == "ordinal") {
                                    for (size_t i = 0; i < ann.args.size(); i++) {
                                        if (ann.args[i].first == variantName) {
                                            return makeConstInt(static_cast<int>(i));
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    auto obj = lowerExpression(expr->object.get());
    auto inst = std::make_shared<Instruction>(Opcode::GetField, IRType::Int32, "t" + std::to_string(nextId()), nextId());
    inst->addOperand(obj);
    inst->targetName = expr->member;
    inst->sourceLine = expr->location.line;
    inst->sourceColumn = expr->location.column;
    if (currentBlock_) currentBlock_->instructions.push_back(inst);
    return inst;
}

ValueRef IRGenerator::lowerIndexAccess(IndexAccessExpr* expr) {
    auto obj = lowerExpression(expr->object.get());
    auto idx = lowerExpression(expr->index.get());
    return emit(Opcode::GetElement, IRType::Pointer, {obj, idx});
}

ValueRef IRGenerator::lowerAssignment(AssignmentExpr* expr) {
    auto val = lowerExpression(expr->value.get());

    if (auto* id = dynamic_cast<IdentifierExpr*>(expr->target.get())) {
        // Check for const reassignment
        if (constVars_.count(id->name)) {
            errors_.push_back("GardConstError: Cannot reassign constant variable '" + id->name + "'"
                              " at " + std::to_string(expr->location.line) + ":" + std::to_string(expr->location.column));
            return val;
        }
        auto addr = lookupVar(id->name);
        if (addr) {
            // Compound assignment
            if (expr->op != TokenType::Assign) {
                auto current = emitLoad(addr, IRType::Pointer);
                Opcode op = Opcode::Nop;
                switch (expr->op) {
                    case TokenType::PlusAssign: op = Opcode::Add; break;
                    case TokenType::MinusAssign: op = Opcode::Sub; break;
                    case TokenType::StarAssign: op = Opcode::Mul; break;
                    case TokenType::SlashAssign: op = Opcode::Div; break;
                    default: break;
                }
                val = emit(op, current->type, {current, val});
            }
            emitStore(addr, val);
        }
    } else if (auto* member = dynamic_cast<MemberAccessExpr*>(expr->target.get())) {
        auto obj = lowerExpression(member->object.get());
        auto inst = std::make_shared<Instruction>(Opcode::SetField, IRType::Void, "sf" + std::to_string(nextId()), nextId());
        inst->addOperand(obj);
        inst->addOperand(val);
        inst->targetName = member->member;
        inst->sourceLine = expr->location.line;
        inst->sourceColumn = expr->location.column;
        if (currentBlock_) currentBlock_->instructions.push_back(inst);
    } else if (auto* idx = dynamic_cast<IndexAccessExpr*>(expr->target.get())) {
        auto obj = lowerExpression(idx->object.get());
        auto index = lowerExpression(idx->index.get());
        emit(Opcode::SetElement, IRType::Void, {obj, index, val});
    } else if (auto* unary = dynamic_cast<UnaryExpr*>(expr->target.get())) {
        // *ptr = value — store through pointer dereference
        if (unary->op == TokenType::Star) {
            auto ptr = lowerExpression(unary->operand.get());
            emitStore(ptr, val);
        }
    }

    return val;
}

ValueRef IRGenerator::lowerTernary(TernaryExpr* expr) {
    auto cond = lowerExpression(expr->condition.get());
    auto* thenBlock = createBlock(nextLabel("tern.then"));
    auto* elseBlock = createBlock(nextLabel("tern.else"));
    auto* mergeBlock = createBlock(nextLabel("tern.end"));

    // Allocate temp to store result from either branch
    auto resultAddr = emitAlloca(IRType::Pointer, "tern.result." + std::to_string(nextId()));

    emitCondBranch(cond, thenBlock, elseBlock);

    setInsertPoint(thenBlock);
    auto thenVal = lowerExpression(expr->thenExpr.get());
    emitStore(resultAddr, thenVal);
    emitBranch(mergeBlock);

    setInsertPoint(elseBlock);
    auto elseVal = lowerExpression(expr->elseExpr.get());
    emitStore(resultAddr, elseVal);
    emitBranch(mergeBlock);

    setInsertPoint(mergeBlock);
    return emitLoad(resultAddr, IRType::Pointer);
}

ValueRef IRGenerator::lowerNew(NewExpr* expr) {
    std::vector<ValueRef> args;
    for (auto& arg : expr->arguments) {
        args.push_back(lowerExpression(arg.get()));
    }
    // Create the object
    auto inst = std::make_shared<Instruction>(Opcode::NewObject, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
    inst->targetName = expr->className;
    // Attach constructor args to NewObject so the emitter can use them for runtime primitives
    for (auto& a : args) inst->addOperand(a);
    if (currentBlock_) currentBlock_->instructions.push_back(inst);

    // Reified generics: store type arguments on the object as _typeArgs field
    if (!expr->typeArgs.empty()) {
        // Build a comma-separated string of type arg names
        std::string typeArgsStr;
        for (size_t i = 0; i < expr->typeArgs.size(); i++) {
            if (i > 0) typeArgsStr += ",";
            if (auto* named = dynamic_cast<NamedType*>(expr->typeArgs[i].get())) {
                typeArgsStr += named->name;
            } else if (auto* generic = dynamic_cast<GenericType*>(expr->typeArgs[i].get())) {
                typeArgsStr += generic->name;
            } else {
                typeArgsStr += "unknown";
            }
        }
        // Emit: obj._typeArgs = "int,string"
        auto typeArgsConst = std::make_shared<ConstantString>(typeArgsStr, nextId());
        module_->stringPool.push_back(typeArgsConst); // Add to string pool so emitter generates the constant
        auto setTypeArgs = std::make_shared<Instruction>(Opcode::SetField, IRType::Void, "t" + std::to_string(nextId()), nextId());
        setTypeArgs->targetName = "_typeArgs";
        setTypeArgs->addOperand(inst);
        setTypeArgs->addOperand(typeArgsConst);
        if (currentBlock_) currentBlock_->instructions.push_back(setTypeArgs);
    }

    // Always call constructor: ClassName.constructor(this, ...args)
    std::vector<ValueRef> ctorArgs;
    ctorArgs.push_back(inst); // 'this' as first arg
    for (auto& a : args) ctorArgs.push_back(a);

    // Only call constructor if one is defined for this class
    bool hasConstructor = false;
    for (auto& fn : module_->functions) {
        if (fn->name == expr->className + ".constructor") {
            hasConstructor = true;
            break;
        }
    }
    // Also check if the class declaration has a constructor (it may not be emitted yet)
    if (!hasConstructor) {
        bool classFound = false;
        for (auto& cls : module_->classes) {
            if (cls.name == expr->className) {
                classFound = true;
                // Check if any method is named "constructor"
                for (auto& m : cls.methodNames) {
                    if (m == "constructor") { hasConstructor = true; break; }
                }
                break;
            }
        }
        // If class is not in module (runtime/built-in class), don't call constructor
        // unless args were passed (indicating the runtime expects them)
        if (!classFound && args.empty()) {
            hasConstructor = false;
        } else if (!classFound && !args.empty()) {
            hasConstructor = true; // assume runtime class has a constructor if args are passed
        }
    }

    // Skip constructor call for known runtime primitive classes that don't have real constructors
    static const std::set<std::string> runtimePrimitives = {
        "Mutex", "Semaphore", "Barrier", "Channel", "RWLock"
    };
    bool isRuntimePrimitive = runtimePrimitives.count(expr->className) > 0;

    if ((hasConstructor || !args.empty()) && !isRuntimePrimitive) {
        auto callInst = std::make_shared<Instruction>(Opcode::CallVirtual, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
        callInst->targetName = expr->className + ".constructor";
        for (auto& a : ctorArgs) callInst->addOperand(a);
        callInst->sourceLine = currentSourceLine_;
        callInst->sourceColumn = currentSourceColumn_;
        if (currentBlock_) currentBlock_->instructions.push_back(callInst);
    }

    return inst;
}

ValueRef IRGenerator::lowerAwait(AwaitExpr* expr) {
    auto operand = lowerExpression(expr->operand.get());
    return emit(Opcode::Await, IRType::Pointer, {operand});
}

ValueRef IRGenerator::lowerCast(CastExpr* expr) {
    auto val = lowerExpression(expr->expression.get());
    // Encode the target type as a string constant for runtime dispatch
    std::string typeName = "unknown";
    if (expr->targetType) {
        if (auto* named = dynamic_cast<NamedType*>(expr->targetType.get())) {
            typeName = named->name;
        }
    }
    auto typeConst = makeConstString(typeName);
    return emit(Opcode::Cast, IRType::Pointer, {val, typeConst});
}

ValueRef IRGenerator::lowerLambda(LambdaExpr* expr) {
    // Generate lambda as anonymous function
    std::string lambdaName = "<lambda." + std::to_string(nextId()) + ">";
    IRType retType = IRType::Int64;
    if (expr->returnType) retType = mapTypeAnnotation(expr->returnType.get());
    auto* fn = module_->createFunction(lambdaName, retType);

    for (auto& param : expr->params) {
        IRType pType = param.type ? mapTypeAnnotation(param.type.get()) : IRType::Int64;
        auto p = std::make_shared<Parameter>(pType, param.name, static_cast<int>(fn->params.size()), nextId());
        fn->params.push_back(p);
    }

    auto* savedFn = currentFunction_;
    auto* savedBlock = currentBlock_;
    currentFunction_ = fn;
    auto* entry = createBlock("entry");
    setInsertPoint(entry);
    pushScope();

    for (auto& p : fn->params) {
        auto addr = emitAlloca(p->type, p->name);
        emitStore(addr, p);
        declareVar(p->name, addr);
    }

    if (expr->hasBlockBody) {
        for (auto& s : expr->bodyBlock) lowerStatement(s.get());
    } else if (expr->bodyExpr) {
        auto val = lowerExpression(expr->bodyExpr.get());
        emitReturn(val);
    }

    if (currentBlock_ && !currentBlock_->isTerminated()) emitReturnVoid();

    popScope();
    currentFunction_ = savedFn;
    currentBlock_ = savedBlock;

    // Store lambda name for variable tracking
    lastLambdaName_ = lambdaName;

    // Create closure reference — store the function name for the emitter
    auto closureInst = std::make_shared<Instruction>(Opcode::CreateClosure, IRType::Pointer, "t" + std::to_string(nextId()), nextId());
    closureInst->targetName = lambdaName; // The actual function name
    closureInst->parent = currentBlock_;
    if (currentBlock_) currentBlock_->instructions.push_back(closureInst);
    return closureInst;
}

ValueRef IRGenerator::lowerArray(ArrayExpr* expr) {
    auto arr = emit(Opcode::NewArray, IRType::Array, {makeConstInt(static_cast<int64_t>(expr->elements.size()))});
    for (size_t i = 0; i < expr->elements.size(); i++) {
        auto elem = lowerExpression(expr->elements[i].get());
        emit(Opcode::SetElement, IRType::Void, {arr, makeConstInt(static_cast<int64_t>(i)), elem});
    }
    return arr;
}

ValueRef IRGenerator::lowerMap(MapExpr* expr) {
    auto map = emit(Opcode::NewMap, IRType::Map, {});
    for (auto& entry : expr->entries) {
        ValueRef key;
        // Object literal keys: if the key is an identifier, treat it as a string constant
        // e.g., {port: 9876} → key is the string "port", not a variable lookup
        if (auto* idKey = dynamic_cast<IdentifierExpr*>(entry.first.get())) {
            key = makeConstString(idKey->name);
        } else {
            key = lowerExpression(entry.first.get());
        }
        auto val = lowerExpression(entry.second.get());
        emit(Opcode::SetElement, IRType::Void, {map, key, val});
    }
    return map;
}

ValueRef IRGenerator::lowerSet(SetExpr* expr) {
    // Create a new array (sets are backed by arrays with uniqueness enforcement)
    auto set = emit(Opcode::NewArray, IRType::Array, {});
    for (auto& elem : expr->elements) {
        auto val = lowerExpression(elem.get());
        // Use CallVirtual to Set.add for uniqueness enforcement
        auto addInst = std::make_shared<Instruction>(Opcode::CallVirtual, IRType::Void, "t" + std::to_string(nextId()), nextId());
        addInst->targetName = "Set.add";
        addInst->addOperand(makeConstString("Set"));
        addInst->addOperand(set);
        addInst->addOperand(val);
        addInst->parent = currentBlock_;
        if (currentBlock_) currentBlock_->instructions.push_back(addInst);
    }
    return set;
}

} // namespace ir
} // namespace gard
