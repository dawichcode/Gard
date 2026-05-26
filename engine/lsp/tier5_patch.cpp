// ============================================================================
// Tier 5: textDocument/semanticTokens/full
// ============================================================================

// Semantic token type indices (must match legend in capabilities)
static const int SEM_NAMESPACE = 0;
static const int SEM_TYPE = 1;
static const int SEM_CLASS = 2;
static const int SEM_ENUM = 3;
static const int SEM_INTERFACE = 4;
static const int SEM_STRUCT = 5;
static const int SEM_TYPE_PARAMETER = 6;
static const int SEM_PARAMETER = 7;
static const int SEM_VARIABLE = 8;
static const int SEM_PROPERTY = 9;
static const int SEM_ENUM_MEMBER = 10;
static const int SEM_FUNCTION = 11;
static const int SEM_METHOD = 12;
static const int SEM_KEYWORD = 13;
static const int SEM_COMMENT = 14;
static const int SEM_STRING = 15;
static const int SEM_NUMBER = 16;
static const int SEM_OPERATOR = 17;
static const int SEM_DECORATOR = 18;

// Semantic token modifier bit flags
static const int SEM_MOD_DECLARATION = 1 << 0;
static const int SEM_MOD_DEFINITION = 1 << 1;
static const int SEM_MOD_READONLY = 1 << 2;
static const int SEM_MOD_STATIC = 1 << 3;
static const int SEM_MOD_ASYNC = 1 << 4;

// Determine semantic token type for a given Gard token
static int getSemanticTokenType(const gard::Token& tok, const DocumentState& doc) {
    switch (tok.type) {
        // String literals
        case gard::TokenType::StringLiteral:
        case gard::TokenType::TemplateLiteral:
        case gard::TokenType::CharLiteral:
            return SEM_STRING;

        // Number literals
        case gard::TokenType::IntLiteral:
        case gard::TokenType::DoubleLiteral:
        case gard::TokenType::FloatLiteral:
        case gard::TokenType::LongLiteral:
        case gard::TokenType::HexLiteral:
        case gard::TokenType::BinaryLiteral:
            return SEM_NUMBER;

        // Comments
        case gard::TokenType::DocComment:
            return SEM_COMMENT;

        // Decorator/annotation
        case gard::TokenType::At:
            return SEM_DECORATOR;

        // Operators
        case gard::TokenType::Plus:
        case gard::TokenType::Minus:
        case gard::TokenType::Star:
        case gard::TokenType::Slash:
        case gard::TokenType::Percent:
        case gard::TokenType::Assign:
        case gard::TokenType::PlusAssign:
        case gard::TokenType::MinusAssign:
        case gard::TokenType::StarAssign:
        case gard::TokenType::SlashAssign:
        case gard::TokenType::Equal:
        case gard::TokenType::NotEqual:
        case gard::TokenType::Less:
        case gard::TokenType::Greater:
        case gard::TokenType::LessEqual:
        case gard::TokenType::GreaterEqual:
        case gard::TokenType::And:
        case gard::TokenType::Or:
        case gard::TokenType::Not:
        case gard::TokenType::BitAnd:
        case gard::TokenType::BitOr:
        case gard::TokenType::BitXor:
        case gard::TokenType::BitNot:
        case gard::TokenType::ShiftLeft:
        case gard::TokenType::ShiftRight:
        case gard::TokenType::UnsignedShiftRight:
        case gard::TokenType::Arrow:
        case gard::TokenType::OptionalChain:
        case gard::TokenType::NullCoalesce:
        case gard::TokenType::Increment:
        case gard::TokenType::Decrement:
            return SEM_OPERATOR;

        // Identifiers — look up in symbol table
        case gard::TokenType::Identifier: {
            if (doc.sema && doc.analysisValid) {
                gard::Symbol* sym = doc.sema->getSymbolTable().resolve(tok.value);
                if (sym) {
                    switch (sym->kind) {
                        case gard::SymbolKind::Variable:    return SEM_VARIABLE;
                        case gard::SymbolKind::Parameter:   return SEM_PARAMETER;
                        case gard::SymbolKind::Function:    return SEM_FUNCTION;
                        case gard::SymbolKind::Method:      return SEM_METHOD;
                        case gard::SymbolKind::Class:       return SEM_CLASS;
                        case gard::SymbolKind::Interface:   return SEM_INTERFACE;
                        case gard::SymbolKind::Field:       return SEM_PROPERTY;
                        case gard::SymbolKind::Constructor: return SEM_FUNCTION;
                        case gard::SymbolKind::Module:      return SEM_NAMESPACE;
                        case gard::SymbolKind::GenericParam:return SEM_TYPE_PARAMETER;
                        case gard::SymbolKind::BlockchainContract: return SEM_CLASS;
                        default: return SEM_VARIABLE;
                    }
                }
            }
            return SEM_VARIABLE;
        }

        // Boolean/null literals are keywords
        case gard::TokenType::BoolLiteral:
        case gard::TokenType::NullLiteral:
        case gard::TokenType::True:
        case gard::TokenType::False:
        case gard::TokenType::Null:
            return SEM_KEYWORD;

        default:
            break;
    }

    // Check if it's a keyword token
    if (tok.isKeyword()) {
        return SEM_KEYWORD;
    }

    return -1; // skip this token
}

