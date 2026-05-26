#include "tools/formatter.h"
#include <algorithm>

namespace gard {
namespace tools {

Formatter::Formatter(const FormatConfig& config) : config_(config) {}

std::string Formatter::format(const Program& program) {
    out_.str("");
    out_.clear();
    indentLevel_ = 0;

    // Separate imports from other statements
    std::vector<Statement*> imports;
    std::vector<Statement*> others;

    for (auto& stmt : program.statements) {
        if (stmt->kind == StmtKind::Import) imports.push_back(stmt.get());
        else others.push_back(stmt.get());
    }

    // Emit imports first (sorted if configured)
    if (!imports.empty()) {
        if (config_.sortImports) {
            std::sort(imports.begin(), imports.end(), [](Statement* a, Statement* b) {
                auto* ia = static_cast<ImportStmt*>(a);
                auto* ib = static_cast<ImportStmt*>(b);
                return ia->path < ib->path;
            });
        }
        for (auto* stmt : imports) formatStatement(stmt);
        emitLine();
    }

    // Emit other statements with blank lines between top-level declarations
    for (size_t i = 0; i < others.size(); i++) {
        formatStatement(others[i]);
        if (i + 1 < others.size()) emitLine();
    }

    return out_.str();
}

// --- Statement formatting ---

void Formatter::formatStatement(Statement* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case StmtKind::VarDeclaration: formatVarDecl(static_cast<VarDeclarationStmt*>(stmt)); break;
        case StmtKind::FunctionDeclaration: formatFunctionDecl(static_cast<FunctionDeclStmt*>(stmt)); break;
        case StmtKind::Class: formatClassDecl(static_cast<ClassDeclStmt*>(stmt)); break;
        case StmtKind::Interface: formatInterfaceDecl(static_cast<InterfaceDeclStmt*>(stmt)); break;
        case StmtKind::Import: formatImport(static_cast<ImportStmt*>(stmt)); break;
        case StmtKind::Export: formatExport(static_cast<ExportStmt*>(stmt)); break;
        case StmtKind::If: formatIf(static_cast<IfStmt*>(stmt)); break;
        case StmtKind::For: formatFor(static_cast<ForStmt*>(stmt)); break;
        case StmtKind::ForEach: formatForEach(static_cast<ForEachStmt*>(stmt)); break;
        case StmtKind::While: formatWhile(static_cast<WhileStmt*>(stmt)); break;
        case StmtKind::DoWhile: formatDoWhile(static_cast<DoWhileStmt*>(stmt)); break;
        case StmtKind::Switch: formatSwitch(static_cast<SwitchStmt*>(stmt)); break;
        case StmtKind::Match: formatMatch(static_cast<MatchStmt*>(stmt)); break;
        case StmtKind::Return: formatReturn(static_cast<ReturnStmt*>(stmt)); break;
        case StmtKind::Throw: formatThrow(static_cast<ThrowStmt*>(stmt)); break;
        case StmtKind::TryCatch: formatTryCatch(static_cast<TryCatchStmt*>(stmt)); break;
        case StmtKind::Expression: formatExprStmt(static_cast<ExpressionStmt*>(stmt)); break;
        case StmtKind::BlockchainContract: formatBlockchainContract(static_cast<BlockchainContractStmt*>(stmt)); break;
        case StmtKind::Break: emitLine(indentStr() + "break;"); break;
        case StmtKind::Continue: emitLine(indentStr() + "continue;"); break;
        default: break;
    }
}

void Formatter::formatVarDecl(VarDeclarationStmt* stmt) {
    std::string line = indentStr();
    switch (stmt->declKind) {
        case VarDeclKind::Let: line += "let "; break;
        case VarDeclKind::Var: line += "var "; break;
        case VarDeclKind::Const: line += "const "; break;
        case VarDeclKind::Readonly: line += "readonly "; break;
    }
    line += stmt->name;
    if (stmt->type) line += ": " + formatType(stmt->type.get());
    if (stmt->initializer) line += " = " + formatExpr(stmt->initializer.get());
    line += ";";
    emitLine(line);
}

void Formatter::formatFunctionDecl(FunctionDeclStmt* stmt) {
    std::string line = indentStr();
    if (!stmt->accessModifier.empty()) line += stmt->accessModifier + " ";
    if (stmt->isStatic) line += "static ";
    if (stmt->isAsync) line += "async ";
    line += "function " + stmt->name + "(";

    for (size_t i = 0; i < stmt->params.size(); i++) {
        if (i > 0) line += ", ";
        line += stmt->params[i].name;
        if (stmt->params[i].type) line += ": " + formatType(stmt->params[i].type.get());
    }
    line += ")";
    if (stmt->returnType) line += ": " + formatType(stmt->returnType.get());
    line += " {";
    emitLine(line);

    indent();
    for (auto& s : stmt->body) formatStatement(s.get());
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatClassDecl(ClassDeclStmt* stmt) {
    std::string line = indentStr();
    for (auto& ann : stmt->annotations) line += "@" + ann.name + "\n" + indentStr();
    if (stmt->isAbstract) line += "abstract ";
    line += "class " + stmt->name;
    if (!stmt->baseClass.empty()) line += " extends " + stmt->baseClass;
    if (!stmt->interfaces.empty()) {
        line += " implements ";
        for (size_t i = 0; i < stmt->interfaces.size(); i++) {
            if (i > 0) line += ", ";
            line += stmt->interfaces[i];
        }
    }
    line += " {";
    emitLine(line);

    indent();
    for (auto& field : stmt->fields) {
        std::string fl = indentStr();
        if (!field.accessModifier.empty()) fl += field.accessModifier + " ";
        if (field.isStatic) fl += "static ";
        switch (field.declKind) {
            case VarDeclKind::Let: fl += "let "; break;
            case VarDeclKind::Const: fl += "const "; break;
            default: fl += "let "; break;
        }
        fl += field.name;
        if (field.type) fl += ": " + formatType(field.type.get());
        if (field.initializer) fl += " = " + formatExpr(field.initializer.get());
        fl += ";";
        emitLine(fl);
    }
    if (!stmt->fields.empty() && (!stmt->methods.empty() || stmt->constructor)) emitLine();

    if (stmt->constructor) {
        std::string cl = indentStr() + "constructor(";
        for (size_t i = 0; i < stmt->constructor->params.size(); i++) {
            if (i > 0) cl += ", ";
            cl += stmt->constructor->params[i].name;
            if (stmt->constructor->params[i].type) cl += ": " + formatType(stmt->constructor->params[i].type.get());
        }
        cl += ") {";
        emitLine(cl);
        indent();
        for (auto& s : stmt->constructor->body) formatStatement(s.get());
        dedent();
        emitLine(indentStr() + "}");
        if (!stmt->methods.empty()) emitLine();
    }

    for (auto& method : stmt->methods) {
        std::string ml = indentStr();
        if (!method.accessModifier.empty()) ml += method.accessModifier + " ";
        if (method.isStatic) ml += "static ";
        if (method.isAsync) ml += "async ";
        if (method.isAbstract) ml += "abstract ";
        ml += "function " + method.name + "(";
        for (size_t i = 0; i < method.params.size(); i++) {
            if (i > 0) ml += ", ";
            ml += method.params[i].name;
            if (method.params[i].type) ml += ": " + formatType(method.params[i].type.get());
        }
        ml += ")";
        if (method.returnType) ml += ": " + formatType(method.returnType.get());
        if (method.isAbstract) { ml += ";"; emitLine(ml); continue; }
        ml += " {";
        emitLine(ml);
        indent();
        for (auto& s : method.body) formatStatement(s.get());
        dedent();
        emitLine(indentStr() + "}");
    }

    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatInterfaceDecl(InterfaceDeclStmt* stmt) {
    emitLine(indentStr() + "interface " + stmt->name + " {");
    indent();
    for (auto& m : stmt->methods) {
        std::string line = indentStr() + m.name + "(";
        for (size_t i = 0; i < m.params.size(); i++) {
            if (i > 0) line += ", ";
            line += m.params[i].name;
            if (m.params[i].type) line += ": " + formatType(m.params[i].type.get());
        }
        line += ")";
        if (m.returnType) line += ": " + formatType(m.returnType.get());
        line += ";";
        emitLine(line);
    }
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatImport(ImportStmt* stmt) {
    std::string line = indentStr() + "import { ";
    for (size_t i = 0; i < stmt->names.size(); i++) {
        if (i > 0) line += ", ";
        line += stmt->names[i];
    }
    line += " } from \"" + stmt->path + "\";";
    emitLine(line);
}

void Formatter::formatExport(ExportStmt* stmt) {
    emit(indentStr() + "export ");
    if (stmt->declaration) formatStatement(stmt->declaration.get());
}

void Formatter::formatIf(IfStmt* stmt) {
    emitLine(indentStr() + "if (" + formatExpr(stmt->condition.get()) + ") {");
    indent();
    if (auto* block = dynamic_cast<BlockStmt*>(stmt->thenBranch.get())) {
        for (auto& s : block->statements) formatStatement(s.get());
    } else {
        formatStatement(stmt->thenBranch.get());
    }
    dedent();
    if (stmt->elseBranch) {
        emitLine(indentStr() + "} else {");
        indent();
        if (auto* block = dynamic_cast<BlockStmt*>(stmt->elseBranch.get())) {
            for (auto& s : block->statements) formatStatement(s.get());
        } else {
            formatStatement(stmt->elseBranch.get());
        }
        dedent();
    }
    emitLine(indentStr() + "}");
}

void Formatter::formatFor(ForStmt* stmt) {
    emitLine(indentStr() + "for (...) {");
    indent();
    if (auto* block = dynamic_cast<BlockStmt*>(stmt->body.get())) {
        for (auto& s : block->statements) formatStatement(s.get());
    } else if (stmt->body) {
        formatStatement(stmt->body.get());
    }
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatForEach(ForEachStmt* stmt) {
    emitLine(indentStr() + "foreach (" + stmt->variable + " in " + formatExpr(stmt->iterable.get()) + ") {");
    indent();
    if (auto* block = dynamic_cast<BlockStmt*>(stmt->body.get())) {
        for (auto& s : block->statements) formatStatement(s.get());
    } else if (stmt->body) {
        formatStatement(stmt->body.get());
    }
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatWhile(WhileStmt* stmt) {
    emitLine(indentStr() + "while (" + formatExpr(stmt->condition.get()) + ") {");
    indent();
    if (auto* block = dynamic_cast<BlockStmt*>(stmt->body.get())) {
        for (auto& s : block->statements) formatStatement(s.get());
    } else if (stmt->body) {
        formatStatement(stmt->body.get());
    }
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatDoWhile(DoWhileStmt* stmt) {
    emitLine(indentStr() + "do {");
    indent();
    if (auto* block = dynamic_cast<BlockStmt*>(stmt->body.get())) {
        for (auto& s : block->statements) formatStatement(s.get());
    } else if (stmt->body) {
        formatStatement(stmt->body.get());
    }
    dedent();
    emitLine(indentStr() + "} while (" + formatExpr(stmt->condition.get()) + ");");
}

void Formatter::formatSwitch(SwitchStmt* stmt) {
    emitLine(indentStr() + "switch (" + formatExpr(stmt->discriminant.get()) + ") {");
    indent();
    for (auto& c : stmt->cases) {
        if (c.isDefault) emitLine(indentStr() + "default:");
        else emitLine(indentStr() + "case " + formatExpr(c.value.get()) + ":");
        indent();
        for (auto& s : c.body) formatStatement(s.get());
        dedent();
    }
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatMatch(MatchStmt* stmt) {
    emitLine(indentStr() + "match " + formatExpr(stmt->value.get()) + " {");
    indent();
    for (auto& arm : stmt->arms) {
        emitLine(indentStr() + formatExpr(arm.pattern.get()) + " => " + formatExpr(arm.body.get()) + ",");
    }
    dedent();
    emitLine(indentStr() + "}");
}

void Formatter::formatReturn(ReturnStmt* stmt) {
    if (stmt->value) emitLine(indentStr() + "return " + formatExpr(stmt->value.get()) + ";");
    else emitLine(indentStr() + "return;");
}

void Formatter::formatThrow(ThrowStmt* stmt) {
    emitLine(indentStr() + "throw " + formatExpr(stmt->value.get()) + ";");
}

void Formatter::formatTryCatch(TryCatchStmt* stmt) {
    emitLine(indentStr() + "try {");
    indent();
    for (auto& s : stmt->tryBody) formatStatement(s.get());
    dedent();
    for (auto& c : stmt->catchClauses) {
        std::string cl = indentStr() + "} catch (" + c.paramName;
        if (c.paramType) cl += ": " + formatType(c.paramType.get());
        cl += ") {";
        emitLine(cl);
        indent();
        for (auto& s : c.body) formatStatement(s.get());
        dedent();
    }
    if (!stmt->finallyBody.empty()) {
        emitLine(indentStr() + "} finally {");
        indent();
        for (auto& s : stmt->finallyBody) formatStatement(s.get());
        dedent();
    }
    emitLine(indentStr() + "}");
}

void Formatter::formatExprStmt(ExpressionStmt* stmt) {
    if (stmt->expression) emitLine(indentStr() + formatExpr(stmt->expression.get()) + ";");
}

void Formatter::formatBlockchainContract(BlockchainContractStmt* stmt) {
    emitLine(indentStr() + "blockchain contract " + stmt->name + " {");
    indent();
    for (auto& field : stmt->fields) {
        std::string fl = indentStr();
        if (!field.accessModifier.empty()) fl += field.accessModifier + " ";
        fl += "ledger " + field.name;
        if (field.type) fl += ": " + formatType(field.type.get());
        fl += ";";
        emitLine(fl);
    }
    dedent();
    emitLine(indentStr() + "}");
}

// --- Expression formatting (simplified) ---

std::string Formatter::formatExpr(Expression* expr) {
    if (!expr) return "";
    switch (expr->kind) {
        case ExprKind::IntLiteral: return static_cast<IntLiteralExpr*>(expr)->value;
        case ExprKind::DoubleLiteral: return static_cast<DoubleLiteralExpr*>(expr)->value;
        case ExprKind::StringLiteral: return "\"" + static_cast<StringLiteralExpr*>(expr)->value + "\"";
        case ExprKind::BoolLiteral: return static_cast<BoolLiteralExpr*>(expr)->value ? "true" : "false";
        case ExprKind::NullLiteral: return "null";
        case ExprKind::Identifier: return static_cast<IdentifierExpr*>(expr)->name;
        case ExprKind::This: return "this";
        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(expr);
            return formatExpr(b->left.get()) + " " + tokenTypeToString(b->op) + " " + formatExpr(b->right.get());
        }
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(expr);
            std::string r = formatExpr(c->callee.get()) + "(";
            for (size_t i = 0; i < c->arguments.size(); i++) {
                if (i > 0) r += ", ";
                r += formatExpr(c->arguments[i].get());
            }
            return r + ")";
        }
        case ExprKind::MemberAccess: {
            auto* m = static_cast<MemberAccessExpr*>(expr);
            return formatExpr(m->object.get()) + "." + m->member;
        }
        case ExprKind::New: {
            auto* n = static_cast<NewExpr*>(expr);
            std::string r = "new " + n->className + "(";
            for (size_t i = 0; i < n->arguments.size(); i++) {
                if (i > 0) r += ", ";
                r += formatExpr(n->arguments[i].get());
            }
            return r + ")";
        }
        case ExprKind::Assignment: {
            auto* a = static_cast<AssignmentExpr*>(expr);
            return formatExpr(a->target.get()) + " " + tokenTypeToString(a->op) + " " + formatExpr(a->value.get());
        }
        case ExprKind::Array: {
            auto* arr = static_cast<ArrayExpr*>(expr);
            std::string r = "[";
            for (size_t i = 0; i < arr->elements.size(); i++) {
                if (i > 0) r += ", ";
                r += formatExpr(arr->elements[i].get());
            }
            return r + "]";
        }
        case ExprKind::Await: {
            auto* aw = static_cast<AwaitExpr*>(expr);
            return "await " + formatExpr(aw->operand.get());
        }
        default: return "/* expr */";
    }
}

std::string Formatter::formatType(TypeAnnotation* type) {
    if (!type) return "";
    if (auto* named = dynamic_cast<NamedType*>(type)) return named->name;
    if (auto* generic = dynamic_cast<GenericType*>(type)) {
        std::string r = generic->name + "<";
        for (size_t i = 0; i < generic->typeArgs.size(); i++) {
            if (i > 0) r += ", ";
            r += formatType(generic->typeArgs[i].get());
        }
        return r + ">";
    }
    if (auto* nullable = dynamic_cast<NullableType*>(type)) {
        return formatType(nullable->inner.get()) + "?";
    }
    return "any";
}

// --- Helpers ---

void Formatter::emit(const std::string& text) { out_ << text; }
void Formatter::emitLine(const std::string& text) { out_ << text << "\n"; }
void Formatter::indent() { indentLevel_++; }
void Formatter::dedent() { if (indentLevel_ > 0) indentLevel_--; }
std::string Formatter::indentStr() const {
    return std::string(indentLevel_ * config_.indentSize, ' ');
}

} // namespace tools
} // namespace gard