// Get semantic token modifiers for a token
static int getSemanticTokenModifiers(const gard::Token& tok, const DocumentState& doc) {
    int mods = 0;
    if (tok.type == gard::TokenType::Identifier && doc.sema && doc.analysisValid) {
        gard::Symbol* sym = doc.sema->getSymbolTable().resolve(tok.value);
        if (sym) {
            if (sym->isDefined) mods |= SEM_MOD_DEFINITION;
            if (sym->isConst || sym->isReadonly) mods |= SEM_MOD_READONLY;
            if (sym->isStatic) mods |= SEM_MOD_STATIC;
            if (sym->isAsync) mods |= SEM_MOD_ASYNC;
        }
    }
    return mods;
}

static void handleSemanticTokensFull(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);

    logMessage("semanticTokens/full: " + uri);

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "{\"data\":[]}");
        return;
    }

    DocumentState& doc = it->second;
    if (doc.tokens.empty()) {
        sendResponse(id, "{\"data\":[]}");
        return;
    }

    std::vector<int> data;
    int prevLine = 0;
    int prevStart = 0;

    for (const auto& tok : doc.tokens) {
        if (tok.type == gard::TokenType::EndOfFile ||
            tok.type == gard::TokenType::Invalid) continue;

        // Skip punctuation that doesn't get semantic highlighting
        if (tok.type == gard::TokenType::LeftParen ||
            tok.type == gard::TokenType::RightParen ||
            tok.type == gard::TokenType::LeftBrace ||
            tok.type == gard::TokenType::RightBrace ||
            tok.type == gard::TokenType::LeftBracket ||
            tok.type == gard::TokenType::RightBracket ||
            tok.type == gard::TokenType::Semicolon ||
            tok.type == gard::TokenType::Colon ||
            tok.type == gard::TokenType::Comma ||
            tok.type == gard::TokenType::Dot ||
            tok.type == gard::TokenType::Spread ||
            tok.type == gard::TokenType::QuestionMark) continue;

        int tokenType = getSemanticTokenType(tok, doc);
        if (tokenType < 0) continue;

        int tokenMods = getSemanticTokenModifiers(tok, doc);

        // Convert to 0-indexed LSP positions
        int tokenLine = tok.location.line - 1;
        int tokenStart = tok.location.column - 1;
        int tokenLength = (int)tok.value.size();

        if (tokenLength <= 0) continue;

        int deltaLine = tokenLine - prevLine;
        int deltaStart = (deltaLine == 0) ? (tokenStart - prevStart) : tokenStart;

        data.push_back(deltaLine);
        data.push_back(deltaStart);
        data.push_back(tokenLength);
        data.push_back(tokenType);
        data.push_back(tokenMods);

        prevLine = tokenLine;
        prevStart = tokenStart;
    }

    // Build response
    std::string result = "{\"data\":[";
    for (size_t i = 0; i < data.size(); i++) {
        if (i > 0) result += ",";
        result += std::to_string(data[i]);
    }
    result += "]}";

    sendResponse(id, result);
}

// ============================================================================
// Tier 5: workspace/symbol
// ============================================================================

static void handleWorkspaceSymbol(int id, const std::string& params) {
    std::string query = jsonGetString(params, "query");

    logMessage("workspace/symbol: query=" + query);

    std::vector<std::string> results;

    // Search all open documents for matching symbols
    for (const auto& pair : documents) {
        const std::string& docUri = pair.first;
        const DocumentState& doc = pair.second;

        if (!doc.analysisValid || doc.program.statements.empty()) continue;

        // Walk top-level statements looking for named declarations
        for (const auto& stmt : doc.program.statements) {
            if (!stmt) continue;

            std::string symbolName;
            int symbolKind = 0;
            int symbolLine = stmt->location.line > 0 ? stmt->location.line - 1 : 0;
            int symbolCol = stmt->location.column > 0 ? stmt->location.column - 1 : 0;

            switch (stmt->kind) {
                case gard::StmtKind::FunctionDeclaration: {
                    auto* fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
                    symbolName = fn->name;
                    symbolKind = LSP_SYMBOL_FUNCTION;
                    break;
                }
                case gard::StmtKind::Class: {
                    auto* cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                    symbolName = cls->name;
                    symbolKind = LSP_SYMBOL_CLASS;
                    break;
                }
                case gard::StmtKind::Interface: {
                    auto* iface = static_cast<gard::InterfaceDeclStmt*>(stmt.get());
                    symbolName = iface->name;
                    symbolKind = LSP_SYMBOL_INTERFACE;
                    break;
                }
                case gard::StmtKind::Enum: {
                    auto* enm = static_cast<gard::EnumDeclStmt*>(stmt.get());
                    symbolName = enm->name;
                    symbolKind = LSP_SYMBOL_ENUM;
                    break;
                }
                case gard::StmtKind::VarDeclaration: {
                    auto* var = static_cast<gard::VarDeclarationStmt*>(stmt.get());
                    symbolName = var->name;
                    symbolKind = LSP_SYMBOL_VARIABLE;
                    break;
                }
                case gard::StmtKind::Export: {
                    auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                    if (exp->declaration) {
                        auto* inner = exp->declaration.get();
                        symbolLine = inner->location.line > 0 ? inner->location.line - 1 : 0;
                        symbolCol = inner->location.column > 0 ? inner->location.column - 1 : 0;
                        switch (inner->kind) {
                            case gard::StmtKind::FunctionDeclaration:
                                symbolName = static_cast<gard::FunctionDeclStmt*>(inner)->name;
                                symbolKind = LSP_SYMBOL_FUNCTION;
                                break;
                            case gard::StmtKind::Class:
                                symbolName = static_cast<gard::ClassDeclStmt*>(inner)->name;
                                symbolKind = LSP_SYMBOL_CLASS;
                                break;
                            case gard::StmtKind::Interface:
                                symbolName = static_cast<gard::InterfaceDeclStmt*>(inner)->name;
                                symbolKind = LSP_SYMBOL_INTERFACE;
                                break;
                            case gard::StmtKind::Enum:
                                symbolName = static_cast<gard::EnumDeclStmt*>(inner)->name;
                                symbolKind = LSP_SYMBOL_ENUM;
                                break;
                            case gard::StmtKind::VarDeclaration:
                                symbolName = static_cast<gard::VarDeclarationStmt*>(inner)->name;
                                symbolKind = LSP_SYMBOL_VARIABLE;
                                break;
                            default: break;
                        }
                    }
                    break;
                }
                default: break;
            }

            if (symbolName.empty()) continue;

            // Case-insensitive substring match
            if (!query.empty()) {
                std::string lowerName = symbolName;
                std::string lowerQuery = query;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
                if (lowerName.find(lowerQuery) == std::string::npos) continue;
            }

            int selEnd = symbolCol + (int)symbolName.size();
            std::string entry = "{";
            entry += "\"name\":" + jsonString(symbolName) + ",";
            entry += "\"kind\":" + jsonInt(symbolKind) + ",";
            entry += "\"location\":{";
            entry += "\"uri\":" + jsonString(docUri) + ",";
            entry += "\"range\":{";
            entry += "\"start\":{\"line\":" + jsonInt(symbolLine) + ",\"character\":" + jsonInt(symbolCol) + "},";
            entry += "\"end\":{\"line\":" + jsonInt(symbolLine) + ",\"character\":" + jsonInt(selEnd) + "}";
            entry += "}}}";
            results.push_back(entry);
        }
    }

    // Build response
    std::string result = "[";
    for (size_t i = 0; i < results.size(); i++) {
        if (i > 0) result += ",";
        result += results[i];
    }
    result += "]";

    sendResponse(id, result);
}

// ============================================================================
// Tier 5: textDocument/inlayHint
// ============================================================================

// Helper: walk expressions recursively to find CallExpr nodes
static void collectCallExprs(const gard::Expression* expr,
                             std::vector<const gard::CallExpr*>& calls) {
    if (!expr) return;

    if (expr->kind == gard::ExprKind::Call) {
        calls.push_back(static_cast<const gard::CallExpr*>(expr));
        // Also recurse into callee and arguments
        auto* call = static_cast<const gard::CallExpr*>(expr);
        collectCallExprs(call->callee.get(), calls);
        for (const auto& arg : call->arguments) {
            collectCallExprs(arg.get(), calls);
        }
        return;
    }

    switch (expr->kind) {
        case gard::ExprKind::Binary: {
            auto* bin = static_cast<const gard::BinaryExpr*>(expr);
            collectCallExprs(bin->left.get(), calls);
            collectCallExprs(bin->right.get(), calls);
            break;
        }
        case gard::ExprKind::Unary: {
            auto* un = static_cast<const gard::UnaryExpr*>(expr);
            collectCallExprs(un->operand.get(), calls);
            break;
        }
        case gard::ExprKind::Ternary: {
            auto* tern = static_cast<const gard::TernaryExpr*>(expr);
            collectCallExprs(tern->condition.get(), calls);
            collectCallExprs(tern->thenExpr.get(), calls);
            collectCallExprs(tern->elseExpr.get(), calls);
            break;
        }
        case gard::ExprKind::Assignment: {
            auto* assign = static_cast<const gard::AssignmentExpr*>(expr);
            collectCallExprs(assign->target.get(), calls);
            collectCallExprs(assign->value.get(), calls);
            break;
        }
        case gard::ExprKind::MemberAccess: {
            auto* ma = static_cast<const gard::MemberAccessExpr*>(expr);
            collectCallExprs(ma->object.get(), calls);
            break;
        }
        case gard::ExprKind::IndexAccess: {
            auto* ia = static_cast<const gard::IndexAccessExpr*>(expr);
            collectCallExprs(ia->object.get(), calls);
            collectCallExprs(ia->index.get(), calls);
            break;
        }
        case gard::ExprKind::Await: {
            auto* aw = static_cast<const gard::AwaitExpr*>(expr);
            collectCallExprs(aw->operand.get(), calls);
            break;
        }
        case gard::ExprKind::Grouped: {
            auto* gr = static_cast<const gard::GroupedExpr*>(expr);
            collectCallExprs(gr->inner.get(), calls);
            break;
        }
        default:
            break;
    }
}

// Helper: walk statements recursively to find all call expressions
static void collectCallsFromStmts(const std::vector<gard::StmtPtr>& stmts,
                                   std::vector<const gard::CallExpr*>& calls);

static void collectCallsFromStmt(const gard::Statement* stmt,
                                  std::vector<const gard::CallExpr*>& calls) {
    if (!stmt) return;

    switch (stmt->kind) {
        case gard::StmtKind::Expression: {
            auto* es = static_cast<const gard::ExpressionStmt*>(stmt);
            collectCallExprs(es->expression.get(), calls);
            break;
        }
        case gard::StmtKind::VarDeclaration: {
            auto* vd = static_cast<const gard::VarDeclarationStmt*>(stmt);
            collectCallExprs(vd->initializer.get(), calls);
            break;
        }
        case gard::StmtKind::Return: {
            auto* ret = static_cast<const gard::ReturnStmt*>(stmt);
            collectCallExprs(ret->value.get(), calls);
            break;
        }
        case gard::StmtKind::If: {
            auto* ifs = static_cast<const gard::IfStmt*>(stmt);
            collectCallExprs(ifs->condition.get(), calls);
            collectCallsFromStmt(ifs->thenBranch.get(), calls);
            collectCallsFromStmt(ifs->elseBranch.get(), calls);
            break;
        }
        case gard::StmtKind::For: {
            auto* fs = static_cast<const gard::ForStmt*>(stmt);
            collectCallsFromStmt(fs->initializer.get(), calls);
            collectCallExprs(fs->condition.get(), calls);
            collectCallExprs(fs->increment.get(), calls);
            collectCallsFromStmt(fs->body.get(), calls);
            break;
        }
        case gard::StmtKind::While: {
            auto* ws = static_cast<const gard::WhileStmt*>(stmt);
            collectCallExprs(ws->condition.get(), calls);
            collectCallsFromStmt(ws->body.get(), calls);
            break;
        }
        case gard::StmtKind::Block: {
            auto* bs = static_cast<const gard::BlockStmt*>(stmt);
            collectCallsFromStmts(bs->statements, calls);
            break;
        }
        case gard::StmtKind::FunctionDeclaration: {
            auto* fn = static_cast<const gard::FunctionDeclStmt*>(stmt);
            collectCallsFromStmts(fn->body, calls);
            break;
        }
        case gard::StmtKind::Throw: {
            auto* th = static_cast<const gard::ThrowStmt*>(stmt);
            collectCallExprs(th->value.get(), calls);
            break;
        }
        case gard::StmtKind::Print: {
            auto* pr = static_cast<const gard::PrintStmt*>(stmt);
            collectCallExprs(pr->argument.get(), calls);
            break;
        }
        case gard::StmtKind::Export: {
            auto* exp = static_cast<const gard::ExportStmt*>(stmt);
            collectCallsFromStmt(exp->declaration.get(), calls);
            break;
        }
        case gard::StmtKind::Class: {
            auto* cls = static_cast<const gard::ClassDeclStmt*>(stmt);
            for (const auto& method : cls->methods) {
                collectCallsFromStmts(method.body, calls);
            }
            if (cls->constructor) {
                collectCallsFromStmts(cls->constructor->body, calls);
            }
            break;
        }
        default:
            break;
    }
}

static void collectCallsFromStmts(const std::vector<gard::StmtPtr>& stmts,
                                   std::vector<const gard::CallExpr*>& calls) {
    for (const auto& stmt : stmts) {
        collectCallsFromStmt(stmt.get(), calls);
    }
}

// Helper: get the function name from a callee expression
static std::string getCalleeName(const gard::Expression* callee) {
    if (!callee) return "";
    if (callee->kind == gard::ExprKind::Identifier) {
        return static_cast<const gard::IdentifierExpr*>(callee)->name;
    }
    if (callee->kind == gard::ExprKind::MemberAccess) {
        return static_cast<const gard::MemberAccessExpr*>(callee)->member;
    }
    return "";
}

// Helper: find function parameter names from AST
static std::vector<std::string> findFunctionParamNames(const DocumentState& doc,
                                                        const std::string& funcName) {
    std::vector<std::string> paramNames;

    for (const auto& stmt : doc.program.statements) {
        if (!stmt) continue;

        // Top-level functions
        gard::FunctionDeclStmt* fn = nullptr;
        if (stmt->kind == gard::StmtKind::FunctionDeclaration) {
            fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
        } else if (stmt->kind == gard::StmtKind::Export) {
            auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
            if (exp->declaration && exp->declaration->kind == gard::StmtKind::FunctionDeclaration) {
                fn = static_cast<gard::FunctionDeclStmt*>(exp->declaration.get());
            }
        }
        if (fn && fn->name == funcName) {
            for (const auto& p : fn->params) {
                paramNames.push_back(p.name);
            }
            return paramNames;
        }

        // Class methods
        gard::ClassDeclStmt* cls = nullptr;
        if (stmt->kind == gard::StmtKind::Class) {
            cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
        } else if (stmt->kind == gard::StmtKind::Export) {
            auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
            if (exp->declaration && exp->declaration->kind == gard::StmtKind::Class) {
                cls = static_cast<gard::ClassDeclStmt*>(exp->declaration.get());
            }
        }
        if (cls) {
            for (const auto& method : cls->methods) {
                if (method.name == funcName) {
                    for (const auto& p : method.params) {
                        paramNames.push_back(p.name);
                    }
                    return paramNames;
                }
            }
        }
    }

    return paramNames;
}

static void handleInlayHint(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);

    logMessage("inlayHint: " + uri);

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "[]");
        return;
    }

    DocumentState& doc = it->second;
    if (!doc.analysisValid || doc.program.statements.empty()) {
        sendResponse(id, "[]");
        return;
    }

    std::vector<std::string> hints;

    // Collect all call expressions from the AST
    std::vector<const gard::CallExpr*> calls;
    collectCallsFromStmts(doc.program.statements, calls);

    // For each call, try to show parameter names
    for (const auto* call : calls) {
        std::string funcName = getCalleeName(call->callee.get());
        if (funcName.empty()) continue;
        if (call->arguments.empty()) continue;

        // Find parameter names for this function
        std::vector<std::string> paramNames = findFunctionParamNames(doc, funcName);
        if (paramNames.empty()) continue;

        // Show parameter name hints before each argument
        for (size_t i = 0; i < call->arguments.size() && i < paramNames.size(); i++) {
            const auto& arg = call->arguments[i];
            if (!arg) continue;

            // Skip if the argument is already a simple identifier matching the param name
            if (arg->kind == gard::ExprKind::Identifier) {
                auto* ident = static_cast<const gard::IdentifierExpr*>(arg.get());
                if (ident->name == paramNames[i]) continue;
            }

            int hintLine = arg->location.line - 1;
            int hintChar = arg->location.column - 1;

            std::string hint = "{";
            hint += "\"position\":{\"line\":" + jsonInt(hintLine) + ",\"character\":" + jsonInt(hintChar) + "},";
            hint += "\"label\":" + jsonString(paramNames[i] + ":") + ",";
            hint += "\"kind\":2,"; // Parameter hint
            hint += "\"paddingRight\":true";
            hint += "}";
            hints.push_back(hint);
        }
    }

    // Build response
    std::string result = "[";
    for (size_t i = 0; i < hints.size(); i++) {
        if (i > 0) result += ",";
        result += hints[i];
    }
    result += "]";

    sendResponse(id, result);
}

// ============================================================================
// Tier 5: textDocument/foldingRange
// ============================================================================

static void handleFoldingRange(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);

    logMessage("foldingRange: " + uri);

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "[]");
        return;
    }

    DocumentState& doc = it->second;
    int totalLines = countLines(doc.content);
    std::vector<std::string> ranges;

    // Strategy 1: AST-based folding for functions, classes, interfaces, enums
    if (doc.analysisValid && !doc.program.statements.empty()) {
        for (const auto& stmt : doc.program.statements) {
            if (!stmt) continue;

            int startLine = stmt->location.line > 0 ? stmt->location.line - 1 : 0;
            int endLine = startLine;
            std::string kind = "region";

            switch (stmt->kind) {
                case gard::StmtKind::FunctionDeclaration: {
                    auto* fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
                    endLine = startLine + (int)fn->body.size() + 1;
                    if (endLine > totalLines) endLine = totalLines;
                    break;
                }
                case gard::StmtKind::Class: {
                    auto* cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                    endLine = startLine + (int)cls->fields.size() + (int)cls->methods.size() + 2;
                    if (endLine > totalLines) endLine = totalLines;
                    // Also add folding ranges for each method
                    for (const auto& method : cls->methods) {
                        int mStart = method.location.line > 0 ? method.location.line - 1 : startLine;
                        int mEnd = mStart + (int)method.body.size() + 1;
                        if (mEnd > totalLines) mEnd = totalLines;
                        if (mEnd > mStart) {
                            std::string mRange = "{\"startLine\":" + jsonInt(mStart) +
                                ",\"endLine\":" + jsonInt(mEnd) +
                                ",\"kind\":\"region\"}";
                            ranges.push_back(mRange);
                        }
                    }
                    break;
                }
                case gard::StmtKind::Interface: {
                    auto* iface = static_cast<gard::InterfaceDeclStmt*>(stmt.get());
                    endLine = startLine + (int)iface->methods.size() + 2;
                    if (endLine > totalLines) endLine = totalLines;
                    break;
                }
                case gard::StmtKind::Enum: {
                    auto* enm = static_cast<gard::EnumDeclStmt*>(stmt.get());
                    endLine = startLine + (int)enm->variants.size() + 2;
                    if (endLine > totalLines) endLine = totalLines;
                    break;
                }
                case gard::StmtKind::Export: {
                    auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                    if (exp->declaration) {
                        auto* inner = exp->declaration.get();
                        startLine = inner->location.line > 0 ? inner->location.line - 1 : startLine;
                        switch (inner->kind) {
                            case gard::StmtKind::FunctionDeclaration: {
                                auto* fn = static_cast<gard::FunctionDeclStmt*>(inner);
                                endLine = startLine + (int)fn->body.size() + 1;
                                break;
                            }
                            case gard::StmtKind::Class: {
                                auto* cls = static_cast<gard::ClassDeclStmt*>(inner);
                                endLine = startLine + (int)cls->fields.size() + (int)cls->methods.size() + 2;
                                break;
                            }
                            case gard::StmtKind::Interface: {
                                auto* iface = static_cast<gard::InterfaceDeclStmt*>(inner);
                                endLine = startLine + (int)iface->methods.size() + 2;
                                break;
                            }
                            case gard::StmtKind::Enum: {
                                auto* enm = static_cast<gard::EnumDeclStmt*>(inner);
                                endLine = startLine + (int)enm->variants.size() + 2;
                                break;
                            }
                            default: continue;
                        }
                        if (endLine > totalLines) endLine = totalLines;
                    } else {
                        continue;
                    }
                    break;
                }
                default:
                    continue;
            }

            if (endLine > startLine) {
                std::string range = "{\"startLine\":" + jsonInt(startLine) +
                    ",\"endLine\":" + jsonInt(endLine) +
                    ",\"kind\":" + jsonString(kind) + "}";
                ranges.push_back(range);
            }
        }
    }

    // Strategy 2: Token-based folding for multi-line comments
    // Scan tokens for DocComment that span multiple lines
    for (const auto& tok : doc.tokens) {
        if (tok.type == gard::TokenType::DocComment) {
            int commentStart = tok.location.line - 1;
            // Count newlines in the comment value to determine end line
            int newlines = 0;
            for (char c : tok.value) {
                if (c == '\n') newlines++;
            }
            if (newlines > 0) {
                int commentEnd = commentStart + newlines;
                std::string range = "{\"startLine\":" + jsonInt(commentStart) +
                    ",\"endLine\":" + jsonInt(commentEnd) +
                    ",\"kind\":\"comment\"}";
                ranges.push_back(range);
            }
        }
    }

    // Strategy 3: Import block folding
    // Find consecutive import statements and fold them
    int importStart = -1, importEnd = -1;
    for (const auto& stmt : doc.program.statements) {
        if (!stmt) continue;
        if (stmt->kind == gard::StmtKind::Import) {
            int line = stmt->location.line > 0 ? stmt->location.line - 1 : 0;
            if (importStart < 0) {
                importStart = line;
            }
            importEnd = line;
        } else if (importStart >= 0) {
            break; // imports are typically at the top
        }
    }
    if (importStart >= 0 && importEnd > importStart) {
        std::string range = "{\"startLine\":" + jsonInt(importStart) +
            ",\"endLine\":" + jsonInt(importEnd) +
            ",\"kind\":\"imports\"}";
        ranges.push_back(range);
    }

    // Build response
    std::string result = "[";
    for (size_t i = 0; i < ranges.size(); i++) {
        if (i > 0) result += ",";
        result += ranges[i];
    }
    result += "]";

    sendResponse(id, result);
}

