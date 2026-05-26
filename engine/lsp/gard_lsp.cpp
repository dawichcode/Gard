/**
 * Gard Language Server Protocol (LSP) - Tier 1 + Tier 2 + Tier 3 + Tier 4 + Tier 5
 *
 * A single-file LSP server providing diagnostics, hover, go-to-definition,
 * document symbols, formatting, completion, signature help, references,
 * rename, code actions, semantic tokens, workspace symbols, inlay hints,
 * and folding ranges for the Gard programming language.
 * Communicates via JSON-RPC over stdin/stdout with Content-Length framing.
 *
 * Supported methods:
 *   - initialize / initialized / shutdown / exit
 *   - textDocument/didOpen, didChange, didClose
 *   - textDocument/publishDiagnostics (server → client)
 *   - textDocument/hover
 *   - textDocument/definition
 *   - textDocument/documentSymbol
 *   - textDocument/formatting
 *   - textDocument/completion
 *   - textDocument/signatureHelp
 *   - textDocument/references
 *   - textDocument/rename
 *   - textDocument/prepareRename
 *   - textDocument/codeAction
 *   - textDocument/semanticTokens/full
 *   - workspace/symbol
 *   - textDocument/inlayHint
 *   - textDocument/foldingRange
 */

#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "sema/analyzer.h"
#include "sema/symbol.h"
#include "types/checker.h"
#include "tools/formatter.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <algorithm>

// ============================================================================
// Logging (stderr only — stdout is the LSP channel)
// ============================================================================

static void logMessage(const std::string& msg) {
    std::cerr << "[gard-lsp] " << msg << std::endl;
}

// ============================================================================
// JSON Utilities (manual — no external library)
// ============================================================================

static std::string jsonEscapeString(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

static std::string jsonString(const std::string& s) {
    return "\"" + jsonEscapeString(s) + "\"";
}

static std::string jsonInt(int n) {
    return std::to_string(n);
}

static std::string jsonBool(bool b) {
    return b ? "true" : "false";
}

// ============================================================================
// Simple JSON Parser (minimal — just enough for LSP messages)
// ============================================================================

// Extract a string value for a given key from a JSON object string
static std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos += searchKey.size();
    // Skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        pos++;

    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case '/':  result += '/'; break;
                default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

// Extract an integer value for a given key
static int jsonGetInt(const std::string& json, const std::string& key, int defaultVal = -1) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return defaultVal;

    pos += searchKey.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        pos++;

    if (pos >= json.size()) return defaultVal;

    // Check for null
    if (json.substr(pos, 4) == "null") return defaultVal;

    std::string numStr;
    bool negative = false;
    if (json[pos] == '-') { negative = true; pos++; }
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        numStr += json[pos];
        pos++;
    }
    if (numStr.empty()) return defaultVal;
    int val = std::stoi(numStr);
    return negative ? -val : val;
}

// Extract the "textDocument" → "uri" from params
static std::string jsonGetTextDocumentUri(const std::string& json) {
    // Find "textDocument" object, then find "uri" within it
    size_t tdPos = json.find("\"textDocument\"");
    if (tdPos == std::string::npos) return "";
    std::string sub = json.substr(tdPos);
    return jsonGetString(sub, "uri");
}

// Extract text content from didOpen/didChange params
// For didOpen: params.textDocument.text
// For didChange: params.contentChanges[0].text (full sync)
static std::string jsonGetDocumentText(const std::string& json, bool isChange) {
    if (isChange) {
        // Find contentChanges array, then get "text" from first element
        size_t ccPos = json.find("\"contentChanges\"");
        if (ccPos == std::string::npos) return "";
        std::string sub = json.substr(ccPos);
        return jsonGetString(sub, "text");
    } else {
        // didOpen: textDocument.text
        size_t tdPos = json.find("\"textDocument\"");
        if (tdPos == std::string::npos) return "";
        std::string sub = json.substr(tdPos);
        return jsonGetString(sub, "text");
    }
}

// Extract "position" → { "line": ..., "character": ... } from params
static void jsonGetPosition(const std::string& json, int& line, int& character) {
    size_t posPos = json.find("\"position\"");
    if (posPos == std::string::npos) {
        line = 0;
        character = 0;
        return;
    }
    std::string sub = json.substr(posPos);
    line = jsonGetInt(sub, "line", 0);
    character = jsonGetInt(sub, "character", 0);
}

// ============================================================================
// LSP Transport (Content-Length framing)
// ============================================================================

static std::string readMessage() {
    // Read headers until empty line
    int contentLength = -1;
    std::string line;

    while (true) {
        line.clear();
        int c;
        while ((c = std::getchar()) != EOF) {
            if (c == '\n') break;
            if (c != '\r') line += (char)c;
        }
        if (c == EOF) {
            logMessage("EOF on stdin, exiting");
            std::exit(0);
        }

        if (line.empty()) break; // end of headers

        // Parse Content-Length
        if (line.rfind("Content-Length:", 0) == 0) {
            std::string val = line.substr(15);
            // Trim whitespace
            size_t start = val.find_first_not_of(" \t");
            if (start != std::string::npos) val = val.substr(start);
            contentLength = std::stoi(val);
        }
    }

    if (contentLength <= 0) {
        logMessage("Invalid Content-Length: " + std::to_string(contentLength));
        return "";
    }

    // Read body
    std::string body(contentLength, '\0');
    size_t bytesRead = 0;
    while (bytesRead < (size_t)contentLength) {
        int c = std::getchar();
        if (c == EOF) {
            logMessage("EOF while reading body");
            std::exit(0);
        }
        body[bytesRead++] = (char)c;
    }

    return body;
}

static void sendMessage(const std::string& json) {
    std::string msg = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    std::cout << msg;
    std::cout.flush();
}

static void sendResponse(int id, const std::string& resultJson) {
    std::string response = "{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":" + resultJson + "}";
    sendMessage(response);
}

static void sendNotification(const std::string& method, const std::string& paramsJson) {
    std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":" + jsonString(method) + ",\"params\":" + paramsJson + "}";
    sendMessage(notification);
}

// ============================================================================
// Document State (Tier 2: stores parsed AST and symbol table per document)
// ============================================================================

struct DocumentState {
    std::string content;
    std::string uri;
    std::vector<gard::Token> tokens;
    gard::Program program;
    std::unique_ptr<gard::SemanticAnalyzer> sema;
    bool analysisValid = false; // true if tokens/program/sema are up-to-date

    DocumentState() = default;
    DocumentState(DocumentState&&) = default;
    DocumentState& operator=(DocumentState&&) = default;
};

static std::unordered_map<std::string, DocumentState> documents;

// ============================================================================
// Analysis Pipeline (shared between diagnostics and Tier 2 features)
// ============================================================================

// Re-analyze a document: lex, parse, semantic analysis, type check.
// Stores results in DocumentState for reuse by hover/definition/symbols.
static void analyzeDocument(DocumentState& doc, const std::string& filename) {
    doc.analysisValid = false;
    doc.tokens.clear();
    doc.program.statements.clear();
    doc.sema.reset();

    try {
        // Stage 1: Lexing
        gard::Lexer lexer(doc.content, filename);
        doc.tokens = lexer.tokenize();

        // Stage 2: Parsing
        gard::Parser parser(doc.tokens, filename);
        doc.program = parser.parse();

        // Stage 3: Semantic Analysis
        if (!parser.hasErrors() || !doc.program.statements.empty()) {
            doc.sema = std::make_unique<gard::SemanticAnalyzer>();
            doc.sema->analyze(doc.program);
            doc.analysisValid = true;
        }
    } catch (const std::exception& e) {
        logMessage("Exception during document analysis: " + std::string(e.what()));
    } catch (...) {
        logMessage("Unknown exception during document analysis");
    }
}

// ============================================================================
// Diagnostics Pipeline
// ============================================================================

struct LspDiagnostic {
    int startLine;
    int startChar;
    int endLine;
    int endChar;
    int severity; // 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string message;
    std::string source;
};

static std::string diagnosticToJson(const LspDiagnostic& d) {
    std::string json = "{";
    json += "\"range\":{";
    json += "\"start\":{\"line\":" + jsonInt(d.startLine) + ",\"character\":" + jsonInt(d.startChar) + "},";
    json += "\"end\":{\"line\":" + jsonInt(d.endLine) + ",\"character\":" + jsonInt(d.endChar) + "}";
    json += "},";
    json += "\"severity\":" + jsonInt(d.severity) + ",";
    json += "\"source\":" + jsonString(d.source) + ",";
    json += "\"message\":" + jsonString(d.message);
    json += "}";
    return json;
}

static int mapSeverity(gard::DiagSeverity sev) {
    switch (sev) {
        case gard::DiagSeverity::Error:   return 1;
        case gard::DiagSeverity::Warning: return 2;
        case gard::DiagSeverity::Info:    return 3;
        default: return 1;
    }
}

static void publishDiagnostics(const std::string& uri, DocumentState& doc) {
    std::vector<LspDiagnostic> diagnostics;

    // Extract filename from URI (strip file:// prefix)
    std::string filename = uri;
    if (filename.rfind("file://", 0) == 0) {
        filename = filename.substr(7);
    }

    try {
        // Stage 1: Lexing
        gard::Lexer lexer(doc.content, filename);
        std::vector<gard::Token> tokens = lexer.tokenize();

        if (lexer.hasErrors()) {
            for (const auto& err : lexer.getErrors()) {
                LspDiagnostic d;
                d.startLine = 0;
                d.startChar = 0;
                d.endLine = 0;
                d.endChar = 0;
                d.severity = 1;
                d.message = err;
                d.source = "gard-lexer";

                // Try to parse location from error string "file:line:col: message"
                size_t firstColon = err.find(':');
                if (firstColon != std::string::npos) {
                    size_t secondColon = err.find(':', firstColon + 1);
                    if (secondColon != std::string::npos) {
                        try {
                            std::string lineStr = err.substr(firstColon + 1, secondColon - firstColon - 1);
                            int line = std::stoi(lineStr);
                            d.startLine = line > 0 ? line - 1 : 0;
                            d.endLine = d.startLine;

                            size_t thirdColon = err.find(':', secondColon + 1);
                            if (thirdColon != std::string::npos) {
                                std::string colStr = err.substr(secondColon + 1, thirdColon - secondColon - 1);
                                int col = std::stoi(colStr);
                                d.startChar = col > 0 ? col - 1 : 0;
                                d.endChar = d.startChar + 1;
                                // Strip the "file:line:col: " prefix from the message
                                size_t msgStart = thirdColon + 1;
                                while (msgStart < err.size() && err[msgStart] == ' ') msgStart++;
                                if (msgStart < err.size()) {
                                    d.message = err.substr(msgStart);
                                }
                            }
                        } catch (...) {
                            // Keep defaults
                        }
                    }
                }

                diagnostics.push_back(d);
            }
        }

        // Stage 2: Parsing
        gard::Parser parser(tokens, filename);
        gard::Program program = parser.parse();

        if (parser.hasErrors()) {
            for (const auto& err : parser.getErrors()) {
                LspDiagnostic d;
                d.startLine = 0;
                d.startChar = 0;
                d.endLine = 0;
                d.endChar = 0;
                d.severity = 1;
                d.message = err;
                d.source = "gard-parser";

                size_t firstColon = err.find(':');
                if (firstColon != std::string::npos) {
                    size_t secondColon = err.find(':', firstColon + 1);
                    if (secondColon != std::string::npos) {
                        try {
                            std::string lineStr = err.substr(firstColon + 1, secondColon - firstColon - 1);
                            int line = std::stoi(lineStr);
                            d.startLine = line > 0 ? line - 1 : 0;
                            d.endLine = d.startLine;

                            size_t thirdColon = err.find(':', secondColon + 1);
                            if (thirdColon != std::string::npos) {
                                std::string colStr = err.substr(secondColon + 1, thirdColon - secondColon - 1);
                                int col = std::stoi(colStr);
                                d.startChar = col > 0 ? col - 1 : 0;
                                d.endChar = d.startChar + 1;
                                // Strip the "file:line:col: " prefix from the message
                                size_t msgStart = thirdColon + 1;
                                while (msgStart < err.size() && err[msgStart] == ' ') msgStart++;
                                if (msgStart < err.size()) {
                                    d.message = err.substr(msgStart);
                                }
                            }
                        } catch (...) {
                            // Keep defaults
                        }
                    }
                }

                diagnostics.push_back(d);
            }
        }

        // Stage 3: Semantic Analysis
        if (!parser.hasErrors() || !program.statements.empty()) {
            gard::SemanticAnalyzer sema;
            sema.analyze(program);

            for (const auto& diag : sema.getDiagnostics()) {
                LspDiagnostic d;
                d.startLine = diag.location.line > 0 ? diag.location.line - 1 : 0;
                d.startChar = diag.location.column > 0 ? diag.location.column - 1 : 0;
                d.endLine = d.startLine;
                d.endChar = d.startChar + 1;
                d.severity = mapSeverity(diag.severity);
                d.message = diag.message;
                d.source = "gard-sema";
                diagnostics.push_back(d);
            }

            // Stage 4: Type Checking
            gard::TypeChecker typeChecker(sema.getSymbolTable());
            typeChecker.check(program);

            for (const auto& diag : typeChecker.getDiagnostics()) {
                LspDiagnostic d;
                d.startLine = diag.location.line > 0 ? diag.location.line - 1 : 0;
                d.startChar = diag.location.column > 0 ? diag.location.column - 1 : 0;
                d.endLine = d.startLine;
                d.endChar = d.startChar + 1;
                d.severity = mapSeverity(diag.severity);
                d.message = diag.message;
                d.source = "gard-types";
                diagnostics.push_back(d);
            }
        }
    } catch (const std::exception& e) {
        logMessage("Exception during analysis: " + std::string(e.what()));
        LspDiagnostic d;
        d.startLine = 0;
        d.startChar = 0;
        d.endLine = 0;
        d.endChar = 0;
        d.severity = 1;
        d.message = std::string("Internal error: ") + e.what();
        d.source = "gard-lsp";
        diagnostics.push_back(d);
    } catch (...) {
        logMessage("Unknown exception during analysis");
    }

    // Build diagnostics array JSON
    std::string diagArray = "[";
    for (size_t i = 0; i < diagnostics.size(); i++) {
        if (i > 0) diagArray += ",";
        diagArray += diagnosticToJson(diagnostics[i]);
    }
    diagArray += "]";

    // Publish
    std::string params = "{\"uri\":" + jsonString(uri) + ",\"diagnostics\":" + diagArray + "}";
    sendNotification("textDocument/publishDiagnostics", params);

    logMessage("Published " + std::to_string(diagnostics.size()) + " diagnostics for " + uri);
}

// ============================================================================
// Tier 2: Token Lookup Helpers
// ============================================================================

// Find the token at a given LSP position (0-indexed line/character).
// Gard tokens use 1-indexed line/column.
static const gard::Token* findTokenAtPosition(const std::vector<gard::Token>& tokens,
                                               int lspLine, int lspChar) {
    int astLine = lspLine + 1;
    int astCol = lspChar + 1;

    for (const auto& tok : tokens) {
        if (tok.type == gard::TokenType::EndOfFile) continue;
        if (tok.location.line == astLine) {
            int tokStart = tok.location.column;
            int tokEnd = tokStart + (int)tok.value.size();
            if (astCol >= tokStart && astCol < tokEnd) {
                return &tok;
            }
        }
    }
    return nullptr;
}

// Get a keyword description for hover
static std::string getKeywordDescription(const std::string& keyword) {
    if (keyword == "let") return "Declares a mutable variable";
    if (keyword == "const") return "Declares an immutable constant";
    if (keyword == "var") return "Declares a mutable variable (legacy style)";
    if (keyword == "readonly") return "Declares a readonly variable";
    if (keyword == "function") return "Declares a function";
    if (keyword == "class") return "Declares a class";
    if (keyword == "interface") return "Declares an interface";
    if (keyword == "enum") return "Declares an enumeration";
    if (keyword == "if") return "Conditional branch";
    if (keyword == "else") return "Alternative branch of an if statement";
    if (keyword == "for") return "For loop";
    if (keyword == "foreach") return "For-each loop over an iterable";
    if (keyword == "while") return "While loop";
    if (keyword == "do") return "Do-while loop";
    if (keyword == "return") return "Returns a value from a function";
    if (keyword == "break") return "Exits the current loop or switch";
    if (keyword == "continue") return "Skips to the next loop iteration";
    if (keyword == "switch") return "Multi-way branch statement";
    if (keyword == "match") return "Pattern matching expression";
    if (keyword == "import") return "Imports symbols from a module";
    if (keyword == "export") return "Exports a declaration from a module";
    if (keyword == "async") return "Marks a function as asynchronous";
    if (keyword == "await") return "Awaits an asynchronous operation";
    if (keyword == "new") return "Creates a new instance of a class";
    if (keyword == "this") return "Reference to the current instance";
    if (keyword == "super") return "Reference to the parent class";
    if (keyword == "extends") return "Inherits from a base class";
    if (keyword == "implements") return "Implements an interface";
    if (keyword == "abstract") return "Marks a class or method as abstract";
    if (keyword == "static") return "Declares a static member";
    if (keyword == "public") return "Public access modifier";
    if (keyword == "private") return "Private access modifier";
    if (keyword == "protected") return "Protected access modifier";
    if (keyword == "try") return "Begins a try-catch block";
    if (keyword == "catch") return "Catches an exception";
    if (keyword == "finally") return "Executes after try/catch regardless of outcome";
    if (keyword == "throw") return "Throws an exception";
    if (keyword == "null") return "Null literal value";
    if (keyword == "true") return "Boolean true literal";
    if (keyword == "false") return "Boolean false literal";
    if (keyword == "void") return "Void type (no return value)";
    if (keyword == "int") return "32-bit signed integer type";
    if (keyword == "double") return "64-bit floating point type";
    if (keyword == "float") return "32-bit floating point type";
    if (keyword == "long") return "64-bit signed integer type";
    if (keyword == "short") return "16-bit signed integer type";
    if (keyword == "boolean") return "Boolean type (true/false)";
    if (keyword == "string") return "String type (UTF-8 text)";
    if (keyword == "char") return "Single character type";
    if (keyword == "blockchain") return "Blockchain contract declaration";
    if (keyword == "contract") return "Smart contract keyword";
    return "";
}

// Forward declaration (defined later in the file)
static std::string typeToString(const gard::TypeAnnotation* type);

// ============================================================================
// Tier 1: Local Variable Resolution
// ============================================================================

// Collect local variables from a function body that are visible at a given line
struct LocalVar {
    std::string name;
    std::string typeName;
    int line; // declaration line (1-indexed)
};

static std::vector<LocalVar> collectLocalsAtLine(const gard::Program& program, int targetLine) {
    std::vector<LocalVar> locals;
    // targetLine is 1-indexed (AST convention)
    
    // Walk all functions to find which one contains targetLine
    for (const auto& stmt : program.statements) {
        if (!stmt) continue;
        
        // Check function declarations
        gard::FunctionDeclStmt* fn = nullptr;
        if (stmt->kind == gard::StmtKind::FunctionDeclaration) {
            fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
        } else if (stmt->kind == gard::StmtKind::Export) {
            auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
            if (exp->declaration && exp->declaration->kind == gard::StmtKind::FunctionDeclaration) {
                fn = static_cast<gard::FunctionDeclStmt*>(exp->declaration.get());
            }
        }
        
        if (fn) {
            int fnStartLine = fn->location.line;
            // Estimate end: start + body size (rough)
            int fnEndLine = fnStartLine + (int)fn->body.size() + 5;
            
            if (targetLine >= fnStartLine && targetLine <= fnEndLine) {
                // Add function parameters as locals
                for (const auto& param : fn->params) {
                    std::string paramType = param.type ? typeToString(param.type.get()) : "any";
                    locals.push_back({param.name, paramType, fnStartLine});
                }
                // Walk body for variable declarations
                for (const auto& bodyStmt : fn->body) {
                    if (!bodyStmt) continue;
                    if (bodyStmt->kind == gard::StmtKind::VarDeclaration) {
                        auto* var = static_cast<gard::VarDeclarationStmt*>(bodyStmt.get());
                        if (var->location.line <= targetLine) {
                            std::string varType = var->type ? typeToString(var->type.get()) : "any";
                            // Try to infer from initializer if no explicit type
                            if (varType == "any" && var->initializer) {
                                if (dynamic_cast<gard::IntLiteralExpr*>(var->initializer.get())) varType = "int";
                                else if (dynamic_cast<gard::DoubleLiteralExpr*>(var->initializer.get())) varType = "double";
                                else if (dynamic_cast<gard::StringLiteralExpr*>(var->initializer.get())) varType = "string";
                                else if (dynamic_cast<gard::BoolLiteralExpr*>(var->initializer.get())) varType = "bool";
                                else if (auto* ne = dynamic_cast<gard::NewExpr*>(var->initializer.get())) { varType = ne->className; if (!ne->typeArgs.empty()) { varType += "<"; for (size_t ti = 0; ti < ne->typeArgs.size(); ti++) { if (ti > 0) varType += ", "; varType += typeToString(ne->typeArgs[ti].get()); } varType += ">"; } }
                                else if (dynamic_cast<gard::ArrayExpr*>(var->initializer.get())) varType = "array";
                            }
                            locals.push_back({var->name, varType, var->location.line});
                        }
                    }
                }
                break; // found the containing function
            }
        }
        
        // Also check class methods
        if (stmt->kind == gard::StmtKind::Class) {
            auto* cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
            for (const auto& method : cls->methods) {
                int mStart = method.location.line;
                int mEnd = mStart + (int)method.body.size() + 5;
                if (targetLine >= mStart && targetLine <= mEnd) {
                    // Add 'this' fields
                    for (const auto& field : cls->fields) {
                        std::string ft = field.type ? typeToString(field.type.get()) : "any";
                        locals.push_back({field.name, ft, cls->location.line});
                    }
                    // Add method params
                    for (const auto& param : method.params) {
                        std::string pt = param.type ? typeToString(param.type.get()) : "any";
                        locals.push_back({param.name, pt, mStart});
                    }
                    // Walk method body
                    for (const auto& bodyStmt : method.body) {
                        if (!bodyStmt) continue;
                        if (bodyStmt->kind == gard::StmtKind::VarDeclaration) {
                            auto* var = static_cast<gard::VarDeclarationStmt*>(bodyStmt.get());
                            if (var->location.line <= targetLine) {
                                std::string varType = var->type ? typeToString(var->type.get()) : "any";
                                if (varType == "any" && var->initializer) {
                                    if (dynamic_cast<gard::IntLiteralExpr*>(var->initializer.get())) varType = "int";
                                    else if (dynamic_cast<gard::DoubleLiteralExpr*>(var->initializer.get())) varType = "double";
                                    else if (dynamic_cast<gard::StringLiteralExpr*>(var->initializer.get())) varType = "string";
                                    else if (dynamic_cast<gard::BoolLiteralExpr*>(var->initializer.get())) varType = "bool";
                                    else if (auto* ne = dynamic_cast<gard::NewExpr*>(var->initializer.get())) { varType = ne->className; if (!ne->typeArgs.empty()) { varType += "<"; for (size_t ti = 0; ti < ne->typeArgs.size(); ti++) { if (ti > 0) varType += ", "; varType += typeToString(ne->typeArgs[ti].get()); } varType += ">"; } }
                                    else if (dynamic_cast<gard::ArrayExpr*>(var->initializer.get())) varType = "array";
                                }
                                locals.push_back({var->name, varType, var->location.line});
                            }
                        }
                    }
                    break;
                }
            }
        }
    }
    return locals;
}

// ============================================================================
// Tier 3: Stdlib Method Signatures Registry
// ============================================================================

struct StdlibParam {
    const char* name;
    const char* type;
};

struct StdlibMethod {
    const char* ns;        // namespace (e.g., "Math")
    const char* name;      // method name (e.g., "sqrt")
    const char* returnType;
    StdlibParam params[8]; // max 8 params, terminated by {nullptr,nullptr}
    const char* doc;       // one-line description
};

static const StdlibMethod STDLIB_METHODS[] = {
    // Math
    {"Math", "abs", "int", {{"x","int"},{nullptr,nullptr}}, "Returns absolute value"},
    {"Math", "max", "int", {{"a","int"},{"b","int"},{nullptr,nullptr}}, "Returns the larger of two values"},
    {"Math", "min", "int", {{"a","int"},{"b","int"},{nullptr,nullptr}}, "Returns the smaller of two values"},
    {"Math", "sqrt", "double", {{"x","double"},{nullptr,nullptr}}, "Returns the square root"},
    {"Math", "pow", "double", {{"base","double"},{"exp","double"},{nullptr,nullptr}}, "Returns base raised to exp"},
    {"Math", "round", "int", {{"x","double"},{nullptr,nullptr}}, "Rounds to nearest integer"},
    {"Math", "floor", "int", {{"x","double"},{nullptr,nullptr}}, "Rounds down to integer"},
    {"Math", "ceil", "int", {{"x","double"},{nullptr,nullptr}}, "Rounds up to integer"},
    {"Math", "sin", "double", {{"x","double"},{nullptr,nullptr}}, "Sine of angle in radians"},
    {"Math", "cos", "double", {{"x","double"},{nullptr,nullptr}}, "Cosine of angle in radians"},
    {"Math", "tan", "double", {{"x","double"},{nullptr,nullptr}}, "Tangent of angle in radians"},
    {"Math", "log", "double", {{"x","double"},{nullptr,nullptr}}, "Natural logarithm"},
    {"Math", "log2", "double", {{"x","double"},{nullptr,nullptr}}, "Base-2 logarithm"},
    {"Math", "log10", "double", {{"x","double"},{nullptr,nullptr}}, "Base-10 logarithm"},
    {"Math", "exp", "double", {{"x","double"},{nullptr,nullptr}}, "e raised to x"},
    {"Math", "random", "double", {{nullptr,nullptr}}, "Random number between 0 and 1"},
    {"Math", "randomInt", "int", {{"min","int"},{"max","int"},{nullptr,nullptr}}, "Random integer in range"},
    {"Math", "pi", "double", {{nullptr,nullptr}}, "Pi constant (3.14159...)"},
    {"Math", "e", "double", {{nullptr,nullptr}}, "Euler's number (2.71828...)"},
    {"Math", "clamp", "double", {{"val","double"},{"lo","double"},{"hi","double"},{nullptr,nullptr}}, "Clamp value to range"},
    {"Math", "sign", "int", {{"x","double"},{nullptr,nullptr}}, "Returns -1, 0, or 1"},
    {"Math", "cbrt", "double", {{"x","double"},{nullptr,nullptr}}, "Cube root"},
    {"Math", "atan2", "double", {{"y","double"},{"x","double"},{nullptr,nullptr}}, "Two-argument arctangent"},
    // String
    {"String", "length", "int", {{"s","string"},{nullptr,nullptr}}, "Returns string length"},
    {"String", "concat", "string", {{"a","string"},{"b","string"},{nullptr,nullptr}}, "Concatenates two strings"},
    {"String", "substring", "string", {{"s","string"},{"start","int"},{"end","int"},{nullptr,nullptr}}, "Extracts substring"},
    {"String", "indexOf", "int", {{"s","string"},{"sub","string"},{nullptr,nullptr}}, "First index of substring"},
    {"String", "toUpper", "string", {{"s","string"},{nullptr,nullptr}}, "Converts to uppercase"},
    {"String", "toLower", "string", {{"s","string"},{nullptr,nullptr}}, "Converts to lowercase"},
    {"String", "trim", "string", {{"s","string"},{nullptr,nullptr}}, "Removes leading/trailing whitespace"},
    {"String", "replace", "string", {{"s","string"},{"old","string"},{"new","string"},{nullptr,nullptr}}, "Replaces first occurrence"},
    {"String", "replaceAll", "string", {{"s","string"},{"old","string"},{"new","string"},{nullptr,nullptr}}, "Replaces all occurrences"},
    {"String", "split", "array", {{"s","string"},{"delimiter","string"},{nullptr,nullptr}}, "Splits string by delimiter"},
    {"String", "startsWith", "bool", {{"s","string"},{"prefix","string"},{nullptr,nullptr}}, "Checks if starts with prefix"},
    {"String", "endsWith", "bool", {{"s","string"},{"suffix","string"},{nullptr,nullptr}}, "Checks if ends with suffix"},
    {"String", "contains", "bool", {{"s","string"},{"sub","string"},{nullptr,nullptr}}, "Checks if contains substring"},
    {"String", "repeat", "string", {{"s","string"},{"count","int"},{nullptr,nullptr}}, "Repeats string n times"},
    // JSON
    {"JSON", "stringify", "string", {{"value","any"},{nullptr,nullptr}}, "Converts value to JSON string"},
    {"JSON", "parse", "any", {{"json","string"},{nullptr,nullptr}}, "Parses JSON string to value"},
    {"JSON", "isValid", "bool", {{"s","string"},{nullptr,nullptr}}, "Checks if string is valid JSON"},
    // File
    {"File", "readText", "string", {{"path","string"},{nullptr,nullptr}}, "Reads file as text"},
    {"File", "writeText", "bool", {{"path","string"},{"content","string"},{nullptr,nullptr}}, "Writes text to file"},
    {"File", "exists", "bool", {{"path","string"},{nullptr,nullptr}}, "Checks if file exists"},
    {"File", "size", "long", {{"path","string"},{nullptr,nullptr}}, "Returns file size in bytes"},
    {"File", "delete", "bool", {{"path","string"},{nullptr,nullptr}}, "Deletes a file"},
    {"File", "copy", "bool", {{"src","string"},{"dst","string"},{nullptr,nullptr}}, "Copies a file"},
    // Console
    {"Console", "readLine", "string", {{nullptr,nullptr}}, "Reads a line from stdin"},
    {"Console", "exec", "void", {{"cmd","string"},{nullptr,nullptr}}, "Executes a shell command"},
    {"Console", "write", "void", {{"text","string"},{nullptr,nullptr}}, "Writes text without newline"},
    {"Console", "writeLine", "void", {{"text","string"},{nullptr,nullptr}}, "Writes text with newline"},
    // Process
    {"Process", "pid", "int", {{nullptr,nullptr}}, "Returns current process ID"},
    {"Process", "platform", "string", {{nullptr,nullptr}}, "Returns OS platform name"},
    {"Process", "arch", "string", {{nullptr,nullptr}}, "Returns CPU architecture"},
    {"Process", "cwd", "string", {{nullptr,nullptr}}, "Returns current working directory"},
    {"Process", "env", "string", {{"name","string"},{nullptr,nullptr}}, "Gets environment variable"},
    {"Process", "exit", "void", {{"code","int"},{nullptr,nullptr}}, "Exits with status code"},
    {"Process", "execute", "string", {{"cmd","string"},{nullptr,nullptr}}, "Executes command and returns output"},
    // DateTime
    {"DateTime", "now", "long", {{nullptr,nullptr}}, "Current time in milliseconds"},
    {"DateTime", "format", "string", {{"epochMs","long"},{nullptr,nullptr}}, "Formats timestamp as string"},
    {"DateTime", "parse", "long", {{"str","string"},{nullptr,nullptr}}, "Parses date string to epoch ms"},
    // Array/List instance methods (called on objects)
    {"Array", "push", "void", {{"value","any"},{nullptr,nullptr}}, "Adds element to end"},
    {"Array", "pop", "any", {{nullptr,nullptr}}, "Removes and returns last element"},
    {"Array", "length", "int", {{nullptr,nullptr}}, "Returns array length"},
    {"Array", "get", "any", {{"index","int"},{nullptr,nullptr}}, "Gets element at index"},
    {"Array", "set", "void", {{"index","int"},{"value","any"},{nullptr,nullptr}}, "Sets element at index"},
    {"Array", "contains", "bool", {{"value","any"},{nullptr,nullptr}}, "Checks if array contains value"},
    {"Array", "indexOf", "int", {{"value","any"},{nullptr,nullptr}}, "First index of value"},
    {"Array", "sort", "void", {{nullptr,nullptr}}, "Sorts array in place"},
    {"Array", "reverse", "void", {{nullptr,nullptr}}, "Reverses array in place"},
    {"Array", "slice", "array", {{"start","int"},{"end","int"},{nullptr,nullptr}}, "Returns sub-array"},
    {"Array", "join", "string", {{"separator","string"},{nullptr,nullptr}}, "Joins elements with separator"},
    // Hash
    {"Hash", "sha256", "string", {{"data","string"},{nullptr,nullptr}}, "SHA-256 hash"},
    {"Hash", "sha512", "string", {{"data","string"},{nullptr,nullptr}}, "SHA-512 hash"},
    {"Hash", "sha1", "string", {{"data","string"},{nullptr,nullptr}}, "SHA-1 hash"},
    {"Hash", "hmacSha256", "string", {{"key","string"},{"data","string"},{nullptr,nullptr}}, "HMAC-SHA256"},
    // Regex
    {"Regex", "test", "bool", {{"pattern","string"},{"str","string"},{nullptr,nullptr}}, "Tests if pattern matches"},
    {"Regex", "match", "string", {{"pattern","string"},{"str","string"},{nullptr,nullptr}}, "Returns first match"},
    {"Regex", "replace", "string", {{"pattern","string"},{"str","string"},{"replacement","string"},{nullptr,nullptr}}, "Replaces first match"},
    {"Regex", "replaceAll", "string", {{"pattern","string"},{"str","string"},{"replacement","string"},{nullptr,nullptr}}, "Replaces all matches"},
    {"Regex", "split", "array", {{"pattern","string"},{"str","string"},{nullptr,nullptr}}, "Splits by pattern"},
    // Database
    {"Database", "connect", "ptr", {{"connectionString","string"},{nullptr,nullptr}}, "Connects to database"},
    {"Database", "execute", "void", {{"db","ptr"},{"sql","string"},{nullptr,nullptr}}, "Executes SQL statement"},
    {"Database", "query", "array", {{"db","ptr"},{"sql","string"},{nullptr,nullptr}}, "Queries and returns results"},
    {"Database", "close", "void", {{"db","ptr"},{nullptr,nullptr}}, "Closes database connection"},
    // Path
    {"Path", "join", "string", {{"a","string"},{"b","string"},{nullptr,nullptr}}, "Joins two path segments"},
    {"Path", "extension", "string", {{"path","string"},{nullptr,nullptr}}, "Returns file extension"},
    {"Path", "dirname", "string", {{"path","string"},{nullptr,nullptr}}, "Returns directory name"},
    {"Path", "basename", "string", {{"path","string"},{nullptr,nullptr}}, "Returns file name"},
    {"Path", "resolve", "string", {{"path","string"},{nullptr,nullptr}}, "Resolves to absolute path"},
    // Directory
    {"Directory", "create", "bool", {{"path","string"},{nullptr,nullptr}}, "Creates a directory"},
    {"Directory", "exists", "bool", {{"path","string"},{nullptr,nullptr}}, "Checks if directory exists"},
    {"Directory", "delete", "bool", {{"path","string"},{nullptr,nullptr}}, "Deletes a directory"},
    {"Directory", "list", "array", {{"path","string"},{nullptr,nullptr}}, "Lists directory contents"},
    // Base64
    {"Base64", "encode", "string", {{"data","string"},{nullptr,nullptr}}, "Encodes to Base64"},
    {"Base64", "decode", "string", {{"data","string"},{nullptr,nullptr}}, "Decodes from Base64"},
    {"Base64", "encodeUrlSafe", "string", {{"data","string"},{nullptr,nullptr}}, "URL-safe Base64 encode"},
    {"Base64", "decodeUrlSafe", "string", {{"data","string"},{nullptr,nullptr}}, "URL-safe Base64 decode"},
    // Compression
    {"Compression", "compress", "string", {{"data","string"},{nullptr,nullptr}}, "Compresses data"},
    {"Compression", "decompress", "string", {{"data","string"},{nullptr,nullptr}}, "Decompresses data"},
    {"Compression", "compressFile", "bool", {{"input","string"},{"output","string"},{nullptr,nullptr}}, "Compresses a file"},
    {"Compression", "decompressFile", "bool", {{"input","string"},{"output","string"},{nullptr,nullptr}}, "Decompresses a file"},
    // XML
    {"XML", "parse", "ptr", {{"xml","string"},{nullptr,nullptr}}, "Parses XML string to DOM"},
    {"XML", "createElement", "ptr", {{"tag","string"},{nullptr,nullptr}}, "Creates an XML element"},
    {"XML", "setAttribute", "void", {{"node","ptr"},{"key","string"},{"value","string"},{nullptr,nullptr}}, "Sets attribute on node"},
    {"XML", "setText", "void", {{"node","ptr"},{"text","string"},{nullptr,nullptr}}, "Sets text content"},
    {"XML", "appendChild", "void", {{"parent","ptr"},{"child","ptr"},{nullptr,nullptr}}, "Appends child node"},
    {"XML", "toString", "string", {{"node","ptr"},{nullptr,nullptr}}, "Serializes node to string"},
    {"XML", "getAttribute", "string", {{"node","ptr"},{"key","string"},{nullptr,nullptr}}, "Gets attribute value"},
    {"XML", "querySelector", "ptr", {{"root","ptr"},{"tag","string"},{nullptr,nullptr}}, "Finds first matching element"},
    // HttpClient
    {"HttpClient", "get", "ptr", {{"url","string"},{nullptr,nullptr}}, "HTTP GET request"},
    {"HttpClient", "post", "ptr", {{"url","string"},{"body","string"},{nullptr,nullptr}}, "HTTP POST request"},
    {"HttpClient", "put", "ptr", {{"url","string"},{"body","string"},{nullptr,nullptr}}, "HTTP PUT request"},
    {"HttpClient", "delete", "ptr", {{"url","string"},{nullptr,nullptr}}, "HTTP DELETE request"},
    {"HttpClient", "request", "ptr", {{"method","string"},{"url","string"},{"body","string"},{nullptr,nullptr}}, "Custom HTTP request"},
    // HttpServer
    {"HttpServer", "create", "ptr", {{nullptr,nullptr}}, "Creates an HTTP server"},
    {"HttpServer", "listen", "void", {{"server","ptr"},{"port","int"},{nullptr,nullptr}}, "Starts listening on port"},
    {"HttpServer", "route", "void", {{"server","ptr"},{"method","string"},{"path","string"},{nullptr,nullptr}}, "Registers a route"},
    {"HttpServer", "stop", "void", {{"server","ptr"},{nullptr,nullptr}}, "Stops the server"},
    // WebSocket
    {"WebSocket", "connect", "ptr", {{"url","string"},{nullptr,nullptr}}, "Connects to WebSocket server"},
    {"WebSocket", "send", "void", {{"ws","ptr"},{"message","string"},{nullptr,nullptr}}, "Sends a message"},
    {"WebSocket", "receive", "string", {{"ws","ptr"},{nullptr,nullptr}}, "Receives a message"},
    {"WebSocket", "close", "void", {{"ws","ptr"},{nullptr,nullptr}}, "Closes the connection"},
    // WebSocketServer
    {"WebSocketServer", "create", "ptr", {{"port","int"},{nullptr,nullptr}}, "Creates a WebSocket server"},
    {"WebSocketServer", "accept", "ptr", {{"server","ptr"},{nullptr,nullptr}}, "Accepts a client connection"},
    {"WebSocketServer", "send", "void", {{"client","ptr"},{"message","string"},{nullptr,nullptr}}, "Sends to client"},
    {"WebSocketServer", "broadcast", "void", {{"server","ptr"},{"message","string"},{nullptr,nullptr}}, "Broadcasts to all clients"},
    // TcpSocket
    {"TcpSocket", "connect", "ptr", {{"host","string"},{"port","int"},{nullptr,nullptr}}, "Connects to TCP server"},
    {"TcpSocket", "send", "void", {{"sock","ptr"},{"data","string"},{nullptr,nullptr}}, "Sends data"},
    {"TcpSocket", "receive", "string", {{"sock","ptr"},{nullptr,nullptr}}, "Receives data"},
    {"TcpSocket", "close", "void", {{"sock","ptr"},{nullptr,nullptr}}, "Closes socket"},
    // TcpServer
    {"TcpServer", "create", "ptr", {{"port","int"},{nullptr,nullptr}}, "Creates a TCP server"},
    {"TcpServer", "listen", "void", {{"server","ptr"},{nullptr,nullptr}}, "Starts listening"},
    {"TcpServer", "accept", "ptr", {{"server","ptr"},{nullptr,nullptr}}, "Accepts a connection"},
    {"TcpServer", "close", "void", {{"server","ptr"},{nullptr,nullptr}}, "Closes server"},
    // UdpSocket
    {"UdpSocket", "create", "ptr", {{nullptr,nullptr}}, "Creates a UDP socket"},
    {"UdpSocket", "bind", "void", {{"sock","ptr"},{"port","int"},{nullptr,nullptr}}, "Binds to port"},
    {"UdpSocket", "send", "void", {{"sock","ptr"},{"host","string"},{"port","int"},{"data","string"},{nullptr,nullptr}}, "Sends datagram"},
    {"UdpSocket", "receive", "string", {{"sock","ptr"},{nullptr,nullptr}}, "Receives datagram"},
    {"UdpSocket", "close", "void", {{"sock","ptr"},{nullptr,nullptr}}, "Closes socket"},
    // FFI
    {"FFI", "loadLibrary", "ptr", {{"path","string"},{nullptr,nullptr}}, "Loads a shared library"},
    {"FFI", "getSymbol", "ptr", {{"lib","ptr"},{"name","string"},{nullptr,nullptr}}, "Gets a symbol from library"},
    {"FFI", "call", "any", {{"sym","ptr"},{"retType","string"},{"argTypes","array"},{"args","array"},{nullptr,nullptr}}, "Calls a foreign function"},
    {"FFI", "closeLibrary", "void", {{"lib","ptr"},{nullptr,nullptr}}, "Closes a library"},
    {"FFI", "isLoaded", "bool", {{"lib","ptr"},{nullptr,nullptr}}, "Checks if library is loaded"},
    // Memory
    {"Memory", "alloc", "ptr", {{"size","int"},{nullptr,nullptr}}, "Allocates memory"},
    {"Memory", "free", "void", {{"ptr","ptr"},{nullptr,nullptr}}, "Frees memory"},
    {"Memory", "readInt32", "int", {{"ptr","ptr"},{"offset","int"},{nullptr,nullptr}}, "Reads 32-bit integer"},
    {"Memory", "writeInt32", "void", {{"ptr","ptr"},{"offset","int"},{"value","int"},{nullptr,nullptr}}, "Writes 32-bit integer"},
    {"Memory", "readInt64", "long", {{"ptr","ptr"},{"offset","int"},{nullptr,nullptr}}, "Reads 64-bit integer"},
    {"Memory", "writeInt64", "void", {{"ptr","ptr"},{"offset","int"},{"value","long"},{nullptr,nullptr}}, "Writes 64-bit integer"},
    {"Memory", "readString", "string", {{"ptr","ptr"},{"offset","int"},{nullptr,nullptr}}, "Reads null-terminated string"},
    {"Memory", "writeString", "void", {{"ptr","ptr"},{"offset","int"},{"value","string"},{nullptr,nullptr}}, "Writes string to memory"},
    {"Memory", "copy", "void", {{"dst","ptr"},{"dstOff","int"},{"src","ptr"},{"srcOff","int"},{"len","int"},{nullptr,nullptr}}, "Copies memory"},
    {"Memory", "fill", "void", {{"ptr","ptr"},{"offset","int"},{"value","int"},{"len","int"},{nullptr,nullptr}}, "Fills memory with value"},
    {"Memory", "size", "int", {{"ptr","ptr"},{nullptr,nullptr}}, "Returns allocation size"},
    {"Memory", "isNull", "bool", {{"ptr","ptr"},{nullptr,nullptr}}, "Checks if pointer is null"},
    // System
    {"System", "platform", "string", {{nullptr,nullptr}}, "Returns OS platform"},
    {"System", "arch", "string", {{nullptr,nullptr}}, "Returns CPU architecture"},
    {"System", "cpuCount", "int", {{nullptr,nullptr}}, "Returns number of CPU cores"},
    {"System", "memoryTotal", "long", {{nullptr,nullptr}}, "Returns total system memory"},
    {"System", "memoryFree", "long", {{nullptr,nullptr}}, "Returns free system memory"},
    // Compiler
    {"Compiler", "version", "string", {{nullptr,nullptr}}, "Returns compiler version"},
    {"Compiler", "isOptimized", "bool", {{nullptr,nullptr}}, "Returns true if release build"},
    // MMap
    {"MMap", "open", "ptr", {{"path","string"},{"size","long"},{nullptr,nullptr}}, "Opens memory-mapped file"},
    {"MMap", "read", "string", {{"m","ptr"},{"offset","int"},{"length","int"},{nullptr,nullptr}}, "Reads from mmap"},
    {"MMap", "write", "int", {{"m","ptr"},{"offset","int"},{"data","string"},{nullptr,nullptr}}, "Writes to mmap"},
    {"MMap", "sync", "void", {{"m","ptr"},{nullptr,nullptr}}, "Syncs mmap to disk"},
    {"MMap", "close", "void", {{"m","ptr"},{nullptr,nullptr}}, "Closes mmap"},
    // ConcurrentMap
    {"ConcurrentMap", "create", "ptr", {{nullptr,nullptr}}, "Creates a thread-safe map"},
    {"ConcurrentMap", "get", "any", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Gets value by key"},
    {"ConcurrentMap", "set", "void", {{"m","ptr"},{"key","string"},{"value","any"},{nullptr,nullptr}}, "Sets key-value pair"},
    {"ConcurrentMap", "has", "bool", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Checks if key exists"},
    {"ConcurrentMap", "remove", "bool", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Removes a key"},
    {"ConcurrentMap", "size", "int", {{"m","ptr"},{nullptr,nullptr}}, "Returns map size"},
    // Mutex
    {"Mutex", "create", "ptr", {{nullptr,nullptr}}, "Creates a mutex"},
    {"Mutex", "lock", "void", {{"m","ptr"},{nullptr,nullptr}}, "Locks the mutex"},
    {"Mutex", "unlock", "void", {{"m","ptr"},{nullptr,nullptr}}, "Unlocks the mutex"},
    {"Mutex", "free", "void", {{"m","ptr"},{nullptr,nullptr}}, "Frees the mutex"},
    // Semaphore
    {"Semaphore", "create", "ptr", {{"permits","int"},{nullptr,nullptr}}, "Creates a semaphore"},
    {"Semaphore", "acquire", "void", {{"s","ptr"},{nullptr,nullptr}}, "Acquires a permit"},
    {"Semaphore", "release", "void", {{"s","ptr"},{nullptr,nullptr}}, "Releases a permit"},
    {"Semaphore", "free", "void", {{"s","ptr"},{nullptr,nullptr}}, "Frees the semaphore"},
    // Barrier
    {"Barrier", "create", "ptr", {{"count","int"},{nullptr,nullptr}}, "Creates a barrier"},
    {"Barrier", "wait", "void", {{"b","ptr"},{nullptr,nullptr}}, "Waits at the barrier"},
    {"Barrier", "free", "void", {{"b","ptr"},{nullptr,nullptr}}, "Frees the barrier"},
    // RWLock
    {"RWLock", "create", "ptr", {{nullptr,nullptr}}, "Creates a read-write lock"},
    {"RWLock", "readLock", "void", {{"rw","ptr"},{nullptr,nullptr}}, "Acquires read lock"},
    {"RWLock", "readUnlock", "void", {{"rw","ptr"},{nullptr,nullptr}}, "Releases read lock"},
    {"RWLock", "writeLock", "void", {{"rw","ptr"},{nullptr,nullptr}}, "Acquires write lock"},
    {"RWLock", "writeUnlock", "void", {{"rw","ptr"},{nullptr,nullptr}}, "Releases write lock"},
    {"RWLock", "free", "void", {{"rw","ptr"},{nullptr,nullptr}}, "Frees the lock"},
    // Channel
    {"Channel", "create", "ptr", {{"capacity","int"},{nullptr,nullptr}}, "Creates a channel"},
    {"Channel", "send", "bool", {{"ch","ptr"},{"value","any"},{nullptr,nullptr}}, "Sends value to channel"},
    {"Channel", "receive", "any", {{"ch","ptr"},{nullptr,nullptr}}, "Receives value from channel"},
    {"Channel", "close", "void", {{"ch","ptr"},{nullptr,nullptr}}, "Closes the channel"},
    {"Channel", "isClosed", "bool", {{"ch","ptr"},{nullptr,nullptr}}, "Checks if channel is closed"},
    // Mock
    {"Mock", "create", "ptr", {{"name","string"},{nullptr,nullptr}}, "Creates a mock object"},
    {"Mock", "when", "void", {{"mock","ptr"},{"method","string"},{"returnValue","any"},{nullptr,nullptr}}, "Configures mock return"},
    {"Mock", "call", "any", {{"mock","ptr"},{"method","string"},{nullptr,nullptr}}, "Calls a mocked method"},
    {"Mock", "callCount", "int", {{"mock","ptr"},{"method","string"},{nullptr,nullptr}}, "Returns call count"},
    {"Mock", "calledOnce", "bool", {{"mock","ptr"},{"method","string"},{nullptr,nullptr}}, "Checks called exactly once"},
    {"Mock", "neverCalled", "bool", {{"mock","ptr"},{"method","string"},{nullptr,nullptr}}, "Checks never called"},
    {"Mock", "reset", "void", {{"mock","ptr"},{nullptr,nullptr}}, "Resets mock state"},
    // Reflect
    {"Reflect", "getAnnotations", "array", {{"className","string"},{nullptr,nullptr}}, "Gets class annotations"},
    {"Reflect", "hasAnnotation", "bool", {{"className","string"},{"annotation","string"},{nullptr,nullptr}}, "Checks for annotation"},
    {"Reflect", "getAnnotationArgs", "string", {{"className","string"},{"annotation","string"},{nullptr,nullptr}}, "Gets annotation args"},
    {"Reflect", "getMethodAnnotations", "array", {{"className","string"},{"method","string"},{nullptr,nullptr}}, "Gets method annotations"},
    {"Reflect", "getFieldAnnotations", "array", {{"className","string"},{"field","string"},{nullptr,nullptr}}, "Gets field annotations"},
    // Mail
    {"Mail", "createTransport", "ptr", {{"config","map"},{nullptr,nullptr}}, "Creates mail transport"},
    {"Mail", "send", "ptr", {{"transport","ptr"},{"message","map"},{nullptr,nullptr}}, "Sends an email"},
    // ORM
    {"ORM", "setConnection", "void", {{"db","ptr"},{nullptr,nullptr}}, "Sets the database connection"},
    {"ORM", "findById", "ptr", {{"db","ptr"},{"table","string"},{"id","int"},{nullptr,nullptr}}, "Finds record by ID"},
    {"ORM", "findAll", "array", {{"db","ptr"},{"table","string"},{nullptr,nullptr}}, "Finds all records"},
    // Query
    {"Query", "select", "ptr", {{"table","string"},{nullptr,nullptr}}, "Creates SELECT query"},
    {"Query", "insert", "ptr", {{"table","string"},{nullptr,nullptr}}, "Creates INSERT query"},
    {"Query", "update", "ptr", {{"table","string"},{nullptr,nullptr}}, "Creates UPDATE query"},
    {"Query", "delete", "ptr", {{"table","string"},{nullptr,nullptr}}, "Creates DELETE query"},
    {"Query", "where", "ptr", {{"query","ptr"},{"condition","string"},{nullptr,nullptr}}, "Adds WHERE clause"},
    {"Query", "set", "ptr", {{"query","ptr"},{"field","string"},{"value","string"},{nullptr,nullptr}}, "Sets field value"},
    {"Query", "execute", "ptr", {{"query","ptr"},{nullptr,nullptr}}, "Executes the query"},
    {"Query", "build", "string", {{"query","ptr"},{nullptr,nullptr}}, "Builds SQL string"},
    // ConnectionPool
    {"ConnectionPool", "create", "ptr", {{"config","map"},{nullptr,nullptr}}, "Creates connection pool"},
    {"ConnectionPool", "acquire", "ptr", {{"pool","ptr"},{nullptr,nullptr}}, "Acquires a connection"},
    {"ConnectionPool", "release", "void", {{"pool","ptr"},{"conn","ptr"},{nullptr,nullptr}}, "Releases a connection"},
    {"ConnectionPool", "destroy", "void", {{"pool","ptr"},{nullptr,nullptr}}, "Destroys the pool"},
    {"ConnectionPool", "stats", "map", {{"pool","ptr"},{nullptr,nullptr}}, "Returns pool statistics"},
    // Crypto
    {"Crypto", "generateKey", "string", {{"bits","int"},{nullptr,nullptr}}, "Generates random key"},
    {"Crypto", "encrypt", "string", {{"plaintext","string"},{"key","string"},{nullptr,nullptr}}, "Encrypts data"},
    {"Crypto", "decrypt", "string", {{"ciphertext","string"},{"key","string"},{nullptr,nullptr}}, "Decrypts data"},
    {"Crypto", "getRandomValues", "array", {{"size","int"},{nullptr,nullptr}}, "Gets random bytes"},
    // RSA
    {"RSA", "generateKeyPair", "string", {{"bits","int"},{nullptr,nullptr}}, "Generates RSA key pair"},
    {"RSA", "encrypt", "string", {{"data","string"},{"key","string"},{nullptr,nullptr}}, "RSA encrypt"},
    {"RSA", "decrypt", "string", {{"data","string"},{"key","string"},{nullptr,nullptr}}, "RSA decrypt"},
    {"RSA", "sign", "string", {{"data","string"},{"key","string"},{nullptr,nullptr}}, "RSA sign"},
    {"RSA", "verify", "bool", {{"data","string"},{"signature","string"},{"key","string"},{nullptr,nullptr}}, "RSA verify"},
    // List (instance methods)
    {"List", "get", "any", {{"list","ptr"},{"index","int"},{nullptr,nullptr}}, "Gets element at index"},
    {"List", "set", "void", {{"list","ptr"},{"index","int"},{"value","any"},{nullptr,nullptr}}, "Sets element at index"},
    {"List", "add", "void", {{"list","ptr"},{"value","any"},{nullptr,nullptr}}, "Adds element to end"},
    {"List", "remove", "bool", {{"list","ptr"},{"value","any"},{nullptr,nullptr}}, "Removes first occurrence"},
    {"List", "length", "int", {{"list","ptr"},{nullptr,nullptr}}, "Returns list length"},
    {"List", "contains", "bool", {{"list","ptr"},{"value","any"},{nullptr,nullptr}}, "Checks if contains value"},
    // Queue
    {"Queue", "enqueue", "void", {{"q","ptr"},{"value","any"},{nullptr,nullptr}}, "Adds to back of queue"},
    {"Queue", "dequeue", "any", {{"q","ptr"},{nullptr,nullptr}}, "Removes from front"},
    {"Queue", "peek", "any", {{"q","ptr"},{nullptr,nullptr}}, "Returns front without removing"},
    {"Queue", "isEmpty", "bool", {{"q","ptr"},{nullptr,nullptr}}, "Checks if queue is empty"},
    {"Queue", "size", "int", {{"q","ptr"},{nullptr,nullptr}}, "Returns queue size"},
    // Stack
    {"Stack", "push", "void", {{"s","ptr"},{"value","any"},{nullptr,nullptr}}, "Pushes onto stack"},
    {"Stack", "pop", "any", {{"s","ptr"},{nullptr,nullptr}}, "Pops from stack"},
    {"Stack", "peek", "any", {{"s","ptr"},{nullptr,nullptr}}, "Returns top without removing"},
    {"Stack", "isEmpty", "bool", {{"s","ptr"},{nullptr,nullptr}}, "Checks if stack is empty"},
    {"Stack", "size", "int", {{"s","ptr"},{nullptr,nullptr}}, "Returns stack size"},
    // Set
    {"Set", "add", "void", {{"s","ptr"},{"value","any"},{nullptr,nullptr}}, "Adds element to set"},
    {"Set", "remove", "bool", {{"s","ptr"},{"value","any"},{nullptr,nullptr}}, "Removes element"},
    {"Set", "contains", "bool", {{"s","ptr"},{"value","any"},{nullptr,nullptr}}, "Checks membership"},
    {"Set", "size", "int", {{"s","ptr"},{nullptr,nullptr}}, "Returns set size"},
    {"Set", "union", "ptr", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "Returns union of two sets"},
    {"Set", "intersection", "ptr", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "Returns intersection"},
    {"Set", "difference", "ptr", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "Returns difference"},
    // Map (additional)
    {"Map", "new", "ptr", {{nullptr,nullptr}}, "Creates a new map"},
    {"Map", "get", "any", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Gets value by key"},
    {"Map", "set", "void", {{"m","ptr"},{"key","string"},{"value","any"},{nullptr,nullptr}}, "Sets key-value pair"},
    {"Map", "has", "bool", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Checks if key exists"},
    {"Map", "remove", "bool", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Removes a key"},
    {"Map", "delete", "bool", {{"m","ptr"},{"key","string"},{nullptr,nullptr}}, "Deletes a key"},
    {"Map", "size", "int", {{"m","ptr"},{nullptr,nullptr}}, "Returns map size"},
    {"Map", "keys", "array", {{"m","ptr"},{nullptr,nullptr}}, "Returns all keys"},
    {"Map", "values", "array", {{"m","ptr"},{nullptr,nullptr}}, "Returns all values"},
    {"Map", "entries", "array", {{"m","ptr"},{nullptr,nullptr}}, "Returns key-value pairs"},
    // Window/Graphics
    {"Window", "create", "ptr", {{"title","string"},{"width","int"},{"height","int"},{nullptr,nullptr}}, "Creates a window"},
    {"Window", "show", "void", {{"w","ptr"},{nullptr,nullptr}}, "Shows the window"},
    {"Window", "close", "void", {{"w","ptr"},{nullptr,nullptr}}, "Closes the window"},
    {"Window", "pollEvents", "void", {{"w","ptr"},{nullptr,nullptr}}, "Polls window events"},
    {"Graphics", "createRenderer", "ptr", {{"window","ptr"},{nullptr,nullptr}}, "Creates a renderer"},
    {"Graphics", "clear", "void", {{"renderer","ptr"},{nullptr,nullptr}}, "Clears the screen"},
    {"Graphics", "present", "void", {{"renderer","ptr"},{nullptr,nullptr}}, "Presents the frame"},
    {"Input", "isKeyDown", "bool", {{"key","int"},{nullptr,nullptr}}, "Checks if key is pressed"},
    {"Input", "getMousePosition", "ptr", {{nullptr,nullptr}}, "Returns mouse position"},
    // Audio
    {"Audio", "init", "void", {{nullptr,nullptr}}, "Initializes audio system"},
    {"Audio", "loadSound", "ptr", {{"path","string"},{nullptr,nullptr}}, "Loads a sound file"},
    {"Audio", "loadMusic", "ptr", {{"path","string"},{nullptr,nullptr}}, "Loads a music file"},
    {"Audio", "playSound", "void", {{"sound","ptr"},{nullptr,nullptr}}, "Plays a sound"},
    {"Audio", "playMusic", "void", {{"music","ptr"},{nullptr,nullptr}}, "Plays music"},
    {"Audio", "stopMusic", "void", {{nullptr,nullptr}}, "Stops music playback"},
    {"Audio", "setMusicVolume", "void", {{"volume","float"},{nullptr,nullptr}}, "Sets music volume"},
    // Color
    {"Color", "rgb", "ptr", {{"r","int"},{"g","int"},{"b","int"},{nullptr,nullptr}}, "Creates RGB color"},
    {"Color", "rgba", "ptr", {{"r","int"},{"g","int"},{"b","int"},{"a","int"},{nullptr,nullptr}}, "Creates RGBA color"},
    {"Color", "hex", "ptr", {{"hex","string"},{nullptr,nullptr}}, "Creates color from hex string"},
    {"Color", "hsl", "ptr", {{"h","float"},{"s","float"},{"l","float"},{nullptr,nullptr}}, "Creates HSL color"},
    // Collision
    {"Collision", "rectRect", "bool", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "Rectangle-rectangle collision"},
    {"Collision", "circleCircle", "bool", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "Circle-circle collision"},
    {"Collision", "rectCircle", "bool", {{"rect","ptr"},{"circle","ptr"},{nullptr,nullptr}}, "Rectangle-circle collision"},
    {"Collision", "pointInRect", "bool", {{"x","int"},{"y","int"},{"rect","ptr"},{nullptr,nullptr}}, "Point in rectangle test"},
    {"Collision", "raycast", "ptr", {{"origin","ptr"},{"direction","ptr"},{"maxDist","float"},{nullptr,nullptr}}, "Raycast test"},
    // DOM
    {"DOM", "createElement", "ptr", {{"tag","string"},{nullptr,nullptr}}, "Creates a DOM element"},
    {"DOM", "getElementById", "ptr", {{"id","string"},{nullptr,nullptr}}, "Gets element by ID"},
    {"DOM", "querySelector", "ptr", {{"selector","string"},{nullptr,nullptr}}, "Queries DOM selector"},
    {"DOM", "setAttribute", "void", {{"el","ptr"},{"key","string"},{"value","string"},{nullptr,nullptr}}, "Sets element attribute"},
    {"DOM", "addEventListener", "void", {{"el","ptr"},{"event","string"},{"handler","ptr"},{nullptr,nullptr}}, "Adds event listener"},
    // EventEmitter
    {"EventEmitter", "create", "ptr", {{nullptr,nullptr}}, "Creates an event emitter"},
    {"EventEmitter", "on", "void", {{"emitter","ptr"},{"event","string"},{"handler","ptr"},{nullptr,nullptr}}, "Registers event handler"},
    {"EventEmitter", "emit", "void", {{"emitter","ptr"},{"event","string"},{"data","any"},{nullptr,nullptr}}, "Emits an event"},
    {"EventEmitter", "off", "void", {{"emitter","ptr"},{"event","string"},{nullptr,nullptr}}, "Removes event handler"},
    // Framebuffer
    {"Framebuffer", "create", "ptr", {{"width","int"},{"height","int"},{nullptr,nullptr}}, "Creates a framebuffer"},
    {"Framebuffer", "bind", "void", {{"fb","ptr"},{nullptr,nullptr}}, "Binds framebuffer for rendering"},
    {"Framebuffer", "unbind", "void", {{nullptr,nullptr}}, "Unbinds framebuffer"},
    {"Framebuffer", "getTexture", "ptr", {{"fb","ptr"},{nullptr,nullptr}}, "Gets framebuffer texture"},
    // Image
    {"Image", "load", "ptr", {{"path","string"},{nullptr,nullptr}}, "Loads an image file"},
    {"Image", "create", "ptr", {{"width","int"},{"height","int"},{nullptr,nullptr}}, "Creates blank image"},
    {"Image", "getPixel", "ptr", {{"img","ptr"},{"x","int"},{"y","int"},{nullptr,nullptr}}, "Gets pixel color"},
    {"Image", "setPixel", "void", {{"img","ptr"},{"x","int"},{"y","int"},{"color","ptr"},{nullptr,nullptr}}, "Sets pixel color"},
    {"Image", "save", "bool", {{"img","ptr"},{"path","string"},{nullptr,nullptr}}, "Saves image to file"},
    // Light
    {"Light", "createPoint", "ptr", {{"x","float"},{"y","float"},{"z","float"},{nullptr,nullptr}}, "Creates point light"},
    {"Light", "createDirectional", "ptr", {{"dx","float"},{"dy","float"},{"dz","float"},{nullptr,nullptr}}, "Creates directional light"},
    {"Light", "setColor", "void", {{"light","ptr"},{"r","float"},{"g","float"},{"b","float"},{nullptr,nullptr}}, "Sets light color"},
    {"Light", "setIntensity", "void", {{"light","ptr"},{"intensity","float"},{nullptr,nullptr}}, "Sets light intensity"},
    // Mesh
    {"Mesh", "create", "ptr", {{"vertices","array"},{"indices","array"},{nullptr,nullptr}}, "Creates a mesh"},
    {"Mesh", "createCube", "ptr", {{nullptr,nullptr}}, "Creates a cube mesh"},
    {"Mesh", "createSphere", "ptr", {{"radius","float"},{"segments","int"},{nullptr,nullptr}}, "Creates a sphere mesh"},
    {"Mesh", "draw", "void", {{"mesh","ptr"},{nullptr,nullptr}}, "Draws the mesh"},
    // Model
    {"Model", "load", "ptr", {{"path","string"},{nullptr,nullptr}}, "Loads a 3D model"},
    {"Model", "draw", "void", {{"model","ptr"},{nullptr,nullptr}}, "Draws the model"},
    {"Model", "setPosition", "void", {{"model","ptr"},{"x","float"},{"y","float"},{"z","float"},{nullptr,nullptr}}, "Sets model position"},
    {"Model", "setRotation", "void", {{"model","ptr"},{"rx","float"},{"ry","float"},{"rz","float"},{nullptr,nullptr}}, "Sets model rotation"},
    // OS
    {"OS", "platform", "string", {{nullptr,nullptr}}, "Returns OS name"},
    {"OS", "hostname", "string", {{nullptr,nullptr}}, "Returns hostname"},
    {"OS", "uptime", "long", {{nullptr,nullptr}}, "Returns system uptime in ms"},
    {"OS", "signal", "void", {{"signum","int"},{"action","string"},{nullptr,nullptr}}, "Sets signal handler"},
    {"OS", "tmpdir", "string", {{nullptr,nullptr}}, "Returns temp directory path"},
    // PriorityQueue
    {"PriorityQueue", "enqueue", "void", {{"pq","ptr"},{"value","any"},{nullptr,nullptr}}, "Adds with priority"},
    {"PriorityQueue", "dequeue", "any", {{"pq","ptr"},{nullptr,nullptr}}, "Removes highest priority"},
    {"PriorityQueue", "peek", "any", {{"pq","ptr"},{nullptr,nullptr}}, "Returns highest priority"},
    {"PriorityQueue", "isEmpty", "bool", {{"pq","ptr"},{nullptr,nullptr}}, "Checks if empty"},
    {"PriorityQueue", "size", "int", {{"pq","ptr"},{nullptr,nullptr}}, "Returns size"},
    // Shader
    {"Shader", "create", "ptr", {{"vertSrc","string"},{"fragSrc","string"},{nullptr,nullptr}}, "Creates shader program"},
    {"Shader", "use", "void", {{"shader","ptr"},{nullptr,nullptr}}, "Activates shader"},
    {"Shader", "setUniform", "void", {{"shader","ptr"},{"name","string"},{"value","any"},{nullptr,nullptr}}, "Sets uniform variable"},
    // SIMD
    {"SIMD", "float4", "ptr", {{"a","float"},{"b","float"},{"c","float"},{"d","float"},{nullptr,nullptr}}, "Creates float4 vector"},
    {"SIMD", "add", "ptr", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "SIMD vector add"},
    {"SIMD", "mul", "ptr", {{"a","ptr"},{"b","ptr"},{nullptr,nullptr}}, "SIMD vector multiply"},
    {"SIMD", "extractLane", "float", {{"v","ptr"},{"lane","int"},{nullptr,nullptr}}, "Extracts lane value"},
    // SpriteBatch
    {"SpriteBatch", "create", "ptr", {{nullptr,nullptr}}, "Creates a sprite batch"},
    {"SpriteBatch", "begin", "void", {{"batch","ptr"},{nullptr,nullptr}}, "Begins batching"},
    {"SpriteBatch", "draw", "void", {{"batch","ptr"},{"texture","ptr"},{"x","int"},{"y","int"},{nullptr,nullptr}}, "Draws sprite"},
    {"SpriteBatch", "end", "void", {{"batch","ptr"},{nullptr,nullptr}}, "Ends and flushes batch"},
    // Stream
    {"Stream", "create", "ptr", {{nullptr,nullptr}}, "Creates a data stream"},
    {"Stream", "write", "void", {{"s","ptr"},{"data","string"},{nullptr,nullptr}}, "Writes to stream"},
    {"Stream", "read", "string", {{"s","ptr"},{"size","int"},{nullptr,nullptr}}, "Reads from stream"},
    {"Stream", "pipe", "void", {{"src","ptr"},{"dst","ptr"},{nullptr,nullptr}}, "Pipes one stream to another"},
    {"Stream", "close", "void", {{"s","ptr"},{nullptr,nullptr}}, "Closes the stream"},
    // Texture
    {"Texture", "load", "ptr", {{"path","string"},{nullptr,nullptr}}, "Loads texture from file"},
    {"Texture", "create", "ptr", {{"width","int"},{"height","int"},{nullptr,nullptr}}, "Creates empty texture"},
    {"Texture", "bind", "void", {{"tex","ptr"},{nullptr,nullptr}}, "Binds texture for rendering"},
    // Timer
    {"Timer", "start", "long", {{nullptr,nullptr}}, "Starts high-resolution timer"},
    {"Timer", "elapsedMs", "long", {{"start","long"},{nullptr,nullptr}}, "Returns elapsed milliseconds"},
    {"Timer", "delay", "void", {{"ms","int"},{nullptr,nullptr}}, "Delays execution"},
    // Tween
    {"Tween", "create", "ptr", {{"from","float"},{"to","float"},{"duration","int"},{nullptr,nullptr}}, "Creates a tween"},
    {"Tween", "update", "float", {{"tween","ptr"},{"dt","float"},{nullptr,nullptr}}, "Updates tween, returns value"},
    {"Tween", "isComplete", "bool", {{"tween","ptr"},{nullptr,nullptr}}, "Checks if tween finished"},
    // Pointer (FFI)
    {"Pointer", "create", "ptr", {{"address","long"},{nullptr,nullptr}}, "Creates pointer from address"},
    {"Pointer", "deref", "any", {{"ptr","ptr"},{nullptr,nullptr}}, "Dereferences pointer"},
    {"Pointer", "offset", "ptr", {{"ptr","ptr"},{"bytes","int"},{nullptr,nullptr}}, "Offsets pointer"},
    {"Pointer", "isNull", "bool", {{"ptr","ptr"},{nullptr,nullptr}}, "Checks if null pointer"},
    // Wasm
    {"Wasm", "compile", "ptr", {{"source","string"},{nullptr,nullptr}}, "Compiles Wasm module"},
    {"Wasm", "instantiate", "ptr", {{"module","ptr"},{nullptr,nullptr}}, "Instantiates Wasm module"},
    {"WasmModule", "addFunction", "void", {{"module","ptr"},{"name","string"},{"fn","ptr"},{nullptr,nullptr}}, "Adds function to module"},
    {"WasmModule", "export", "ptr", {{"module","ptr"},{"name","string"},{nullptr,nullptr}}, "Gets exported function"},
    {"WasmModule", "compile", "ptr", {{"module","ptr"},{nullptr,nullptr}}, "Compiles the module"},
    {"WasmHeap", "alloc", "int", {{"heap","ptr"},{"size","int"},{nullptr,nullptr}}, "Allocates on Wasm heap"},
    {"WasmHeap", "free", "void", {{"heap","ptr"},{"offset","int"},{nullptr,nullptr}}, "Frees Wasm heap memory"},
    {"WasmHeap", "read", "int", {{"heap","ptr"},{"offset","int"},{nullptr,nullptr}}, "Reads from Wasm heap"},
    {"WasmHeap", "write", "void", {{"heap","ptr"},{"offset","int"},{"value","int"},{nullptr,nullptr}}, "Writes to Wasm heap"},
    {"WasmMemory", "create", "ptr", {{"pages","int"},{nullptr,nullptr}}, "Creates Wasm memory"},
    {"WasmMemory", "grow", "int", {{"mem","ptr"},{"pages","int"},{nullptr,nullptr}}, "Grows Wasm memory"},
    {"WasmMemory", "size", "int", {{"mem","ptr"},{nullptr,nullptr}}, "Returns memory size in pages"},
    // Canvas (2D drawing)
    {"Canvas", "create", "ptr", {{"width","int"},{"height","int"},{nullptr,nullptr}}, "Creates a canvas"},
    {"Canvas", "clear", "void", {{"canvas","ptr"},{nullptr,nullptr}}, "Clears the canvas"},
    {"Canvas", "fillRect", "void", {{"canvas","ptr"},{"x","int"},{"y","int"},{"w","int"},{"h","int"},{nullptr,nullptr}}, "Fills a rectangle"},
    {"Canvas", "strokeRect", "void", {{"canvas","ptr"},{"x","int"},{"y","int"},{"w","int"},{"h","int"},{nullptr,nullptr}}, "Strokes a rectangle"},
    {"Canvas", "drawCircle", "void", {{"canvas","ptr"},{"x","int"},{"y","int"},{"r","int"},{nullptr,nullptr}}, "Draws a circle"},
    {"Canvas", "drawLine", "void", {{"canvas","ptr"},{"x1","int"},{"y1","int"},{"x2","int"},{"y2","int"},{nullptr,nullptr}}, "Draws a line"},
    {"Canvas", "fillText", "void", {{"canvas","ptr"},{"text","string"},{"x","int"},{"y","int"},{nullptr,nullptr}}, "Draws text"},
    {"Canvas", "setFillStyle", "void", {{"canvas","ptr"},{"color","string"},{nullptr,nullptr}}, "Sets fill color"},
    {"Canvas", "setStrokeStyle", "void", {{"canvas","ptr"},{"color","string"},{nullptr,nullptr}}, "Sets stroke color"},
    {"Canvas", "setFont", "void", {{"canvas","ptr"},{"font","string"},{nullptr,nullptr}}, "Sets font"},
    // Camera (3D)
    {"Camera", "create", "ptr", {{"fov","float"},{"aspect","float"},{nullptr,nullptr}}, "Creates perspective camera"},
    {"Camera", "lookAt", "void", {{"cam","ptr"},{"x","float"},{"y","float"},{"z","float"},{nullptr,nullptr}}, "Points camera at target"},
    {"Camera", "move", "void", {{"cam","ptr"},{"dx","float"},{"dy","float"},{"dz","float"},{nullptr,nullptr}}, "Moves camera"},
    {"Camera", "rotate", "void", {{"cam","ptr"},{"yaw","float"},{"pitch","float"},{nullptr,nullptr}}, "Rotates camera"},
    {"Camera", "getViewMatrix", "ptr", {{"cam","ptr"},{nullptr,nullptr}}, "Returns view matrix"},
    {"Camera", "getProjectionMatrix", "ptr", {{"cam","ptr"},{nullptr,nullptr}}, "Returns projection matrix"},
    // ConcurrentList
    {"ConcurrentList", "create", "ptr", {{nullptr,nullptr}}, "Creates thread-safe list"},
    {"ConcurrentList", "add", "void", {{"list","ptr"},{"value","any"},{nullptr,nullptr}}, "Adds element"},
    {"ConcurrentList", "get", "any", {{"list","ptr"},{"index","int"},{nullptr,nullptr}}, "Gets element"},
    {"ConcurrentList", "size", "int", {{"list","ptr"},{nullptr,nullptr}}, "Returns size"},
    // Sentinel
    // Additional stdlib methods (auto-generated from test samples)
    {"Array", "compact", "void", {{nullptr,nullptr}}, "Compact operation"},
    {"Array", "parallelMap", "any", {{"value","any"},{nullptr,nullptr}}, "parallelMap"},
    {"Array", "parallelReduce", "any", {{"value","any"},{nullptr,nullptr}}, "parallelReduce"},
    {"Camera", "createOrtho", "any", {{"value","any"},{nullptr,nullptr}}, "createOrtho"},
    {"Canvas", "clearRect", "any", {{"value","any"},{nullptr,nullptr}}, "clearRect"},
    {"Canvas", "getCommands", "any", {{nullptr,nullptr}}, "Gets Commands"},
    {"Canvas", "setLineWidth", "void", {{"value","any"},{nullptr,nullptr}}, "Sets LineWidth"},
    {"Color", "BLACK", "any", {{nullptr,nullptr}}, "Constant BLACK"},
    {"Color", "BLUE", "any", {{nullptr,nullptr}}, "Constant BLUE"},
    {"Color", "GREEN", "any", {{nullptr,nullptr}}, "Constant GREEN"},
    {"Color", "RED", "any", {{nullptr,nullptr}}, "Constant RED"},
    {"Color", "TRANSPARENT", "any", {{nullptr,nullptr}}, "Constant TRANSPARENT"},
    {"Color", "WHITE", "any", {{nullptr,nullptr}}, "Constant WHITE"},
    {"Color", "valueOf", "any", {{"value","any"},{nullptr,nullptr}}, "valueOf"},
    {"Color", "values", "any", {{"value","any"},{nullptr,nullptr}}, "values"},
    {"Compiler", "optimizationLevel", "any", {{"value","any"},{nullptr,nullptr}}, "optimizationLevel"},
    {"ConnectionPool", "evict", "any", {{"value","any"},{nullptr,nullptr}}, "evict"},
    {"ConnectionPool", "isHealthy", "bool", {{nullptr,nullptr}}, "Checks isHealthy"},
    {"ConnectionPool", "resize", "any", {{"value","any"},{nullptr,nullptr}}, "resize"},
    {"Console", "color", "string", {{nullptr,nullptr}}, "Returns color"},
    {"Console", "execExitCode", "any", {{"value","any"},{nullptr,nullptr}}, "execExitCode"},
    {"Console", "execPipe", "any", {{"value","any"},{nullptr,nullptr}}, "execPipe"},
    {"Console", "execSilent", "any", {{"value","any"},{nullptr,nullptr}}, "execSilent"},
    {"Console", "execStdout", "any", {{"value","any"},{nullptr,nullptr}}, "execStdout"},
    {"Console", "execSuccess", "any", {{"value","any"},{nullptr,nullptr}}, "execSuccess"},
    {"Console", "execTimeout", "any", {{"value","any"},{nullptr,nullptr}}, "execTimeout"},
    {"Console", "execWithInput", "any", {{"value","any"},{nullptr,nullptr}}, "execWithInput"},
    {"Console", "isAvailable", "bool", {{nullptr,nullptr}}, "Checks isAvailable"},
    {"Console", "isInteractive", "bool", {{nullptr,nullptr}}, "Checks isInteractive"},
    {"Console", "progressBar", "any", {{"value","any"},{nullptr,nullptr}}, "progressBar"},
    {"Console", "size", "int", {{nullptr,nullptr}}, "Returns size"},
    {"Console", "spinner", "any", {{"value","any"},{nullptr,nullptr}}, "spinner"},
    {"Console", "table", "string", {{nullptr,nullptr}}, "Returns table"},
    {"Console", "which", "string", {{nullptr,nullptr}}, "Returns which"},
    {"DOM", "appendChild", "any", {{"value","any"},{nullptr,nullptr}}, "appendChild"},
    {"DOM", "cloneElement", "any", {{"value","any"},{nullptr,nullptr}}, "cloneElement"},
    {"DOM", "dispatchEvent", "any", {{"value","any"},{nullptr,nullptr}}, "dispatchEvent"},
    {"DOM", "getAttribute", "any", {{nullptr,nullptr}}, "Gets Attribute"},
    {"DOM", "getStyle", "any", {{nullptr,nullptr}}, "Gets Style"},
    {"DOM", "getTextContent", "any", {{nullptr,nullptr}}, "Gets TextContent"},
    {"DOM", "insertBefore", "any", {{"value","any"},{nullptr,nullptr}}, "insertBefore"},
    {"DOM", "querySelectorAll", "any", {{"value","any"},{nullptr,nullptr}}, "querySelectorAll"},
    {"DOM", "removeChild", "any", {{"value","any"},{nullptr,nullptr}}, "removeChild"},
    {"DOM", "removeEventListener", "any", {{"value","any"},{nullptr,nullptr}}, "removeEventListener"},
    {"DOM", "render", "any", {{"value","any"},{nullptr,nullptr}}, "render"},
    {"DOM", "setStyle", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Style"},
    {"DOM", "setTextContent", "void", {{"value","any"},{nullptr,nullptr}}, "Sets TextContent"},
    {"Database", "beginTransaction", "any", {{"value","any"},{nullptr,nullptr}}, "beginTransaction"},
    {"Database", "commit", "any", {{"value","any"},{nullptr,nullptr}}, "commit"},
    {"Database", "getDriver", "any", {{nullptr,nullptr}}, "Gets Driver"},
    {"Database", "getQueryCount", "any", {{nullptr,nullptr}}, "Gets QueryCount"},
    {"Database", "isConnected", "bool", {{nullptr,nullptr}}, "Checks isConnected"},
    {"Database", "rollback", "any", {{"value","any"},{nullptr,nullptr}}, "rollback"},
    {"Database", "run", "any", {{"value","any"},{nullptr,nullptr}}, "run"},
    {"DateTime", "addDays", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Days"},
    {"DateTime", "addHours", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Hours"},
    {"DateTime", "addMinutes", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Minutes"},
    {"DateTime", "addMonths", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Months"},
    {"DateTime", "addWeeks", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Weeks"},
    {"DateTime", "addYears", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Years"},
    {"DateTime", "equals", "any", {{"value","any"},{nullptr,nullptr}}, "equals"},
    {"DateTime", "fromEpoch", "long", {{nullptr,nullptr}}, "Returns fromEpoch"},
    {"DateTime", "getTimezone", "any", {{nullptr,nullptr}}, "Gets Timezone"},
    {"DateTime", "isAfter", "bool", {{nullptr,nullptr}}, "Checks isAfter"},
    {"DateTime", "isBefore", "bool", {{nullptr,nullptr}}, "Checks isBefore"},
    {"DateTime", "isExpired", "bool", {{nullptr,nullptr}}, "Checks isExpired"},
    {"DateTime", "subtract", "any", {{"value","any"},{nullptr,nullptr}}, "subtract"},
    {"DateTime", "toEpochMillis", "long", {{nullptr,nullptr}}, "Returns toEpochMillis"},
    {"DateTime", "utc", "any", {{"value","any"},{nullptr,nullptr}}, "utc"},
    {"EventEmitter", "getDeadLetters", "any", {{nullptr,nullptr}}, "Gets DeadLetters"},
    {"EventEmitter", "listenerCount", "any", {{"value","any"},{nullptr,nullptr}}, "listenerCount"},
    {"EventEmitter", "once", "void", {{"value","any"},{nullptr,nullptr}}, "Once operation"},
    {"EventEmitter", "setMaxListeners", "void", {{"value","any"},{nullptr,nullptr}}, "Sets MaxListeners"},
    {"FFI", "getFunction", "any", {{nullptr,nullptr}}, "Gets Function"},
    {"File", "appendText", "any", {{"value","any"},{nullptr,nullptr}}, "appendText"},
    {"Graphics", "clearColor", "any", {{"value","any"},{nullptr,nullptr}}, "clearColor"},
    {"Graphics", "drawCircle", "any", {{"value","any"},{nullptr,nullptr}}, "drawCircle"},
    {"Graphics", "drawLine", "any", {{"value","any"},{nullptr,nullptr}}, "drawLine"},
    {"Graphics", "fillCircle", "any", {{"value","any"},{nullptr,nullptr}}, "fillCircle"},
    {"Graphics", "fillRect", "any", {{"value","any"},{nullptr,nullptr}}, "fillRect"},
    {"Graphics", "setColor", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Color"},
    {"Hash", "hmac", "any", {{"value","any"},{nullptr,nullptr}}, "hmac"},
    {"Hash", "sha", "any", {{"value","any"},{nullptr,nullptr}}, "sha"},
    {"HttpResponse", "bytes", "any", {{"value","any"},{nullptr,nullptr}}, "bytes"},
    {"HttpResponse", "header", "string", {{nullptr,nullptr}}, "Returns header"},
    {"HttpResponse", "isClientError", "bool", {{nullptr,nullptr}}, "Checks isClientError"},
    {"HttpResponse", "isOk", "bool", {{nullptr,nullptr}}, "Checks isOk"},
    {"HttpResponse", "isRedirect", "bool", {{nullptr,nullptr}}, "Checks isRedirect"},
    {"HttpResponse", "isServerError", "bool", {{nullptr,nullptr}}, "Checks isServerError"},
    {"HttpResponse", "text", "string", {{nullptr,nullptr}}, "Returns text"},
    {"HttpServer", "delete", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"HttpServer", "get", "any", {{nullptr,nullptr}}, "Gets "},
    {"HttpServer", "getHost", "any", {{nullptr,nullptr}}, "Gets Host"},
    {"HttpServer", "getPort", "any", {{nullptr,nullptr}}, "Gets Port"},
    {"HttpServer", "isRunning", "bool", {{nullptr,nullptr}}, "Checks isRunning"},
    {"HttpServer", "patch", "any", {{"value","any"},{nullptr,nullptr}}, "patch"},
    {"HttpServer", "port", "int", {{nullptr,nullptr}}, "Returns port"},
    {"HttpServer", "post", "any", {{"value","any"},{nullptr,nullptr}}, "post"},
    {"HttpServer", "put", "any", {{"value","any"},{nullptr,nullptr}}, "put"},
    {"HttpServer", "setDefaultHeaders", "void", {{"value","any"},{nullptr,nullptr}}, "Sets DefaultHeaders"},
    {"HttpServer", "static", "any", {{"value","any"},{nullptr,nullptr}}, "static"},
    {"HttpServer", "use", "void", {{"value","any"},{nullptr,nullptr}}, "Use operation"},
    {"HttpStream", "isDone", "bool", {{nullptr,nullptr}}, "Checks isDone"},
    {"HttpStream", "read", "any", {{nullptr,nullptr}}, "Gets read"},
    {"Input", "isMouseDown", "bool", {{nullptr,nullptr}}, "Checks isMouseDown"},
    {"JSON", "prettyPrint", "any", {{"value","any"},{nullptr,nullptr}}, "prettyPrint"},
    {"JSON", "typeOf", "any", {{"value","any"},{nullptr,nullptr}}, "typeOf"},
    {"Light", "createSpot", "any", {{"value","any"},{nullptr,nullptr}}, "createSpot"},
    {"List", "every", "bool", {{"value","any"},{nullptr,nullptr}}, "Checks every"},
    {"List", "findIndex", "any", {{nullptr,nullptr}}, "Findindex operation"},
    {"List", "indexOf", "any", {{nullptr,nullptr}}, "Indexof operation"},
    {"List", "pop", "any", {{nullptr,nullptr}}, "Pop operation"},
    {"List", "reduce", "float", {{nullptr,nullptr}}, "Returns reduce"},
    {"List", "removeAt", "any", {{"value","any"},{nullptr,nullptr}}, "removeAt"},
    {"List", "reverse", "void", {{nullptr,nullptr}}, "Reverse operation"},
    {"List", "some", "bool", {{"value","any"},{nullptr,nullptr}}, "Checks some"},
    {"List", "sort", "void", {{nullptr,nullptr}}, "Sort operation"},
    {"Mail", "buildMessage", "any", {{"value","any"},{nullptr,nullptr}}, "buildMessage"},
    {"Mail", "createTemplate", "any", {{"value","any"},{nullptr,nullptr}}, "createTemplate"},
    {"Mail", "encodeAttachment", "any", {{"value","any"},{nullptr,nullptr}}, "encodeAttachment"},
    {"Mail", "validateEmail", "any", {{"value","any"},{nullptr,nullptr}}, "validateEmail"},
    {"Mail", "verify", "bool", {{"value","any"},{nullptr,nullptr}}, "Checks verify"},
    {"Math", "E", "any", {{nullptr,nullptr}}, "Constant E"},
    {"Math", "PI", "any", {{nullptr,nullptr}}, "Constant PI"},
    {"Math", "trunc", "any", {{"value","any"},{nullptr,nullptr}}, "trunc"},
    {"Memory", "addressOf", "void", {{"value","any"},{nullptr,nullptr}}, "Adds ressOf"},
    {"Memory", "allocAligned", "any", {{"value","any"},{nullptr,nullptr}}, "allocAligned"},
    {"Memory", "readBytes", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Bytes"},
    {"Memory", "readFloat", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Float"},
    {"Memory", "readInt", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Int"},
    {"Memory", "realloc", "any", {{"value","any"},{nullptr,nullptr}}, "realloc"},
    {"Memory", "sizeof", "any", {{"value","any"},{nullptr,nullptr}}, "sizeof"},
    {"Memory", "writeBytes", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Bytes"},
    {"Memory", "writeFloat", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Float"},
    {"Memory", "writeInt", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Int"},
    {"Mesh", "createPlane", "any", {{"value","any"},{nullptr,nullptr}}, "createPlane"},
    {"Mesh", "createQuad", "any", {{"value","any"},{nullptr,nullptr}}, "createQuad"},
    {"Mesh", "destroy", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Mesh", "drawInstanced", "any", {{"value","any"},{nullptr,nullptr}}, "drawInstanced"},
    {"Mock", "calledTimes", "any", {{"value","any"},{nullptr,nullptr}}, "calledTimes"},
    {"Mock", "verify", "bool", {{"value","any"},{nullptr,nullptr}}, "Checks verify"},
    {"Model", "destroy", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Model", "getBounds", "any", {{nullptr,nullptr}}, "Gets Bounds"},
    {"ORM", "attach", "any", {{"value","any"},{nullptr,nullptr}}, "attach"},
    {"ORM", "belongsTo", "any", {{"value","any"},{nullptr,nullptr}}, "belongsTo"},
    {"ORM", "createTable", "any", {{"value","any"},{nullptr,nullptr}}, "createTable"},
    {"ORM", "detach", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"ORM", "hasMany", "bool", {{nullptr,nullptr}}, "Checks hasMany"},
    {"ORM", "hasOne", "bool", {{nullptr,nullptr}}, "Checks hasOne"},
    {"ORM", "manyToMany", "any", {{"value","any"},{nullptr,nullptr}}, "manyToMany"},
    {"OS", "arch", "string", {{nullptr,nullptr}}, "Returns arch"},
    {"OS", "chdir", "any", {{"value","any"},{nullptr,nullptr}}, "chdir"},
    {"OS", "cpuCount", "any", {{"value","any"},{nullptr,nullptr}}, "cpuCount"},
    {"OS", "cpuInfo", "any", {{"value","any"},{nullptr,nullptr}}, "cpuInfo"},
    {"OS", "cwd", "string", {{nullptr,nullptr}}, "Returns cwd"},
    {"OS", "diskUsage", "any", {{"value","any"},{nullptr,nullptr}}, "diskUsage"},
    {"OS", "env", "any", {{"value","any"},{nullptr,nullptr}}, "env"},
    {"OS", "envAll", "any", {{"value","any"},{nullptr,nullptr}}, "envAll"},
    {"OS", "errno", "any", {{"value","any"},{nullptr,nullptr}}, "errno"},
    {"OS", "exec", "any", {{"value","any"},{nullptr,nullptr}}, "exec"},
    {"OS", "freeMemory", "any", {{"value","any"},{nullptr,nullptr}}, "freeMemory"},
    {"OS", "gid", "any", {{"value","any"},{nullptr,nullptr}}, "gid"},
    {"OS", "home", "string", {{nullptr,nullptr}}, "Returns home"},
    {"OS", "hrtime", "long", {{nullptr,nullptr}}, "Returns hrtime"},
    {"OS", "loadAvg", "any", {{"value","any"},{nullptr,nullptr}}, "loadAvg"},
    {"OS", "memoryUsage", "any", {{"value","any"},{nullptr,nullptr}}, "memoryUsage"},
    {"OS", "networkInterfaces", "any", {{"value","any"},{nullptr,nullptr}}, "networkInterfaces"},
    {"OS", "pid", "any", {{"value","any"},{nullptr,nullptr}}, "pid"},
    {"OS", "ppid", "any", {{"value","any"},{nullptr,nullptr}}, "ppid"},
    {"OS", "processExists", "any", {{"value","any"},{nullptr,nullptr}}, "processExists"},
    {"OS", "processes", "any", {{"value","any"},{nullptr,nullptr}}, "processes"},
    {"OS", "release", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"OS", "setEnv", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Env"},
    {"OS", "sleep", "any", {{"value","any"},{nullptr,nullptr}}, "sleep"},
    {"OS", "tempFile", "any", {{"value","any"},{nullptr,nullptr}}, "tempFile"},
    {"OS", "time", "long", {{nullptr,nullptr}}, "Returns time"},
    {"OS", "totalMemory", "any", {{"value","any"},{nullptr,nullptr}}, "totalMemory"},
    {"OS", "uid", "any", {{"value","any"},{nullptr,nullptr}}, "uid"},
    {"OS", "unsetEnv", "any", {{"value","any"},{nullptr,nullptr}}, "unsetEnv"},
    {"OS", "user", "string", {{nullptr,nullptr}}, "Returns user"},
    {"OS", "version", "string", {{nullptr,nullptr}}, "Returns version"},
    {"Pointer", "alloc", "ptr", {{nullptr,nullptr}}, "Creates/opens resource"},
    {"Pointer", "free", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Pointer", "readInt", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Int"},
    {"Pointer", "readString", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads String"},
    {"Pointer", "writeInt", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Int"},
    {"Pointer", "writeString", "void", {{"value","any"},{nullptr,nullptr}}, "Writes String"},
    {"Process", "args", "any", {{"value","any"},{nullptr,nullptr}}, "args"},
    {"Query", "deleteFrom", "any", {{"value","any"},{nullptr,nullptr}}, "deleteFrom"},
    {"Query", "join", "void", {{"value","any"},{nullptr,nullptr}}, "Join operation"},
    {"Query", "limit", "any", {{"value","any"},{nullptr,nullptr}}, "limit"},
    {"Query", "offset", "any", {{"value","any"},{nullptr,nullptr}}, "offset"},
    {"Query", "orderBy", "any", {{"value","any"},{nullptr,nullptr}}, "orderBy"},
    {"Query", "table", "string", {{nullptr,nullptr}}, "Returns table"},
    {"Query", "toSQL", "string", {{nullptr,nullptr}}, "Returns toSQL"},
    {"Regex", "matchGroups", "any", {{"value","any"},{nullptr,nullptr}}, "matchGroups"},
    {"Regex", "matches", "any", {{"value","any"},{nullptr,nullptr}}, "matches"},
    {"Regex", "testFlags", "any", {{"value","any"},{nullptr,nullptr}}, "testFlags"},
    {"Response", "body", "string", {{nullptr,nullptr}}, "Returns body"},
    {"Response", "status", "any", {{"value","any"},{nullptr,nullptr}}, "status"},
    {"SIMD", "Float", "any", {{"value","any"},{nullptr,nullptr}}, "Float"},
    {"SIMD", "Int", "any", {{"value","any"},{nullptr,nullptr}}, "Int"},
    {"SIMD", "div", "any", {{"value","any"},{nullptr,nullptr}}, "div"},
    {"SIMD", "dot", "float", {{nullptr,nullptr}}, "Returns dot"},
    {"SIMD", "eq", "any", {{"value","any"},{nullptr,nullptr}}, "eq"},
    {"SIMD", "load", "ptr", {{nullptr,nullptr}}, "Creates/opens resource"},
    {"SIMD", "max", "any", {{"value","any"},{nullptr,nullptr}}, "max"},
    {"SIMD", "min", "any", {{"value","any"},{nullptr,nullptr}}, "min"},
    {"SIMD", "replaceLane", "any", {{"value","any"},{nullptr,nullptr}}, "replaceLane"},
    {"SIMD", "splat", "any", {{"value","any"},{nullptr,nullptr}}, "splat"},
    {"SIMD", "store", "any", {{"value","any"},{nullptr,nullptr}}, "store"},
    {"SIMD", "sub", "any", {{"value","any"},{nullptr,nullptr}}, "sub"},
    {"Shader", "destroy", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Shader", "setTexture", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Texture"},
    {"Stream", "available", "any", {{"value","any"},{nullptr,nullptr}}, "available"},
    {"Stream", "batch", "any", {{"value","any"},{nullptr,nullptr}}, "batch"},
    {"Stream", "getStats", "any", {{nullptr,nullptr}}, "Gets Stats"},
    {"Stream", "isClosed", "bool", {{nullptr,nullptr}}, "Checks isClosed"},
    {"Stream", "isPaused", "bool", {{nullptr,nullptr}}, "Checks isPaused"},
    {"Stream", "merge", "any", {{"value","any"},{nullptr,nullptr}}, "merge"},
    {"Stream", "resume", "void", {{nullptr,nullptr}}, "Resume operation"},
    {"String", "charAt", "any", {{"value","any"},{nullptr,nullptr}}, "charAt"},
    {"String", "charCodeAt", "any", {{"value","any"},{nullptr,nullptr}}, "charCodeAt"},
    {"String", "lastIndexOf", "any", {{"value","any"},{nullptr,nullptr}}, "lastIndexOf"},
    {"String", "padEnd", "any", {{"value","any"},{nullptr,nullptr}}, "padEnd"},
    {"String", "padStart", "any", {{"value","any"},{nullptr,nullptr}}, "padStart"},
    {"String", "toInt", "any", {{"value","any"},{nullptr,nullptr}}, "toInt"},
    {"String", "toLowerCase", "any", {{"value","any"},{nullptr,nullptr}}, "toLowerCase"},
    {"String", "toUpperCase", "any", {{"value","any"},{nullptr,nullptr}}, "toUpperCase"},
    {"System", "memory", "any", {{"value","any"},{nullptr,nullptr}}, "memory"},
    {"System", "networkInterfaces", "any", {{"value","any"},{nullptr,nullptr}}, "networkInterfaces"},
    {"TcpServer", "port", "int", {{nullptr,nullptr}}, "Returns port"},
    {"TcpServer", "stop", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"TcpSocket", "isConnected", "bool", {{nullptr,nullptr}}, "Checks isConnected"},
    {"Texture", "destroy", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Texture", "setFilter", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Filter"},
    {"Texture", "setWrap", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Wrap"},
    {"Time", "delta", "float", {{nullptr,nullptr}}, "Returns delta"},
    {"Time", "elapsed", "float", {{nullptr,nullptr}}, "Returns elapsed"},
    {"Time", "fps", "int", {{nullptr,nullptr}}, "Returns fps"},
    {"Time", "setTargetFPS", "void", {{"value","any"},{nullptr,nullptr}}, "Sets TargetFPS"},
    {"Time", "tick", "void", {{nullptr,nullptr}}, "Tick operation"},
    {"Timer", "isActive", "bool", {{nullptr,nullptr}}, "Checks isActive"},
    {"Tween", "getValue", "any", {{nullptr,nullptr}}, "Gets Value"},
    {"UdpSocket", "port", "int", {{nullptr,nullptr}}, "Returns port"},
    {"Wasm", "addFunction", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Function"},
    {"Wasm", "addGlobal", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Global"},
    {"Wasm", "addImport", "void", {{"value","any"},{nullptr,nullptr}}, "Adds Import"},
    {"Wasm", "addString", "void", {{"value","any"},{nullptr,nullptr}}, "Adds String"},
    {"Wasm", "allocate", "ptr", {{nullptr,nullptr}}, "Creates/opens resource"},
    {"Wasm", "createModule", "any", {{"value","any"},{nullptr,nullptr}}, "createModule"},
    {"Wasm", "destroy", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Wasm", "exportFunction", "any", {{"value","any"},{nullptr,nullptr}}, "exportFunction"},
    {"Wasm", "exportMemory", "any", {{"value","any"},{nullptr,nullptr}}, "exportMemory"},
    {"Wasm", "getSize", "any", {{nullptr,nullptr}}, "Gets Size"},
    {"Wasm", "info", "string", {{nullptr,nullptr}}, "Returns info"},
    {"Wasm", "toWat", "string", {{nullptr,nullptr}}, "Returns toWat"},
    {"Wasm", "validate", "bool", {{"value","any"},{nullptr,nullptr}}, "Checks validate"},
    {"Wasm", "writeFile", "void", {{"value","any"},{nullptr,nullptr}}, "Writes File"},
    {"Wasm", "writeWat", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Wat"},
    {"WasmHeap", "availableBytes", "any", {{"value","any"},{nullptr,nullptr}}, "availableBytes"},
    {"WasmHeap", "compact", "void", {{nullptr,nullptr}}, "Compact operation"},
    {"WasmHeap", "create", "ptr", {{nullptr,nullptr}}, "Creates/opens resource"},
    {"WasmHeap", "detectLeaks", "any", {{"value","any"},{nullptr,nullptr}}, "detectLeaks"},
    {"WasmHeap", "refCount", "any", {{"value","any"},{nullptr,nullptr}}, "refCount"},
    {"WasmHeap", "release", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"WasmHeap", "reset", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"WasmHeap", "retain", "any", {{"value","any"},{nullptr,nullptr}}, "retain"},
    {"WasmHeap", "stats", "any", {{"value","any"},{nullptr,nullptr}}, "stats"},
    {"WasmHeap", "usedBytes", "any", {{"value","any"},{nullptr,nullptr}}, "usedBytes"},
    {"WasmInterop", "createHandleTable", "any", {{"value","any"},{nullptr,nullptr}}, "createHandleTable"},
    {"WasmInterop", "generateImports", "any", {{"value","any"},{nullptr,nullptr}}, "generateImports"},
    {"WasmInterop", "generateJSGlue", "any", {{"value","any"},{nullptr,nullptr}}, "generateJSGlue"},
    {"WasmInterop", "getHandle", "any", {{nullptr,nullptr}}, "Gets Handle"},
    {"WasmInterop", "marshalString", "any", {{"value","any"},{nullptr,nullptr}}, "marshalString"},
    {"WasmInterop", "releaseHandle", "any", {{"value","any"},{nullptr,nullptr}}, "releaseHandle"},
    {"WasmInterop", "storeHandle", "any", {{"value","any"},{nullptr,nullptr}}, "storeHandle"},
    {"WasmInterop", "unmarshalString", "any", {{"value","any"},{nullptr,nullptr}}, "unmarshalString"},
    {"WasmMemory", "allocate", "ptr", {{nullptr,nullptr}}, "Creates/opens resource"},
    {"WasmMemory", "copy", "any", {{"value","any"},{nullptr,nullptr}}, "copy"},
    {"WasmMemory", "fill", "any", {{"value","any"},{nullptr,nullptr}}, "fill"},
    {"WasmMemory", "free", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"WasmMemory", "readArray", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Array"},
    {"WasmMemory", "readByte", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Byte"},
    {"WasmMemory", "readFloat", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Float"},
    {"WasmMemory", "readInt", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads Int"},
    {"WasmMemory", "readString", "any", {{"offset","int"},{nullptr,nullptr}}, "Reads String"},
    {"WasmMemory", "writeArray", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Array"},
    {"WasmMemory", "writeByte", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Byte"},
    {"WasmMemory", "writeFloat", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Float"},
    {"WasmMemory", "writeInt", "void", {{"value","any"},{nullptr,nullptr}}, "Writes Int"},
    {"WasmMemory", "writeString", "void", {{"value","any"},{nullptr,nullptr}}, "Writes String"},
    {"WasmModule", "fromClass", "any", {{"value","any"},{nullptr,nullptr}}, "fromClass"},
    {"WasmModule", "instantiate", "ptr", {{nullptr,nullptr}}, "Creates/opens resource"},
    {"WasmOptimizer", "deadCodeElimination", "any", {{"value","any"},{nullptr,nullptr}}, "deadCodeElimination"},
    {"WasmOptimizer", "getStats", "any", {{nullptr,nullptr}}, "Gets Stats"},
    {"WasmOptimizer", "treeShake", "any", {{"value","any"},{nullptr,nullptr}}, "treeShake"},
    {"WasmThread", "atomicAdd", "any", {{"value","any"},{nullptr,nullptr}}, "atomicAdd"},
    {"WasmThread", "atomicCompareExchange", "any", {{"value","any"},{nullptr,nullptr}}, "atomicCompareExchange"},
    {"WasmThread", "atomicLoad", "any", {{"value","any"},{nullptr,nullptr}}, "atomicLoad"},
    {"WasmThread", "atomicStore", "any", {{"value","any"},{nullptr,nullptr}}, "atomicStore"},
    {"WasmThread", "atomicSub", "any", {{"value","any"},{nullptr,nullptr}}, "atomicSub"},
    {"WasmThread", "atomicWait", "any", {{"value","any"},{nullptr,nullptr}}, "atomicWait"},
    {"WasmThread", "createSharedMemory", "any", {{"value","any"},{nullptr,nullptr}}, "createSharedMemory"},
    {"WebSocket", "getProtocol", "any", {{nullptr,nullptr}}, "Gets Protocol"},
    {"WebSocket", "getReadyState", "any", {{nullptr,nullptr}}, "Gets ReadyState"},
    {"WebSocket", "isConnected", "bool", {{nullptr,nullptr}}, "Checks isConnected"},
    {"WebSocket", "ping", "any", {{"value","any"},{nullptr,nullptr}}, "ping"},
    {"WebSocketServer", "broadcastToRoom", "any", {{"value","any"},{nullptr,nullptr}}, "broadcastToRoom"},
    {"WebSocketServer", "disconnect", "any", {{"value","any"},{nullptr,nullptr}}, "disconnect"},
    {"WebSocketServer", "getClientCount", "any", {{nullptr,nullptr}}, "Gets ClientCount"},
    {"WebSocketServer", "getClientInfo", "any", {{nullptr,nullptr}}, "Gets ClientInfo"},
    {"WebSocketServer", "getClients", "any", {{nullptr,nullptr}}, "Gets Clients"},
    {"WebSocketServer", "getPort", "any", {{nullptr,nullptr}}, "Gets Port"},
    {"WebSocketServer", "getRoomClients", "any", {{nullptr,nullptr}}, "Gets RoomClients"},
    {"WebSocketServer", "getRooms", "any", {{nullptr,nullptr}}, "Gets Rooms"},
    {"WebSocketServer", "isRunning", "bool", {{nullptr,nullptr}}, "Checks isRunning"},
    {"WebSocketServer", "joinRoom", "any", {{"value","any"},{nullptr,nullptr}}, "joinRoom"},
    {"WebSocketServer", "leaveRoom", "any", {{"value","any"},{nullptr,nullptr}}, "leaveRoom"},
    {"WebSocketServer", "listen", "void", {{"value","any"},{nullptr,nullptr}}, "Listen operation"},
    {"WebSocketServer", "pingAll", "any", {{"value","any"},{nullptr,nullptr}}, "pingAll"},
    {"WebSocketServer", "pruneDeadClients", "any", {{"value","any"},{nullptr,nullptr}}, "pruneDeadClients"},
    {"WebSocketServer", "receive", "any", {{nullptr,nullptr}}, "Gets receive"},
    {"WebSocketServer", "sendTo", "any", {{"value","any"},{nullptr,nullptr}}, "sendTo"},
    {"WebSocketServer", "setMaxConnections", "void", {{"value","any"},{nullptr,nullptr}}, "Sets MaxConnections"},
    {"WebSocketServer", "setRateLimit", "void", {{"value","any"},{nullptr,nullptr}}, "Sets RateLimit"},
    {"WebSocketServer", "stop", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Window", "destroy", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"Window", "getSize", "any", {{nullptr,nullptr}}, "Gets Size"},
    {"Window", "height", "int", {{nullptr,nullptr}}, "Returns height"},
    {"Window", "isOpen", "bool", {{nullptr,nullptr}}, "Checks isOpen"},
    {"Window", "onEvent", "any", {{"value","any"},{nullptr,nullptr}}, "onEvent"},
    {"Window", "resize", "any", {{"value","any"},{nullptr,nullptr}}, "resize"},
    {"Window", "setTitle", "void", {{"value","any"},{nullptr,nullptr}}, "Sets Title"},
    {"Window", "width", "int", {{nullptr,nullptr}}, "Returns width"},
    {"XML", "evaluate", "any", {{"value","any"},{nullptr,nullptr}}, "evaluate"},
    {"XML", "free", "void", {{nullptr,nullptr}}, "Releases/removes resource"},
    {"XML", "setTextContent", "void", {{"value","any"},{nullptr,nullptr}}, "Sets TextContent"},
    // Tier 5: Runtime function coverage (auto-generated)
    {"Array", "add", "void", {{"value","any"},{nullptr,nullptr}}, "add"},
    {"Array", "addAll", "void", {{"value","any"},{nullptr,nullptr}}, "addAll"},
    {"Array", "filter", "any", {nullptr,nullptr}, "filter"},
    {"Array", "foreach", "any", {nullptr,nullptr}, "foreach"},
    {"Array", "map", "any", {nullptr,nullptr}, "map"},
    {"Array", "new", "ptr", {nullptr,nullptr}, "new"},
    {"Array", "reduce", "any", {nullptr,nullptr}, "reduce"},
    {"Array", "remove", "void", {{"value","any"},{nullptr,nullptr}}, "remove"},
    {"Array", "removeAt", "void", {{"value","any"},{nullptr,nullptr}}, "removeAt"},
    {"Async", "spawn", "ptr", {nullptr,nullptr}, "spawn"},
    {"Async", "spawnVoid", "ptr", {nullptr,nullptr}, "spawnVoid"},
    {"Barrier", "new", "ptr", {nullptr,nullptr}, "new"},
    {"Channel", "new", "ptr", {nullptr,nullptr}, "new"},
    {"ConcurrentMap", "new", "ptr", {nullptr,nullptr}, "new"},
    {"Console", "execFull", "any", {nullptr,nullptr}, "execFull"},
    {"Console", "execTimeoutFull", "any", {nullptr,nullptr}, "execTimeoutFull"},
    {"Console", "sizeFull", "any", {nullptr,nullptr}, "sizeFull"},
    {"CrashReport", "enable", "void", {nullptr,nullptr}, "enable"},
    {"CrashReport", "setDir", "void", {nullptr,nullptr}, "setDir"},
    {"CrashReport", "write", "void", {nullptr,nullptr}, "write"},
    {"File", "move", "any", {nullptr,nullptr}, "move"},
    {"Hash", "sha256Bytes", "any", {nullptr,nullptr}, "sha256Bytes"},
    {"Hash", "sha256File", "any", {nullptr,nullptr}, "sha256File"},
    {"Hash", "sha384", "any", {nullptr,nullptr}, "sha384"},
    {"JSON", "stringifyBool", "string", {nullptr,nullptr}, "stringifyBool"},
    {"JSON", "stringifyInt", "string", {nullptr,nullptr}, "stringifyInt"},
    {"JSON", "stringifyStr", "string", {nullptr,nullptr}, "stringifyStr"},
    {"List", "compact", "any", {nullptr,nullptr}, "compact"},
    {"List", "everyNonzero", "any", {nullptr,nullptr}, "everyNonzero"},
    {"List", "filterNonzero", "any", {nullptr,nullptr}, "filterNonzero"},
    {"List", "find", "any", {nullptr,nullptr}, "find"},
    {"List", "reduceSum", "any", {nullptr,nullptr}, "reduceSum"},
    {"List", "someNonzero", "any", {nullptr,nullptr}, "someNonzero"},
    {"Log", "debug", "any", {nullptr,nullptr}, "debug"},
    {"Log", "error", "any", {nullptr,nullptr}, "error"},
    {"Log", "fatal", "any", {nullptr,nullptr}, "fatal"},
    {"Log", "info", "any", {nullptr,nullptr}, "info"},
    {"Log", "setFormat", "void", {{"value","any"},{nullptr,nullptr}}, "setFormat"},
    {"Log", "setLevel", "void", {{"value","any"},{nullptr,nullptr}}, "setLevel"},
    {"Log", "setOutput", "void", {{"value","any"},{nullptr,nullptr}}, "setOutput"},
    {"Log", "trace", "any", {nullptr,nullptr}, "trace"},
    {"Log", "warn", "any", {nullptr,nullptr}, "warn"},
    {"Mail", "sendAuth", "any", {nullptr,nullptr}, "sendAuth"},
    {"Mail", "sendTransport", "any", {nullptr,nullptr}, "sendTransport"},
    {"Map", "free", "void", {nullptr,nullptr}, "free"},
    {"Math", "acos", "any", {nullptr,nullptr}, "acos"},
    {"Math", "asin", "any", {nullptr,nullptr}, "asin"},
    {"Math", "atan", "any", {nullptr,nullptr}, "atan"},
    {"Memory", "readFloat64", "any", {nullptr,nullptr}, "readFloat64"},
    {"Memory", "writeFloat64", "any", {nullptr,nullptr}, "writeFloat64"},
    {"Mutex", "new", "ptr", {nullptr,nullptr}, "new"},
    {"Reflect", "getAnnotationArgsReal", "any", {nullptr,nullptr}, "getAnnotationArgsReal"},
    {"Reflect", "getAnnotationsReal", "any", {nullptr,nullptr}, "getAnnotationsReal"},
    {"Reflect", "getMethodAnnotationsReal", "any", {nullptr,nullptr}, "getMethodAnnotationsReal"},
    {"Reflect", "hasAnnotationReal", "bool", {nullptr,nullptr}, "hasAnnotationReal"},
    {"RWLock", "new", "ptr", {nullptr,nullptr}, "new"},
    {"Semaphore", "new", "ptr", {nullptr,nullptr}, "new"},
    {"String", "equals", "any", {nullptr,nullptr}, "equals"},
    {"String", "slice", "any", {nullptr,nullptr}, "slice"},
    {"String", "trimEnd", "any", {nullptr,nullptr}, "trimEnd"},
    {"String", "trimStart", "any", {nullptr,nullptr}, "trimStart"},
    {"XML", "childCount", "any", {nullptr,nullptr}, "childCount"},
    {"XML", "getChild", "any", {nullptr,nullptr}, "getChild"},
    {"XML", "getTag", "any", {nullptr,nullptr}, "getTag"},
    {"XML", "getText", "any", {nullptr,nullptr}, "getText"},
    {"XML", "querySelectorAll", "any", {nullptr,nullptr}, "querySelectorAll"},
    {nullptr, nullptr, nullptr, {{nullptr,nullptr}}, nullptr}
}

;

// ============================================================================
// Tier 2: textDocument/hover
// ============================================================================

static void handleTextDocumentHover(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);

    logMessage("hover: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character));

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "null");
        return;
    }

    DocumentState& doc = it->second;
    if (doc.tokens.empty()) {
        sendResponse(id, "null");
        return;
    }

    // Find token at position
    const gard::Token* token = findTokenAtPosition(doc.tokens, line, character);
    if (!token) {
        sendResponse(id, "null");
        return;
    }

    std::string hoverContent;
    int rangeStartLine = token->location.line - 1;
    int rangeStartChar = token->location.column - 1;
    int rangeEndLine = rangeStartLine;
    int rangeEndChar = rangeStartChar + (int)token->value.size();

    if (token->type == gard::TokenType::Identifier) {
        // Look up in symbol table
        if (doc.sema && doc.analysisValid) {
            gard::Symbol* sym = doc.sema->getSymbolTable().resolve(token->value);
            if (sym) {
                // Build hover content based on symbol kind
                std::string declStr;
                switch (sym->kind) {
                    case gard::SymbolKind::Variable:
                        declStr = (sym->isConst ? "const " : "let ") + sym->name;
                        if (!sym->typeName.empty()) declStr += ": " + sym->typeName;
                        break;
                    case gard::SymbolKind::Function: {
                        declStr = (sym->isAsync ? "async " : "") + std::string("function ") + sym->name + "(";
                        for (size_t i = 0; i < sym->paramTypes.size(); i++) {
                            if (i > 0) declStr += ", ";
                            declStr += sym->paramTypes[i];
                        }
                        declStr += ")";
                        if (!sym->returnType.empty()) declStr += ": " + sym->returnType;
                        break;
                    }
                    case gard::SymbolKind::Class:
                        declStr = (sym->isAbstract ? "abstract " : "") + std::string("class ") + sym->name;
                        if (!sym->baseClass.empty()) declStr += " extends " + sym->baseClass;
                        if (!sym->interfaces.empty()) {
                            declStr += " implements ";
                            for (size_t i = 0; i < sym->interfaces.size(); i++) {
                                if (i > 0) declStr += ", ";
                                declStr += sym->interfaces[i];
                            }
                        }
                        break;
                    case gard::SymbolKind::Interface:
                        declStr = "interface " + sym->name;
                        break;
                    case gard::SymbolKind::Parameter:
                        declStr = sym->name;
                        if (!sym->typeName.empty()) declStr += ": " + sym->typeName;
                        declStr += " (parameter)";
                        break;
                    case gard::SymbolKind::Field:
                        declStr = sym->name;
                        if (!sym->typeName.empty()) declStr += ": " + sym->typeName;
                        declStr += " (field)";
                        break;
                    case gard::SymbolKind::Method: {
                        declStr = (sym->isAsync ? "async " : "") + sym->name + "(";
                        for (size_t i = 0; i < sym->paramTypes.size(); i++) {
                            if (i > 0) declStr += ", ";
                            declStr += sym->paramTypes[i];
                        }
                        declStr += ")";
                        if (!sym->returnType.empty()) declStr += ": " + sym->returnType;
                        declStr += " (method)";
                        break;
                    }
                    default:
                        declStr = sym->name;
                        if (!sym->typeName.empty()) declStr += ": " + sym->typeName;
                        break;
                }
                hoverContent = "```gard\\n" + declStr + "\\n```";
            }
        }

        // Tier 1: Try local variables if symbol table lookup failed
        if (hoverContent.empty() && doc.analysisValid) {
            int astLine = line + 1;
            auto locals = collectLocalsAtLine(doc.program, astLine);
            for (const auto& local : locals) {
                if (local.name == token->value) {
                    hoverContent = "```gard\\nlet " + local.name + ": " + local.typeName + "\\n```";
                    break;
                }
            }
        }

        // Tier 3: Try stdlib methods (e.g., hovering on "sqrt" after "Math.")
        if (hoverContent.empty() && doc.analysisValid) {
            for (int i = 0; STDLIB_METHODS[i].ns != nullptr; i++) {
                if (token->value == STDLIB_METHODS[i].name) {
                    std::string sig = std::string(STDLIB_METHODS[i].ns) + "." + STDLIB_METHODS[i].name + "(";
                    for (int p = 0; STDLIB_METHODS[i].params[p].name != nullptr; p++) {
                        if (p > 0) sig += ", ";
                        sig += std::string(STDLIB_METHODS[i].params[p].name) + ": " + STDLIB_METHODS[i].params[p].type;
                    }
                    sig += "): " + std::string(STDLIB_METHODS[i].returnType);
                    hoverContent = "```gard\\n" + sig + "\\n```\\n" + STDLIB_METHODS[i].doc;
                    break;
                }
            }
        }

        if (hoverContent.empty()) {
            // Unknown identifier — return null
            sendResponse(id, "null");
            return;
        }
    } else if (token->is(gard::TokenType::EndOfFile) || token->is(gard::TokenType::Invalid)) {
        sendResponse(id, "null");
        return;
    } else {
        // Check if it's a keyword
        std::string desc = getKeywordDescription(token->value);
        if (!desc.empty()) {
            hoverContent = "*keyword* `" + token->value + "` — " + desc;
        } else {
            // Not a keyword we have info for
            sendResponse(id, "null");
            return;
        }
    }

    // Build response
    std::string result = "{";
    result += "\"contents\":{\"kind\":\"markdown\",\"value\":" + jsonString(hoverContent) + "},";
    result += "\"range\":{";
    result += "\"start\":{\"line\":" + jsonInt(rangeStartLine) + ",\"character\":" + jsonInt(rangeStartChar) + "},";
    result += "\"end\":{\"line\":" + jsonInt(rangeEndLine) + ",\"character\":" + jsonInt(rangeEndChar) + "}";
    result += "}";
    result += "}";

    sendResponse(id, result);
}

// ============================================================================
// Tier 2: textDocument/definition
// ============================================================================

static void handleTextDocumentDefinition(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);

    logMessage("definition: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character));

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "null");
        return;
    }

    DocumentState& doc = it->second;
    if (doc.tokens.empty() || !doc.sema || !doc.analysisValid) {
        sendResponse(id, "null");
        return;
    }

    // Find token at position
    const gard::Token* token = findTokenAtPosition(doc.tokens, line, character);
    if (!token || token->type != gard::TokenType::Identifier) {
        sendResponse(id, "null");
        return;
    }

    // Look up symbol
    gard::Symbol* sym = doc.sema->getSymbolTable().resolve(token->value);
    if (!sym) {
        // Tier 5: Cross-file navigation — check if this is an imported symbol
        // Look through import statements in the AST to find where this symbol comes from
        for (const auto& stmt : doc.program.statements) {
            if (!stmt || stmt->kind != gard::StmtKind::Import) continue;
            auto* imp = static_cast<gard::ImportStmt*>(stmt.get());
            
            // Check if this import brings in the symbol we're looking for
            for (const auto& imported : imp->names) {
                if (imported == token->value) {
                    // Found it — resolve the module path
                    std::string modulePath = imp->path;
                    
                    // For relative imports (./file or ../file), resolve to actual file
                    if (modulePath.size() > 0 && (modulePath[0] == '.' || modulePath[0] == '/')) {
                        // Get the directory of the current file
                        std::string currentFile = uri;
                        if (currentFile.rfind("file://", 0) == 0) currentFile = currentFile.substr(7);
                        size_t lastSlash = currentFile.rfind('/');
                        std::string dir = (lastSlash != std::string::npos) ? currentFile.substr(0, lastSlash) : ".";
                        
                        // Resolve the relative path
                        std::string targetFile = dir + "/" + modulePath;
                        if (targetFile.find(".gard") == std::string::npos) targetFile += ".gard";
                        
                        // Check if this file is already open in the LSP
                        std::string targetUri2 = "file://" + targetFile;
                        auto targetIt = documents.find(targetUri2);
                        if (targetIt != documents.end()) {
                            // Search the target file's AST for the exported symbol
                            for (const auto& tStmt : targetIt->second.program.statements) {
                                if (!tStmt) continue;
                                if (tStmt->kind == gard::StmtKind::Export) {
                                    auto* exp = static_cast<gard::ExportStmt*>(tStmt.get());
                                    if (exp->declaration) {
                                        std::string expName;
                                        if (exp->declaration->kind == gard::StmtKind::FunctionDeclaration)
                                            expName = static_cast<gard::FunctionDeclStmt*>(exp->declaration.get())->name;
                                        else if (exp->declaration->kind == gard::StmtKind::Class)
                                            expName = static_cast<gard::ClassDeclStmt*>(exp->declaration.get())->name;
                                        else if (exp->declaration->kind == gard::StmtKind::VarDeclaration)
                                            expName = static_cast<gard::VarDeclarationStmt*>(exp->declaration.get())->name;
                                        
                                        if (expName == token->value) {
                                            int defLine2 = exp->declaration->location.line > 0 ? exp->declaration->location.line - 1 : 0;
                                            int defCol2 = exp->declaration->location.column > 0 ? exp->declaration->location.column - 1 : 0;
                                           std::string result = "{" + jsonString("uri") + ":" + jsonString(targetUri2) + "," + jsonString("range") + ":{";
                                            result += jsonString("start") + ":{" + jsonString("line") + ":" + jsonInt(defLine2) + "," + jsonString("character") + ":" + jsonInt(defCol2) + "},";
                                            result += jsonString("end") + ":{" + jsonString("line") + ":" + jsonInt(defLine2) + "," + jsonString("character") + ":" + jsonInt(defCol2 + (int)expName.size()) + "}";
                                            sendResponse(id, result);
                                            return;
                                        }
                                    }
                                }
                            }
                        }
                        
                        // File not open — try to read and parse it
                        FILE* f = fopen(targetFile.c_str(), "r");
                        if (f) {
                            fseek(f, 0, SEEK_END);
                            long sz = ftell(f);
                            fseek(f, 0, SEEK_SET);
                            std::string content(sz, '\0');
                            size_t rd = fread(&content[0], 1, sz, f);
                            content.resize(rd);
                            fclose(f);
                            
                            try {
                                gard::Lexer lexer(content, targetFile);
                                auto tokens2 = lexer.tokenize();
                                gard::Parser parser(tokens2, targetFile);
                                auto program2 = parser.parse();
                                
                                for (const auto& tStmt : program2.statements) {
                                    if (!tStmt) continue;
                                    if (tStmt->kind == gard::StmtKind::Export) {
                                        auto* exp = static_cast<gard::ExportStmt*>(tStmt.get());
                                        if (exp->declaration) {
                                            std::string expName;
                                            if (exp->declaration->kind == gard::StmtKind::FunctionDeclaration)
                                                expName = static_cast<gard::FunctionDeclStmt*>(exp->declaration.get())->name;
                                            else if (exp->declaration->kind == gard::StmtKind::Class)
                                                expName = static_cast<gard::ClassDeclStmt*>(exp->declaration.get())->name;
                                            else if (exp->declaration->kind == gard::StmtKind::VarDeclaration)
                                                expName = static_cast<gard::VarDeclarationStmt*>(exp->declaration.get())->name;
                                            
                                            if (expName == token->value) {
                                                int defLine2 = exp->declaration->location.line > 0 ? exp->declaration->location.line - 1 : 0;
                                                int defCol2 = exp->declaration->location.column > 0 ? exp->declaration->location.column - 1 : 0;
                                               std::string result = "{\"uri\":" + jsonString(targetUri2) + ",\"range\":{";
                                                result += "\"start\":{\"line\":" + jsonInt(defLine2) + ",\"character\":" + jsonInt(defCol2) + "},";
                                                result += "\"end\":{\"line\":" + jsonInt(defLine2) + ",\"character\":" + jsonInt(defCol2 + (int)expName.size()) + "}";
                                                sendResponse(id, result);
                                                return;
                                            }
                                        }
                                    }
                                    // Also check non-exported top-level declarations
                                    std::string declName;
                                    if (tStmt->kind == gard::StmtKind::FunctionDeclaration)
                                        declName = static_cast<gard::FunctionDeclStmt*>(tStmt.get())->name;
                                    else if (tStmt->kind == gard::StmtKind::Class)
                                        declName = static_cast<gard::ClassDeclStmt*>(tStmt.get())->name;
                                    
                                    if (declName == token->value) {
                                        int defLine2 = tStmt->location.line > 0 ? tStmt->location.line - 1 : 0;
                                        int defCol2 = tStmt->location.column > 0 ? tStmt->location.column - 1 : 0;
                                       std::string result = "{\"uri\":" + jsonString(targetUri2) + ",\"range\":{";
                                        result += "\"start\":{\"line\":" + jsonInt(defLine2) + ",\"character\":" + jsonInt(defCol2) + "},";
                                        result += "\"end\":{\"line\":" + jsonInt(defLine2) + ",\"character\":" + jsonInt(defCol2 + (int)declName.size()) + "}";
                                        sendResponse(id, result);
                                        return;
                                    }
                                }
                            } catch (...) {
                                // Parse failed — fall through
                            }
                        }
                    }
                    break;
                }
            }
        }
        
        sendResponse(id, "null");
        return;
    }

    // Determine the target URI
    std::string targetUri;
    if (!sym->location.file.empty()) {
        // If the file path is absolute, use it; otherwise use the current document's URI
        if (sym->location.file[0] == '/') {
            targetUri = "file://" + sym->location.file;
        } else {
            // Relative — use the current document URI's directory
            targetUri = uri;
        }
    } else {
        targetUri = uri;
    }

    // Convert AST location (1-indexed) to LSP (0-indexed)
    int defLine = sym->location.line > 0 ? sym->location.line - 1 : 0;
    int defCol = sym->location.column > 0 ? sym->location.column - 1 : 0;
    int defEndCol = defCol + (int)sym->name.size();

    // Build Location response
    std::string result = "{";
    result += jsonString("uri") + ":" + jsonString(targetUri) + ",";
    result += jsonString("range") + ":{";
    result += jsonString("start") + ":{" + jsonString("line") + ":" + jsonInt(defLine) + "," + jsonString("character") + ":" + jsonInt(defCol) + "},";
    result += jsonString("end") + ":{" + jsonString("line") + ":" + jsonInt(defLine) + "," + jsonString("character") + ":" + jsonInt(defEndCol) + "}";
    result += "}";
    result += "}";

    sendResponse(id, result);
}

// ============================================================================
// Tier 2: textDocument/documentSymbol
// ============================================================================

// LSP SymbolKind constants
static const int LSP_SYMBOL_CLASS = 5;
static const int LSP_SYMBOL_METHOD = 6;
static const int LSP_SYMBOL_FIELD = 8;
static const int LSP_SYMBOL_ENUM = 10;
static const int LSP_SYMBOL_INTERFACE = 11;
static const int LSP_SYMBOL_FUNCTION = 12;
static const int LSP_SYMBOL_VARIABLE = 13;

// Count lines in content up to a certain point (for estimating end ranges)
static int countLines(const std::string& content) {
    int lines = 0;
    for (char c : content) {
        if (c == '\n') lines++;
    }
    return lines;
}

// Build a DocumentSymbol JSON object
static std::string buildDocumentSymbol(const std::string& name, int kind,
                                        int startLine, int startChar,
                                        int endLine, int endChar,
                                        int selStartLine, int selStartChar,
                                        int selEndLine, int selEndChar,
                                        const std::string& children = "") {
    std::string json = "{";
    json += "\"name\":" + jsonString(name) + ",";
    json += "\"kind\":" + jsonInt(kind) + ",";
    json += "\"range\":{";
    json += "\"start\":{\"line\":" + jsonInt(startLine) + ",\"character\":" + jsonInt(startChar) + "},";
    json += "\"end\":{\"line\":" + jsonInt(endLine) + ",\"character\":" + jsonInt(endChar) + "}";
    json += "},";
    json += "\"selectionRange\":{";
    json += "\"start\":{\"line\":" + jsonInt(selStartLine) + ",\"character\":" + jsonInt(selStartChar) + "},";
    json += "\"end\":{\"line\":" + jsonInt(selEndLine) + ",\"character\":" + jsonInt(selEndChar) + "}";
    json += "}";
    if (!children.empty()) {
        json += ",\"children\":" + children;
    }
    json += "}";
    return json;
}

static void handleTextDocumentDocumentSymbol(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);

    logMessage("documentSymbol: " + uri);

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

    int totalLines = countLines(doc.content);
    std::vector<std::string> symbols;

    // Walk top-level statements
    for (const auto& stmt : doc.program.statements) {
        if (!stmt) continue;

        int stmtLine = stmt->location.line > 0 ? stmt->location.line - 1 : 0;
        int stmtCol = stmt->location.column > 0 ? stmt->location.column - 1 : 0;

        switch (stmt->kind) {
            case gard::StmtKind::FunctionDeclaration: {
                auto* fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
                int selEnd = stmtCol + (int)fn->name.size();
                // Estimate end of function: use next statement's line or total lines
                int endLine = stmtLine + (int)fn->body.size() + 1;
                if (endLine > totalLines) endLine = totalLines;
                symbols.push_back(buildDocumentSymbol(
                    fn->name, LSP_SYMBOL_FUNCTION,
                    stmtLine, stmtCol, endLine, 0,
                    stmtLine, stmtCol, stmtLine, selEnd
                ));
                break;
            }
            case gard::StmtKind::Class: {
                auto* cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                int selEnd = stmtCol + (int)cls->name.size();

                // Collect children (methods and fields)
                std::vector<std::string> children;

                for (const auto& field : cls->fields) {
                    int fLine = field.location.line > 0 ? field.location.line - 1 : stmtLine;
                    int fCol = field.location.column > 0 ? field.location.column - 1 : 0;
                    int fSelEnd = fCol + (int)field.name.size();
                    children.push_back(buildDocumentSymbol(
                        field.name, LSP_SYMBOL_FIELD,
                        fLine, fCol, fLine, fSelEnd,
                        fLine, fCol, fLine, fSelEnd
                    ));
                }

                for (const auto& method : cls->methods) {
                    int mLine = method.location.line > 0 ? method.location.line - 1 : stmtLine;
                    int mCol = method.location.column > 0 ? method.location.column - 1 : 0;
                    int mSelEnd = mCol + (int)method.name.size();
                    int mEndLine = mLine + (int)method.body.size() + 1;
                    if (mEndLine > totalLines) mEndLine = totalLines;
                    children.push_back(buildDocumentSymbol(
                        method.name, LSP_SYMBOL_METHOD,
                        mLine, mCol, mEndLine, 0,
                        mLine, mCol, mLine, mSelEnd
                    ));
                }

                std::string childrenJson;
                if (!children.empty()) {
                    childrenJson = "[";
                    for (size_t i = 0; i < children.size(); i++) {
                        if (i > 0) childrenJson += ",";
                        childrenJson += children[i];
                    }
                    childrenJson += "]";
                }

                int endLine = stmtLine + (int)cls->fields.size() + (int)cls->methods.size() + 2;
                if (endLine > totalLines) endLine = totalLines;
                symbols.push_back(buildDocumentSymbol(
                    cls->name, LSP_SYMBOL_CLASS,
                    stmtLine, stmtCol, endLine, 0,
                    stmtLine, stmtCol, stmtLine, selEnd,
                    childrenJson
                ));
                break;
            }
            case gard::StmtKind::Interface: {
                auto* iface = static_cast<gard::InterfaceDeclStmt*>(stmt.get());
                int selEnd = stmtCol + (int)iface->name.size();
                int endLine = stmtLine + (int)iface->methods.size() + 2;
                if (endLine > totalLines) endLine = totalLines;

                // Collect interface methods as children
                std::vector<std::string> children;
                for (const auto& method : iface->methods) {
                    int mLine = method.location.line > 0 ? method.location.line - 1 : stmtLine;
                    int mCol = method.location.column > 0 ? method.location.column - 1 : 0;
                    int mSelEnd = mCol + (int)method.name.size();
                    children.push_back(buildDocumentSymbol(
                        method.name, LSP_SYMBOL_METHOD,
                        mLine, mCol, mLine, mSelEnd,
                        mLine, mCol, mLine, mSelEnd
                    ));
                }

                std::string childrenJson;
                if (!children.empty()) {
                    childrenJson = "[";
                    for (size_t i = 0; i < children.size(); i++) {
                        if (i > 0) childrenJson += ",";
                        childrenJson += children[i];
                    }
                    childrenJson += "]";
                }

                symbols.push_back(buildDocumentSymbol(
                    iface->name, LSP_SYMBOL_INTERFACE,
                    stmtLine, stmtCol, endLine, 0,
                    stmtLine, stmtCol, stmtLine, selEnd,
                    childrenJson
                ));
                break;
            }
            case gard::StmtKind::Enum: {
                auto* enm = static_cast<gard::EnumDeclStmt*>(stmt.get());
                int selEnd = stmtCol + (int)enm->name.size();
                int endLine = stmtLine + (int)enm->variants.size() + 2;
                if (endLine > totalLines) endLine = totalLines;
                symbols.push_back(buildDocumentSymbol(
                    enm->name, LSP_SYMBOL_ENUM,
                    stmtLine, stmtCol, endLine, 0,
                    stmtLine, stmtCol, stmtLine, selEnd
                ));
                break;
            }
            case gard::StmtKind::VarDeclaration: {
                auto* var = static_cast<gard::VarDeclarationStmt*>(stmt.get());
                int selEnd = stmtCol + (int)var->name.size();
                symbols.push_back(buildDocumentSymbol(
                    var->name, LSP_SYMBOL_VARIABLE,
                    stmtLine, stmtCol, stmtLine, selEnd,
                    stmtLine, stmtCol, stmtLine, selEnd
                ));
                break;
            }
            case gard::StmtKind::Export: {
                // Check if the exported declaration is a symbol we care about
                auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                if (exp->declaration) {
                    auto* inner = exp->declaration.get();
                    int innerLine = inner->location.line > 0 ? inner->location.line - 1 : stmtLine;
                    int innerCol = inner->location.column > 0 ? inner->location.column - 1 : 0;

                    switch (inner->kind) {
                        case gard::StmtKind::FunctionDeclaration: {
                            auto* fn = static_cast<gard::FunctionDeclStmt*>(inner);
                            int selEnd = innerCol + (int)fn->name.size();
                            int endLine = innerLine + (int)fn->body.size() + 1;
                            if (endLine > totalLines) endLine = totalLines;
                            symbols.push_back(buildDocumentSymbol(
                                fn->name, LSP_SYMBOL_FUNCTION,
                                innerLine, innerCol, endLine, 0,
                                innerLine, innerCol, innerLine, selEnd
                            ));
                            break;
                        }
                        case gard::StmtKind::Class: {
                            auto* cls = static_cast<gard::ClassDeclStmt*>(inner);
                            int selEnd = innerCol + (int)cls->name.size();
                            int endLine = innerLine + (int)cls->fields.size() + (int)cls->methods.size() + 2;
                            if (endLine > totalLines) endLine = totalLines;
                            symbols.push_back(buildDocumentSymbol(
                                cls->name, LSP_SYMBOL_CLASS,
                                innerLine, innerCol, endLine, 0,
                                innerLine, innerCol, innerLine, selEnd
                            ));
                            break;
                        }
                        case gard::StmtKind::VarDeclaration: {
                            auto* var = static_cast<gard::VarDeclarationStmt*>(inner);
                            int selEnd = innerCol + (int)var->name.size();
                            symbols.push_back(buildDocumentSymbol(
                                var->name, LSP_SYMBOL_VARIABLE,
                                innerLine, innerCol, innerLine, selEnd,
                                innerLine, innerCol, innerLine, selEnd
                            ));
                            break;
                        }
                        default:
                            break;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // Build response array
    std::string result = "[";
    for (size_t i = 0; i < symbols.size(); i++) {
        if (i > 0) result += ",";
        result += symbols[i];
    }
    result += "]";

    sendResponse(id, result);
}

// ============================================================================
// Tier 2: textDocument/formatting
// ============================================================================

static void handleTextDocumentFormatting(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);

    logMessage("formatting: " + uri);

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "[]");
        return;
    }

    DocumentState& doc = it->second;

    // We need a valid AST to format
    if (!doc.analysisValid || doc.program.statements.empty()) {
        // Try to parse just for formatting (even with sema errors, AST may be usable)
        std::string filename = uri;
        if (filename.rfind("file://", 0) == 0) {
            filename = filename.substr(7);
        }

        try {
            gard::Lexer lexer(doc.content, filename);
            std::vector<gard::Token> tokens = lexer.tokenize();
            if (lexer.hasErrors()) {
                sendResponse(id, "[]");
                return;
            }

            gard::Parser parser(tokens, filename);
            gard::Program program = parser.parse();
            if (parser.hasErrors()) {
                sendResponse(id, "[]");
                return;
            }

            // Format using the parsed program
            gard::tools::FormatConfig config;
            gard::tools::Formatter formatter(config);
            std::string formatted = formatter.format(program);

            if (formatted.empty() || formatted == doc.content) {
                sendResponse(id, "[]");
                return;
            }

            // Return a single TextEdit replacing the entire document
            int totalLines = countLines(doc.content);
            std::string result = "[{";
            result += "\"range\":{";
            result += "\"start\":{\"line\":0,\"character\":0},";
            result += "\"end\":{\"line\":" + jsonInt(totalLines + 1) + ",\"character\":0}";
            result += "},";
            result += "\"newText\":" + jsonString(formatted);
            result += "}]";

            sendResponse(id, result);
            return;
        } catch (const std::exception& e) {
            logMessage("Formatting failed: " + std::string(e.what()));
            sendResponse(id, "[]");
            return;
        } catch (...) {
            sendResponse(id, "[]");
            return;
        }
    }

    // Use the stored program for formatting
    try {
        gard::tools::FormatConfig config;
        gard::tools::Formatter formatter(config);
        std::string formatted = formatter.format(doc.program);

        if (formatted.empty() || formatted == doc.content) {
            sendResponse(id, "[]");
            return;
        }

        // Return a single TextEdit replacing the entire document
        int totalLines = countLines(doc.content);
        std::string result = "[{";
        result += "\"range\":{";
        result += "\"start\":{\"line\":0,\"character\":0},";
        result += "\"end\":{\"line\":" + jsonInt(totalLines + 1) + ",\"character\":0}";
        result += "},";
        result += "\"newText\":" + jsonString(formatted);
        result += "}]";

        sendResponse(id, result);
    } catch (const std::exception& e) {
        logMessage("Formatting failed: " + std::string(e.what()));
        sendResponse(id, "[]");
    } catch (...) {
        sendResponse(id, "[]");
    }
}

// ============================================================================
// Tier 3: Helper — get line text from document content
// ============================================================================

static std::string getLineText(const std::string& content, int line) {
    int currentLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < content.size(); i++) {
        if (currentLine == line) { lineStart = i; break; }
        if (content[i] == '\n') currentLine++;
    }
    if (currentLine < line) return ""; // line beyond content
    size_t lineEnd = content.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = content.size();
    return content.substr(lineStart, lineEnd - lineStart);
}

// Helper: convert a TypePtr to a readable string
static std::string typeToString(const gard::TypeAnnotation* type) {
    if (!type) return "any";
    switch (type->kind) {
        case gard::TypeKind::Named:
            return static_cast<const gard::NamedType*>(type)->name;
        case gard::TypeKind::Generic: {
            auto* gt = static_cast<const gard::GenericType*>(type);
            std::string s = gt->name + "<";
            for (size_t i = 0; i < gt->typeArgs.size(); i++) {
                if (i > 0) s += ", ";
                s += typeToString(gt->typeArgs[i].get());
            }
            s += ">";
            return s;
        }
        case gard::TypeKind::Nullable: {
            auto* nt = static_cast<const gard::NullableType*>(type);
            return typeToString(nt->inner.get()) + "?";
        }
        case gard::TypeKind::Tuple: {
            auto* tt = static_cast<const gard::TupleType*>(type);
            std::string s = "(";
            for (size_t i = 0; i < tt->elements.size(); i++) {
                if (i > 0) s += ", ";
                s += typeToString(tt->elements[i].get());
            }
            s += ")";
            return s;
        }
        case gard::TypeKind::Function: {
            auto* ft = static_cast<const gard::FunctionType*>(type);
            std::string s = "(";
            for (size_t i = 0; i < ft->paramTypes.size(); i++) {
                if (i > 0) s += ", ";
                s += typeToString(ft->paramTypes[i].get());
            }
            s += ") => ";
            s += typeToString(ft->returnType.get());
            return s;
        }
        default:
            return "any";
    }
}

// ============================================================================
// Tier 3: textDocument/completion
// ============================================================================

// CompletionItemKind constants
static const int COMPLETION_TEXT = 1;
static const int COMPLETION_METHOD = 2;
static const int COMPLETION_FUNCTION = 3;
static const int COMPLETION_CONSTRUCTOR = 4;
static const int COMPLETION_FIELD = 5;
static const int COMPLETION_VARIABLE = 6;
static const int COMPLETION_CLASS = 7;
static const int COMPLETION_INTERFACE = 8;
static const int COMPLETION_MODULE = 9;
static const int COMPLETION_PROPERTY = 10;
static const int COMPLETION_ENUM = 13;
static const int COMPLETION_KEYWORD = 14;
static const int COMPLETION_SNIPPET = 15;

static std::string buildCompletionItem(const std::string& label, int kind,
                                        const std::string& detail,
                                        const std::string& insertText,
                                        int insertTextFormat = 1,
                                        int sortPriority = 50) {
    // sortPriority: 0-9 = highest (locals), 10-19 = symbols, 20-29 = methods,
    //               30-39 = keywords, 40-49 = types, 50+ = default
    char sortBuf[8];
    snprintf(sortBuf, sizeof(sortBuf), "%02d", sortPriority);
    std::string sortText = std::string(sortBuf) + label;
    std::string json = "{";
    json += "\"label\":" + jsonString(label) + ",";
    json += "\"kind\":" + jsonInt(kind) + ",";
    json += "\"detail\":" + jsonString(detail) + ",";
    json += "\"sortText\":" + jsonString(sortText) + ",";
    json += "\"insertText\":" + jsonString(insertText);
    if (insertTextFormat == 2) {
        json += ",\"insertTextFormat\":2";
    }
    json += "}";
    return json;
}

// ============================================================================
// Tier 2: Chained Member Access Resolution
// ============================================================================

// Resolve a chained member access like "user.address.city" to find the final type.
// Returns the class name of the final resolved type, or empty string if unresolvable.
static std::string resolveChainType(const std::string& textBeforeDot, const DocumentState& doc) {
    // Parse the chain: split by '.' to get segments
    // e.g., "user.address" → ["user", "address"]
    std::vector<std::string> segments;
    std::string current;
    for (char c : textBeforeDot) {
        if (c == '.') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else if (std::isalnum(c) || c == '_') {
            current += c;
        } else {
            // Non-identifier char — reset
            segments.clear();
            current.clear();
        }
    }
    if (!current.empty()) segments.push_back(current);

    if (segments.empty()) return "";

    // Resolve the first segment to get its type
    std::string currentType;

    // Try symbol table first
    if (doc.sema && doc.analysisValid) {
        gard::Symbol* sym = doc.sema->getSymbolTable().resolve(segments[0]);
        if (sym) {
            if (sym->kind == gard::SymbolKind::Class) {
                currentType = sym->name; // Static access on class itself
            } else {
                currentType = sym->typeName; // Variable — use its type
            }
        }
    }

    // Try local variables if symbol table failed
    if (currentType.empty()) {
        // Search all functions for the first segment
        for (const auto& stmt : doc.program.statements) {
            if (!stmt) continue;
            gard::FunctionDeclStmt* fn = nullptr;
            if (stmt->kind == gard::StmtKind::FunctionDeclaration)
                fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
            else if (stmt->kind == gard::StmtKind::Export) {
                auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                if (exp->declaration && exp->declaration->kind == gard::StmtKind::FunctionDeclaration)
                    fn = static_cast<gard::FunctionDeclStmt*>(exp->declaration.get());
            }
            if (fn) {
                // Check params
                for (const auto& p : fn->params) {
                    if (p.name == segments[0] && p.type) {
                        currentType = typeToString(p.type.get());
                        break;
                    }
                }
                if (!currentType.empty()) break;
                // Check body vars
                for (const auto& s : fn->body) {
                    if (s && s->kind == gard::StmtKind::VarDeclaration) {
                        auto* var = static_cast<gard::VarDeclarationStmt*>(s.get());
                        if (var->name == segments[0]) {
                            if (var->type) currentType = typeToString(var->type.get());
                            else if (var->initializer) {
                                if (auto* ne = dynamic_cast<gard::NewExpr*>(var->initializer.get())) { currentType = ne->className; if (!ne->typeArgs.empty()) { currentType += "<"; for (size_t ti = 0; ti < ne->typeArgs.size(); ti++) { if (ti > 0) currentType += ", "; currentType += typeToString(ne->typeArgs[ti].get()); } currentType += ">"; } }


                            }
                            break;
                        }
                    }
                }
                if (!currentType.empty()) break;
            }
            // Check class methods too
            if (stmt->kind == gard::StmtKind::Class) {
                auto* cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                for (const auto& method : cls->methods) {
                    for (const auto& p : method.params) {
                        if (p.name == segments[0] && p.type) {
                            currentType = typeToString(p.type.get());
                            break;
                        }
                    }
                    if (!currentType.empty()) break;
                    for (const auto& s : method.body) {
                        if (s && s->kind == gard::StmtKind::VarDeclaration) {
                            auto* var = static_cast<gard::VarDeclarationStmt*>(s.get());
                            if (var->name == segments[0]) {
                                if (var->type) currentType = typeToString(var->type.get());
                                else if (var->initializer) {
                                    if (auto* ne = dynamic_cast<gard::NewExpr*>(var->initializer.get())) { currentType = ne->className; if (!ne->typeArgs.empty()) { currentType += "<"; for (size_t ti = 0; ti < ne->typeArgs.size(); ti++) { if (ti > 0) currentType += ", "; currentType += typeToString(ne->typeArgs[ti].get()); } currentType += ">"; } }
                                }
                                break;
                            }
                        }
                    }
                    if (!currentType.empty()) break;
                }
            }
            if (!currentType.empty()) break;
        }
    }

    if (currentType.empty()) return "";

    // Now resolve remaining segments through class field types
    for (size_t i = 1; i < segments.size(); i++) {
        std::string fieldName = segments[i];
        std::string nextType;

        // Find the class with name currentType and look up the field
        for (const auto& stmt : doc.program.statements) {
            if (!stmt) continue;
            gard::ClassDeclStmt* cls = nullptr;
            if (stmt->kind == gard::StmtKind::Class)
                cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
            else if (stmt->kind == gard::StmtKind::Export) {
                auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                if (exp->declaration && exp->declaration->kind == gard::StmtKind::Class)
                    cls = static_cast<gard::ClassDeclStmt*>(exp->declaration.get());
            }
            if (cls && cls->name == currentType) {
                for (const auto& field : cls->fields) {
                    if (field.name == fieldName && field.type) {
                        nextType = typeToString(field.type.get());
                        break;
                    }
                }
                // Also check methods (for method return types)
                if (nextType.empty()) {
                    for (const auto& method : cls->methods) {
                        if (method.name == fieldName && method.returnType) {
                            nextType = typeToString(method.returnType.get());
                            break;
                        }
                    }
                }
                break;
            }
        }

        if (nextType.empty()) return currentType; // Can't resolve further
        currentType = nextType;
    }

    return currentType;
}

static void handleTextDocumentCompletion(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);

    logMessage("completion: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character));

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "[]");
        return;
    }

    DocumentState& doc = it->second;
    std::string lineText = getLineText(doc.content, line);
    std::string textBeforeCursor = (character <= (int)lineText.size())
        ? lineText.substr(0, character) : lineText;

    std::vector<std::string> items;

    // Determine completion context
    bool isMemberCompletion = false;
    std::string prefix;
    std::string objectName;

    if (!textBeforeCursor.empty() && textBeforeCursor.back() == '.') {
        // Member completion: find the identifier before the dot
        isMemberCompletion = true;
        int dotPos = (int)textBeforeCursor.size() - 1;
        int end = dotPos;
        int start = end - 1;
        while (start >= 0 && (std::isalnum(textBeforeCursor[start]) || textBeforeCursor[start] == '_')) {
            start--;
        }
        start++;
        if (start < end) {
            objectName = textBeforeCursor.substr(start, end - start);
        }
    } else {
        // General completion: get the partial word being typed
        int end = (int)textBeforeCursor.size();
        int start = end - 1;
        while (start >= 0 && (std::isalnum(textBeforeCursor[start]) || textBeforeCursor[start] == '_')) {
            start--;
        }
        start++;
        if (start < end) {
            prefix = textBeforeCursor.substr(start, end - start);
        }
    }

    if (isMemberCompletion) {
        std::string typeName;

        // Try to resolve the full chain (e.g., "user.address." → Address type)
        // Get the full text before the dot
        std::string chainText = textBeforeCursor.substr(0, textBeforeCursor.size() - 1); // remove trailing '.'
        typeName = resolveChainType(chainText, doc);

        // Fallback: simple single-identifier lookup
        if (typeName.empty() && !objectName.empty() && doc.sema && doc.analysisValid) {
            gard::Symbol* objSym = doc.sema->getSymbolTable().resolve(objectName);
            if (objSym) {
                if (objSym->kind == gard::SymbolKind::Class) {
                    typeName = objSym->name;
                } else {
                    typeName = objSym->typeName;
                }
            }
        }

        if (!typeName.empty()) {
            // Find the class in the AST to get its fields and methods
            for (const auto& stmt : doc.program.statements) {
                if (!stmt) continue;
                gard::ClassDeclStmt* cls = nullptr;
                if (stmt->kind == gard::StmtKind::Class) {
                    cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                } else if (stmt->kind == gard::StmtKind::Export) {
                    auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                    if (exp->declaration && exp->declaration->kind == gard::StmtKind::Class) {
                        cls = static_cast<gard::ClassDeclStmt*>(exp->declaration.get());
                    }
                }
                if (cls && cls->name == typeName) {
                    // Add fields
                    for (const auto& field : cls->fields) {
                        std::string fieldType = typeToString(field.type.get());
                        items.push_back(buildCompletionItem(
                            field.name, COMPLETION_FIELD, fieldType, field.name));
                    }
                    // Add methods
                    for (const auto& method : cls->methods) {
                        std::string detail = "(";
                        std::string insertSnippet = method.name + "(";
                        for (size_t i = 0; i < method.params.size(); i++) {
                            if (i > 0) { detail += ", "; insertSnippet += ", "; }
                            detail += method.params[i].name + ": " + typeToString(method.params[i].type.get());
                            insertSnippet += "$" + std::to_string(i + 1);
                        }
                        detail += ")";
                        insertSnippet += ")";
                        if (method.returnType) {
                            detail += ": " + typeToString(method.returnType.get());
                        }
                        items.push_back(buildCompletionItem(
                            method.name, COMPLETION_METHOD, detail,
                            method.params.empty() ? method.name + "()" : insertSnippet,
                            method.params.empty() ? 1 : 2));
                    }
                    break;
                }
            }
        }

        // Also try resolveInClass from the symbol table
        if (items.empty() && !typeName.empty() && doc.sema && doc.analysisValid) {
            // Fallback: look up class symbol and use its info
            gard::Symbol* classSym = doc.sema->getSymbolTable().resolve(typeName);
            if (classSym && classSym->kind == gard::SymbolKind::Class) {
                // We already tried AST above; if empty, just provide the class name
            }
        }

        // Tier 3: Stdlib namespace completion (e.g., Math., String., File.)
        if (items.empty() && !objectName.empty()) {
            for (int i = 0; STDLIB_METHODS[i].ns != nullptr; i++) {
                if (objectName == STDLIB_METHODS[i].ns) {
                    std::string detail = "(";
                    for (int p = 0; STDLIB_METHODS[i].params[p].name != nullptr; p++) {
                        if (p > 0) detail += ", ";
                        detail += std::string(STDLIB_METHODS[i].params[p].name) + ": " + STDLIB_METHODS[i].params[p].type;
                    }
                    detail += "): " + std::string(STDLIB_METHODS[i].returnType);
                    items.push_back(buildCompletionItem(
                        STDLIB_METHODS[i].name, COMPLETION_METHOD, detail,
                        std::string(STDLIB_METHODS[i].name) + "("));
                }
            }
        }
    } else {
        // General completion: symbols + keywords + types

        // 1. Symbols from the symbol table (iterate all scopes)
        if (doc.sema && doc.analysisValid) {
            const auto& scopes = doc.sema->getSymbolTable().getScopes();
            std::unordered_set<std::string> seen;
            for (const auto& scope : scopes) {
                for (const auto& pair : scope->symbols) {
                    const gard::Symbol& sym = pair.second;
                    if (!seen.insert(sym.name).second) continue; // skip duplicates
                    if (!prefix.empty()) {
                        // Prefix filter (case-insensitive start)
                        if (sym.name.size() < prefix.size()) continue;
                        bool match = true;
                        for (size_t i = 0; i < prefix.size(); i++) {
                            if (std::tolower(sym.name[i]) != std::tolower(prefix[i])) {
                                match = false;
                                break;
                            }
                        }
                        if (!match) continue;
                    }

                    int kind = COMPLETION_VARIABLE;
                    std::string detail = sym.typeName;
                    std::string insertText = sym.name;

                    switch (sym.kind) {
                        case gard::SymbolKind::Function: {
                            kind = COMPLETION_FUNCTION;
                            detail = "(";
                            for (size_t i = 0; i < sym.paramTypes.size(); i++) {
                                if (i > 0) detail += ", ";
                                detail += sym.paramTypes[i];
                            }
                            detail += ")";
                            if (!sym.returnType.empty()) detail += ": " + sym.returnType;
                            insertText = sym.name + (sym.paramTypes.empty() ? "()" : "(");
                            break;
                        }
                        case gard::SymbolKind::Class:
                            kind = COMPLETION_CLASS;
                            detail = "class";
                            break;
                        case gard::SymbolKind::Interface:
                            kind = COMPLETION_INTERFACE;
                            detail = "interface";
                            break;
                        case gard::SymbolKind::Method:
                            kind = COMPLETION_METHOD;
                            detail = "(";
                            for (size_t i = 0; i < sym.paramTypes.size(); i++) {
                                if (i > 0) detail += ", ";
                                detail += sym.paramTypes[i];
                            }
                            detail += ")";
                            if (!sym.returnType.empty()) detail += ": " + sym.returnType;
                            break;
                        case gard::SymbolKind::Field:
                            kind = COMPLETION_FIELD;
                            break;
                        case gard::SymbolKind::Parameter:
                            kind = COMPLETION_VARIABLE;
                            break;
                        case gard::SymbolKind::Module:
                            kind = COMPLETION_MODULE;
                            detail = "module";
                            break;
                        case gard::SymbolKind::Constructor:
                            kind = COMPLETION_CONSTRUCTOR;
                            detail = "constructor";
                            break;
                        default:
                            break;
                    }

                    int sortPri = 15; // default
                    if (kind == COMPLETION_VARIABLE) sortPri = 10;
                    else if (kind == COMPLETION_FUNCTION) sortPri = 12;
                    else if (kind == COMPLETION_METHOD) sortPri = 12;
                    else if (kind == COMPLETION_CLASS) sortPri = 20;
                    else if (kind == COMPLETION_INTERFACE) sortPri = 20;
                    else if (kind == COMPLETION_FIELD) sortPri = 8;

                    items.push_back(buildCompletionItem(sym.name, kind, detail, insertText, 1, sortPri));
                }
            }
        }

        // 2. Tier 1: Local variables from the current function scope
        {
            int astLine = line + 1;
            auto locals = collectLocalsAtLine(doc.program, astLine);
            std::unordered_set<std::string> localSeen;
            for (const auto& local : locals) {
                if (!localSeen.insert(local.name).second) continue;
                if (!prefix.empty()) {
                    if (local.name.size() < prefix.size()) continue;
                    bool match = true;
                    for (size_t i = 0; i < prefix.size(); i++) {
                        if (std::tolower(local.name[i]) != std::tolower(prefix[i])) {
                            match = false;
                            break;
                        }
                    }
                    if (!match) continue;
                }
                items.push_back(buildCompletionItem(
                    local.name, COMPLETION_VARIABLE, local.typeName, local.name, 1, 5));
            }
        }

        // 3. Context-aware snippets (Tier 10)
        // Determine context: top-level, inside class, inside function
        enum CompletionContext { CTX_TOP_LEVEL, CTX_CLASS_BODY, CTX_FUNCTION_BODY, CTX_TYPE_POSITION, CTX_ANNOTATION };
        CompletionContext ctx = CTX_TOP_LEVEL;

        // Check if we're after '@' — annotation context
        if (!textBeforeCursor.empty() && textBeforeCursor.back() == '@') {
            ctx = CTX_ANNOTATION;
        }
        // Check if we're in a type position (after ':')
        else if (textBeforeCursor.size() >= 2) {
            // Scan backwards for ':'
            int scanPos = (int)textBeforeCursor.size() - 1;
            while (scanPos >= 0 && textBeforeCursor[scanPos] == ' ') scanPos--;
            if (scanPos >= 0 && textBeforeCursor[scanPos] == ':') {
                ctx = CTX_TYPE_POSITION;
            }
        }

        if (ctx == CTX_TOP_LEVEL || ctx == CTX_FUNCTION_BODY || ctx == CTX_CLASS_BODY) {
            // Determine if we're inside a function or class by checking AST
            int astLine = line + 1;
            for (const auto& stmt : doc.program.statements) {
                if (!stmt) continue;
                if (stmt->kind == gard::StmtKind::FunctionDeclaration) {
                    auto* fn = static_cast<gard::FunctionDeclStmt*>(stmt.get());
                    int fnStart = fn->location.line;
                    int fnEnd = fnStart + (int)fn->body.size() + 3;
                    if (astLine > fnStart && astLine <= fnEnd) { ctx = CTX_FUNCTION_BODY; break; }
                }
                if (stmt->kind == gard::StmtKind::Class) {
                    auto* cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                    int clsStart = cls->location.line;
                    int clsEnd = clsStart + (int)cls->fields.size() + (int)cls->methods.size() + 5;
                    if (astLine > clsStart && astLine <= clsEnd) {
                        // Check if inside a method
                        bool inMethod = false;
                        for (const auto& method : cls->methods) {
                            int mStart = method.location.line;
                            int mEnd = mStart + (int)method.body.size() + 3;
                            if (astLine > mStart && astLine <= mEnd) { inMethod = true; break; }
                        }
                        ctx = inMethod ? CTX_FUNCTION_BODY : CTX_CLASS_BODY;
                        break;
                    }
                }
            }
        }

        struct KeywordSnippet {
            const char* keyword;
            const char* snippet;
            const char* detail;
        };

        if (ctx == CTX_ANNOTATION) {
            // After '@' — offer annotation names
            static const char* annotations[] = {
                "Test", "TestClass", "BeforeEach", "AfterEach",
                "Table", "Column", "Primary", "Timestamp", "SoftDelete",
                "Entity", "HasMany", "HasOne", "BelongsTo",
                "Get", "Post", "Route", "Controller",
                "Injectable", "Inject", "Cacheable", "Paginated", "ValidateBody",
                "Authenticated", "Serializable", "Debug", "Profile",
                "WasmModule", "Name"
            };
            for (const char* ann : annotations) {
                if (prefix.empty() || std::string(ann).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(ann, COMPLETION_KEYWORD, "annotation", ann));
                }
            }
        } else if (ctx == CTX_TYPE_POSITION) {
            // After ':' — offer type names
            static const char* typeNames[] = {
                "int", "string", "void", "bool", "float", "double", "long",
                "array", "map", "set"
            };
            for (const char* t : typeNames) {
                if (prefix.empty() || std::string(t).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(t, COMPLETION_CLASS, "type", t));
                }
            }
            // Also offer class names from the document
            for (const auto& stmt : doc.program.statements) {
                if (!stmt) continue;
                std::string className;
                if (stmt->kind == gard::StmtKind::Class)
                    className = static_cast<gard::ClassDeclStmt*>(stmt.get())->name;
                else if (stmt->kind == gard::StmtKind::Interface)
                    className = static_cast<gard::InterfaceDeclStmt*>(stmt.get())->name;
                if (!className.empty() && (prefix.empty() || className.find(prefix) == 0)) {
                    items.push_back(buildCompletionItem(className, COMPLETION_CLASS, "class", className));
                }
            }
        } else if (ctx == CTX_CLASS_BODY) {
            // Inside a class body — offer class member declarations
            static const KeywordSnippet classSnippets[] = {
                {"constructor", "constructor($1) {\n\t$0\n}", "constructor"},
                {"function", "function $1($2): $3 {\n\t$0\n}", "method declaration"},
                {"let", "let $1: $2;", "field declaration"},
                {"get", "get $1(): $2 {\n\t$0\n}", "getter"},
                {"set", "set $1($2): void {\n\t$0\n}", "setter"},
                {"static", "static function $1($2): $3 {\n\t$0\n}", "static method"},
                {"async", "async function $1($2): $3 {\n\t$0\n}", "async method"},
            };
            for (const auto& ks : classSnippets) {
                if (prefix.empty() || std::string(ks.keyword).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(ks.keyword, COMPLETION_SNIPPET, ks.detail, ks.snippet, 2));
                }
            }
        } else if (ctx == CTX_FUNCTION_BODY) {
            // Inside a function body — offer control flow + local declarations
            static const KeywordSnippet funcSnippets[] = {
                {"if",      "if ($1) {\n\t$0\n}",                          "if statement"},
                {"for",     "for (let $1 = 0; $1 < $2; $1++) {\n\t$0\n}", "for loop"},
                {"foreach", "for (let $1 of $2) {\n\t$0\n}",              "for-of loop"},
                {"while",   "while ($1) {\n\t$0\n}",                       "while loop"},
                {"try",     "try {\n\t$1\n} catch (e) {\n\t$0\n}",        "try-catch block"},
                {"match",   "match ($1) {\n\t$0\n}",                       "match expression"},
                {"let",     "let $1 = $0;",                                "variable declaration"},
                {"const",   "const $1 = $0;",                              "constant declaration"},
            };
            for (const auto& ks : funcSnippets) {
                if (prefix.empty() || std::string(ks.keyword).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(ks.keyword, COMPLETION_SNIPPET, ks.detail, ks.snippet, 2));
                }
            }
            // Simple keywords for function body
            static const char* funcKeywords[] = { "return", "break", "continue", "throw", "await", "new" };
            for (const char* kw : funcKeywords) {
                if (prefix.empty() || std::string(kw).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(kw, COMPLETION_KEYWORD, "keyword", kw));
                }
            }
        } else {
            // Top-level — offer declarations
            static const KeywordSnippet topSnippets[] = {
                {"function",  "function $1($2): $3 {\\n\\t$0\\n}",             "function declaration"},
                {"class",     "class $1 {\\n\\t$0\\n}",                         "class declaration"},
                {"interface", "interface $1 {\\n\\t$0\\n}",                     "interface declaration"},
                {"enum",      "enum $1 {\\n\\t$0\\n}",                          "enum declaration"},
                {"import",    "import { $1 } from \\\"$2\\\";",                  "import statement"},
                {"export",    "export $0",                                    "export declaration"},
                {"async",     "async function $1($2): $3 {\\n\\t$0\\n}",       "async function"},
            };
            for (const auto& ks : topSnippets) {
                if (prefix.empty() || std::string(ks.keyword).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(ks.keyword, COMPLETION_SNIPPET, ks.detail, ks.snippet, 2));
                }
            }
        }

        // 4. Common keywords always available
        static const char* commonKeywords[] = { "true", "false", "null", "this", "super" };
        for (const char* kw : commonKeywords) {
            if (prefix.empty() || std::string(kw).find(prefix) == 0) {
                items.push_back(buildCompletionItem(kw, COMPLETION_KEYWORD, "keyword", kw));
            }
        }

        // 5. Built-in types (always available for type annotations)
        if (ctx != CTX_TYPE_POSITION && ctx != CTX_ANNOTATION) {
            static const char* builtinTypes[] = {
                "int", "string", "void", "bool", "float", "double", "long", "array", "map"
            };
            for (const char* t : builtinTypes) {
                if (prefix.empty() || std::string(t).find(prefix) == 0) {
                    items.push_back(buildCompletionItem(t, COMPLETION_CLASS, "built-in type", t));
                }
            }
        }
    }

    // Build response array
    std::string result = "[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) result += ",";
        result += items[i];
    }
    result += "]";

    sendResponse(id, result);
}

// ============================================================================
// Tier 3: textDocument/signatureHelp
// ============================================================================

static void handleTextDocumentSignatureHelp(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);

    logMessage("signatureHelp: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character));

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "null");
        return;
    }

    DocumentState& doc = it->second;
    std::string lineText = getLineText(doc.content, line);
    std::string textBeforeCursor = (character <= (int)lineText.size())
        ? lineText.substr(0, character) : lineText;

    // Find the matching open paren and the function name before it
    // Scan backwards from cursor, counting parens to find the matching '('
    int parenDepth = 0;
    int activeParameter = 0;
    int openParenPos = -1;

    for (int i = (int)textBeforeCursor.size() - 1; i >= 0; i--) {
        char c = textBeforeCursor[i];
        if (c == ')') {
            parenDepth++;
        } else if (c == '(') {
            if (parenDepth == 0) {
                openParenPos = i;
                break;
            }
            parenDepth--;
        } else if (c == ',' && parenDepth == 0) {
            activeParameter++;
        }
    }

    if (openParenPos < 0) {
        sendResponse(id, "null");
        return;
    }

    // Get the function name before the '('
    int nameEnd = openParenPos;
    int nameStart = nameEnd - 1;
    while (nameStart >= 0 && (std::isalnum(textBeforeCursor[nameStart]) || textBeforeCursor[nameStart] == '_')) {
        nameStart--;
    }
    nameStart++;

    if (nameStart >= nameEnd) {
        sendResponse(id, "null");
        return;
    }

    std::string funcName = textBeforeCursor.substr(nameStart, nameEnd - nameStart);
    logMessage("signatureHelp: looking up function '" + funcName + "', activeParam=" + std::to_string(activeParameter));

    if (!doc.sema || !doc.analysisValid) {
        sendResponse(id, "null");
        return;
    }

    // Try to find the function in the AST first (for full param names)
    std::string signatureLabel;
    std::vector<std::string> paramLabels;

    // Search AST for function declaration
    for (const auto& stmt : doc.program.statements) {
        if (!stmt) continue;
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
            // Build signature from AST
            signatureLabel = "function " + fn->name + "(";
            for (size_t i = 0; i < fn->params.size(); i++) {
                if (i > 0) signatureLabel += ", ";
                std::string paramStr = fn->params[i].name + ": " + typeToString(fn->params[i].type.get());
                paramLabels.push_back(paramStr);
                signatureLabel += paramStr;
            }
            signatureLabel += ")";
            if (fn->returnType) {
                signatureLabel += ": " + typeToString(fn->returnType.get());
            }
            break;
        }
    }

    // If not found in top-level AST, search class methods
    if (signatureLabel.empty()) {
        for (const auto& stmt : doc.program.statements) {
            if (!stmt) continue;
            gard::ClassDeclStmt* cls = nullptr;
            if (stmt->kind == gard::StmtKind::Class) {
                cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
            } else if (stmt->kind == gard::StmtKind::Export) {
                auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                if (exp->declaration && exp->declaration->kind == gard::StmtKind::Class) {
                    cls = static_cast<gard::ClassDeclStmt*>(exp->declaration.get());
                }
            }
            if (!cls) continue;
            for (const auto& method : cls->methods) {
                if (method.name == funcName) {
                    signatureLabel = method.name + "(";
                    for (size_t i = 0; i < method.params.size(); i++) {
                        if (i > 0) signatureLabel += ", ";
                        std::string paramStr = method.params[i].name + ": " + typeToString(method.params[i].type.get());
                        paramLabels.push_back(paramStr);
                        signatureLabel += paramStr;
                    }
                    signatureLabel += ")";
                    if (method.returnType) {
                        signatureLabel += ": " + typeToString(method.returnType.get());
                    }
                    break;
                }
            }
            if (!signatureLabel.empty()) break;
        }
    }

    // Fallback: use symbol table (less info — no param names)
    if (signatureLabel.empty()) {
        gard::Symbol* sym = doc.sema->getSymbolTable().resolve(funcName);
        if (sym && (sym->kind == gard::SymbolKind::Function || sym->kind == gard::SymbolKind::Method)) {
            signatureLabel = "function " + sym->name + "(";
            for (size_t i = 0; i < sym->paramTypes.size(); i++) {
                if (i > 0) signatureLabel += ", ";
                std::string paramStr = "arg" + std::to_string(i) + ": " + sym->paramTypes[i];
                paramLabels.push_back(paramStr);
                signatureLabel += paramStr;
            }
            signatureLabel += ")";
            if (!sym->returnType.empty()) {
                signatureLabel += ": " + sym->returnType;
            }
        }
    }

    // Tier 4: Check for "new ClassName(" pattern — constructor signatures
    if (signatureLabel.empty()) {
        int checkPos = nameStart - 1;
        while (checkPos >= 0 && textBeforeCursor[checkPos] == ' ') checkPos--;
        // Check if the 3 chars before are "new"
        if (checkPos >= 2 && textBeforeCursor.substr(checkPos - 2, 3) == "new") {
            // funcName is the class name — find its constructor in the AST
            for (const auto& stmt : doc.program.statements) {
                if (!stmt) continue;
                gard::ClassDeclStmt* cls = nullptr;
                if (stmt->kind == gard::StmtKind::Class) {
                    cls = static_cast<gard::ClassDeclStmt*>(stmt.get());
                } else if (stmt->kind == gard::StmtKind::Export) {
                    auto* exp = static_cast<gard::ExportStmt*>(stmt.get());
                    if (exp->declaration && exp->declaration->kind == gard::StmtKind::Class)
                        cls = static_cast<gard::ClassDeclStmt*>(exp->declaration.get());
                }
                if (cls && cls->name == funcName && cls->constructor) {
                    signatureLabel = "new " + cls->name + "(";
                    for (size_t i = 0; i < cls->constructor->params.size(); i++) {
                        if (i > 0) signatureLabel += ", ";
                        std::string paramStr = cls->constructor->params[i].name + ": " +
                            typeToString(cls->constructor->params[i].type.get());
                        paramLabels.push_back(paramStr);
                        signatureLabel += paramStr;
                    }
                    signatureLabel += ")";
                    break;
                }
            }
        }
    }

    // Tier 3: Check if it's a stdlib call: Namespace.method(
    if (signatureLabel.empty() && nameStart > 0) {
        int dotPos = nameStart - 1;
        while (dotPos >= 0 && textBeforeCursor[dotPos] == ' ') dotPos--;
        if (dotPos >= 0 && textBeforeCursor[dotPos] == '.') {
            // Get namespace before dot
            int nsEnd = dotPos;
            int nsStart = nsEnd - 1;
            while (nsStart >= 0 && (std::isalnum(textBeforeCursor[nsStart]) || textBeforeCursor[nsStart] == '_')) nsStart--;
            nsStart++;
            std::string nsName = textBeforeCursor.substr(nsStart, nsEnd - nsStart);

            // Look up in stdlib
            for (int i = 0; STDLIB_METHODS[i].ns != nullptr; i++) {
                if (nsName == STDLIB_METHODS[i].ns && funcName == STDLIB_METHODS[i].name) {
                    signatureLabel = std::string(STDLIB_METHODS[i].ns) + "." + STDLIB_METHODS[i].name + "(";
                    for (int p = 0; STDLIB_METHODS[i].params[p].name != nullptr; p++) {
                        if (p > 0) signatureLabel += ", ";
                        std::string paramStr = std::string(STDLIB_METHODS[i].params[p].name) + ": " + STDLIB_METHODS[i].params[p].type;
                        paramLabels.push_back(paramStr);
                        signatureLabel += paramStr;
                    }
                    signatureLabel += "): " + std::string(STDLIB_METHODS[i].returnType);
                    break;
                }
            }
        }
    }

    if (signatureLabel.empty()) {
        sendResponse(id, "null");
        return;
    }

    // Clamp activeParameter
    if (activeParameter >= (int)paramLabels.size()) {
        activeParameter = paramLabels.empty() ? 0 : (int)paramLabels.size() - 1;
    }
    // Build response
    std::string paramArray = "[";
    for (size_t i = 0; i < paramLabels.size(); i++) {
        if (i > 0) paramArray += ",";
        paramArray += "{\"label\":" + jsonString(paramLabels[i]) + "}";
    }
    paramArray += "]";

    std::string result = "{";
    result += "\"signatures\":[{";
    result += "\"label\":" + jsonString(signatureLabel) + ",";
    result += "\"parameters\":" + paramArray;
    result += "}],";
    result += "\"activeSignature\":0,";
    result += "\"activeParameter\":" + jsonInt(activeParameter);
    result += "}";

    sendResponse(id, result);
}

// ============================================================================
// Tier 4: Helper — find all references to a name (token-based)
// ============================================================================

static std::vector<const gard::Token*> findAllReferences(
    const std::vector<gard::Token>& tokens, const std::string& name) {
    std::vector<const gard::Token*> refs;
    for (const auto& tok : tokens) {
        if (tok.type == gard::TokenType::Identifier && tok.value == name) {
            refs.push_back(&tok);
        }
    }
    return refs;
}

// ============================================================================
// Tier 4: textDocument/references
// ============================================================================

static void handleTextDocumentReferences(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);

    logMessage("references: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character));

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "[]");
        return;
    }

    DocumentState& doc = it->second;
    if (doc.tokens.empty()) {
        sendResponse(id, "[]");
        return;
    }

    // Find token at position
    const gard::Token* token = findTokenAtPosition(doc.tokens, line, character);
    if (!token || token->type != gard::TokenType::Identifier) {
        sendResponse(id, "[]");
        return;
    }

    // Find all tokens with the same name
    std::vector<const gard::Token*> refs = findAllReferences(doc.tokens, token->value);

    // Build response array of Location objects
    std::string result = "[";
    for (size_t i = 0; i < refs.size(); i++) {
        if (i > 0) result += ",";
        int refLine = refs[i]->location.line - 1;
        int refCol = refs[i]->location.column - 1;
        int refEndCol = refCol + (int)refs[i]->value.size();
        result += "{\"uri\":" + jsonString(uri) + ",\"range\":{";
        result += "\"start\":{\"line\":" + jsonInt(refLine) + ",\"character\":" + jsonInt(refCol) + "},";
        result += "\"end\":{\"line\":" + jsonInt(refLine) + ",\"character\":" + jsonInt(refEndCol) + "}";
        result += "}}";
    }
    result += "]";

    sendResponse(id, result);
}

// ============================================================================
// Tier 4: textDocument/prepareRename
// ============================================================================

static void handleTextDocumentPrepareRename(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);

    logMessage("prepareRename: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character));

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "null");
        return;
    }

    DocumentState& doc = it->second;
    if (doc.tokens.empty()) {
        sendResponse(id, "null");
        return;
    }

    // Find token at position
    const gard::Token* token = findTokenAtPosition(doc.tokens, line, character);
    if (!token || token->type != gard::TokenType::Identifier) {
        sendResponse(id, "null");
        return;
    }

    // Return the range of the identifier (rename is valid here)
    int tokLine = token->location.line - 1;
    int tokCol = token->location.column - 1;
    int tokEndCol = tokCol + (int)token->value.size();

    std::string result = "{\"range\":{";
    result += "\"start\":{\"line\":" + jsonInt(tokLine) + ",\"character\":" + jsonInt(tokCol) + "},";
    result += "\"end\":{\"line\":" + jsonInt(tokLine) + ",\"character\":" + jsonInt(tokEndCol) + "}";
    result += "}}";

    sendResponse(id, result);
}

// ============================================================================
// Tier 4: textDocument/rename
// ============================================================================

static void handleTextDocumentRename(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    int line = 0, character = 0;
    jsonGetPosition(params, line, character);
    std::string newName = jsonGetString(params, "newName");

    logMessage("rename: " + uri + " at " + std::to_string(line) + ":" + std::to_string(character) + " -> " + newName);

    if (newName.empty()) {
        sendResponse(id, "null");
        return;
    }

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "null");
        return;
    }

    DocumentState& doc = it->second;
    if (doc.tokens.empty()) {
        sendResponse(id, "null");
        return;
    }

    // Find token at position
    const gard::Token* token = findTokenAtPosition(doc.tokens, line, character);
    if (!token || token->type != gard::TokenType::Identifier) {
        sendResponse(id, "null");
        return;
    }

    // Find all tokens with the same name
    std::vector<const gard::Token*> refs = findAllReferences(doc.tokens, token->value);

    if (refs.empty()) {
        sendResponse(id, "null");
        return;
    }

    // Build WorkspaceEdit with TextEdits for each occurrence
    std::string edits = "[";
    for (size_t i = 0; i < refs.size(); i++) {
        if (i > 0) edits += ",";
        int refLine = refs[i]->location.line - 1;
        int refCol = refs[i]->location.column - 1;
        int refEndCol = refCol + (int)refs[i]->value.size();
        edits += "{\"range\":{";
        edits += "\"start\":{\"line\":" + jsonInt(refLine) + ",\"character\":" + jsonInt(refCol) + "},";
        edits += "\"end\":{\"line\":" + jsonInt(refLine) + ",\"character\":" + jsonInt(refEndCol) + "}";
        edits += "},\"newText\":" + jsonString(newName) + "}";
    }
    edits += "]";
    std::string result = "{\"changes\":{" + jsonString(uri) + ":" + edits + "}}";
}

// ============================================================================
// Tier 4: textDocument/codeAction
// ============================================================================

static void handleTextDocumentCodeAction(int id, const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);

    logMessage("codeAction: " + uri);

    auto it = documents.find(uri);
    if (it == documents.end()) {
        sendResponse(id, "[]");
        return;
    }

    DocumentState& doc = it->second;

    // Get the requested range from params
    size_t rangePos = params.find("range");
    int rangeStartLine = 0, rangeEndLine = 0;
    if (rangePos != std::string::npos) {
        std::string rangeSub = params.substr(rangePos);
        // Find "start" -> "line"
        size_t startPos = rangeSub.find("start");
        if (startPos != std::string::npos) {
            std::string startSub = rangeSub.substr(startPos);
            rangeStartLine = jsonGetInt(startSub, "line", 0);
        }
        // Find "end" -> "line"
        size_t endPos = rangeSub.find("end");
        if (endPos != std::string::npos) {
            std::string endSub = rangeSub.substr(endPos);
            rangeEndLine = jsonGetInt(endSub, "line", 0);
        }
    }

    std::vector<std::string> actions;

    // Re-run semantic analysis to get diagnostics for code actions
    std::string filename = uri;
    if (filename.rfind("file://", 0) == 0) {
        filename = filename.substr(7);
    }

    try {
        gard::Lexer lexer(doc.content, filename);
        std::vector<gard::Token> tokens = lexer.tokenize();
        if (!lexer.hasErrors()) {
            gard::Parser parser(tokens, filename);
            gard::Program program = parser.parse();

            if (!parser.hasErrors() || !program.statements.empty()) {
                gard::SemanticAnalyzer sema;
                sema.analyze(program);

                for (const auto& diag : sema.getDiagnostics()) {
                    int diagLine = diag.location.line > 0 ? diag.location.line - 1 : 0;

                    // Check if this diagnostic is within the requested range
                    if (diagLine < rangeStartLine || diagLine > rangeEndLine) continue;

                    // Check for "unused variable" pattern
                    if (diag.message.find("Unused") != std::string::npos ||
                        diag.message.find("unused") != std::string::npos) {
                        // Extract variable name from message if possible
                        std::string varName;
                        size_t quoteStart = diag.message.find('\'');
                        if (quoteStart != std::string::npos) {
                            size_t quoteEnd = diag.message.find('\'', quoteStart + 1);
                            if (quoteEnd != std::string::npos) {
                                varName = diag.message.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                            }
                        }

                        std::string title = varName.empty()
                            ? "Remove unused variable"
                            : "Remove unused variable '" + varName + "'";

                        // Create a TextEdit that deletes the entire line
                        int deleteLine = diagLine;
                        int nextLine = deleteLine + 1;
                        std::string action = "{";
                        action += "\"title\":" + jsonString(title) + ",";
                        action += "\"kind\":\"quickfix\",";
                        action += "\"diagnostics\":[{";
                        action += "\"range\":{\"start\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0},";
                        action += "\"end\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0}},";
                        action += "\"severity\":" + jsonInt(mapSeverity(diag.severity)) + ",";
                        action += "\"message\":" + jsonString(diag.message);
                        action += "}],";
                        action += "\"edit\":{\"changes\":{" + jsonString(uri) + ":[{";
                        action += "\"range\":{";
                        action += "\"start\":{\"line\":" + jsonInt(deleteLine) + ",\"character\":0},";
                        action += "\"end\":{\"line\":" + jsonInt(nextLine) + ",\"character\":0}";
                        action += "},\"newText\":\"\"";
                        action += "}]}}";
                        action += "}";

                        actions.push_back(action);
                    }

                    // For any diagnostic with a warning severity, offer to suppress
                    if (diag.severity == gard::DiagSeverity::Warning) {
                        std::string suppressTitle = "Suppress warning: " + diag.message;
                        // Truncate long messages
                        if (suppressTitle.size() > 80) {
                            suppressTitle = suppressTitle.substr(0, 77) + "...";
                        }

                        std::string action = "{";
                        action += "\"title\":" + jsonString(suppressTitle) + ",";
                        action += "\"kind\":\"quickfix\",";
                        action += "\"diagnostics\":[{";
                        action += "\"range\":{\"start\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0},";
                        action += "\"end\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0}},";
                        action += "\"severity\":" + jsonInt(mapSeverity(diag.severity)) + ",";
                        action += "\"message\":" + jsonString(diag.message);
                        action += "}],";
                        // No-op edit: insert a comment above the line
                        action += "\"edit\":{\"changes\":{" + jsonString(uri) + ":[{";
                        action += "\"range\":{";
                        action += "\"start\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0},";
                        action += "\"end\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0}";
                        action += "},\"newText\":\"// @suppress\\n\"";
                        action += "}]}}";
                        action += "}";

                        actions.push_back(action);
                    }

                    // Tier 8: Auto-import suggestion for undefined identifiers
                    if (diag.message.find("Undefined identifier") != std::string::npos ||
                        diag.message.find("requires import") != std::string::npos) {
                        // Extract the symbol name from the diagnostic message
                        std::string symName;
                        size_t q1 = diag.message.find('\'');
                        if (q1 != std::string::npos) {
                            size_t q2 = diag.message.find('\'', q1 + 1);
                            if (q2 != std::string::npos) {
                                symName = diag.message.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }

                        if (!symName.empty()) {
                            // Auto-import registry: symbol → module
                            static const std::unordered_map<std::string, std::string> importRegistry = {
                                {"Database", "gard/core"}, {"ORM", "gard/core"}, {"Query", "gard/core"},
                                {"ConnectionPool", "gard/core"},
                                {"Crypto", "gard/crypto"}, {"RSA", "gard/crypto"}, {"Hash", "gard/crypto"},
                                {"Base64", "gard/crypto"},
                                {"Compression", "gard/io"}, {"XML", "gard/io"},
                                {"HttpClient", "gard/network"}, {"HttpServer", "gard/network"},
                                {"WebSocket", "gard/network"}, {"WebSocketServer", "gard/network"},
                                {"TcpSocket", "gard/network"}, {"TcpServer", "gard/network"},
                                {"UdpSocket", "gard/network"},
                                {"FFI", "gard/native"}, {"Pointer", "gard/native"}, {"Memory", "gard/native"},
                                {"Window", "gard/graphics"}, {"Graphics", "gard/graphics"},
                                {"Input", "gard/graphics"}, {"Audio", "gard/graphics"},
                                {"Canvas", "gard/graphics"}, {"Camera", "gard/graphics"},
                                {"System", "gard/system"}, {"Compiler", "gard/system"}, {"MMap", "gard/system"},
                                {"ConcurrentMap", "gard/concurrent"}, {"ConcurrentList", "gard/concurrent"},
                                {"Channel", "gard/concurrent"}, {"Mutex", "gard/concurrent"},
                                {"Semaphore", "gard/concurrent"}, {"Barrier", "gard/concurrent"},
                                {"RWLock", "gard/concurrent"},
                                {"Mock", "gard/test"}, {"Reflect", "gard/reflect"},
                                {"Mail", "gard/mail"}, {"SMTP", "gard/mail"},
                            };

                            auto regIt = importRegistry.find(symName);
                            if (regIt != importRegistry.end()) {
                                std::string moduleName = regIt->second;
                                std::string importLine = "import { " + symName + " } from \"" + moduleName + "\";\n";
                                std::string title = "Add import: " + symName + " from \"" + moduleName + "\"";

                                std::string action = "{";
                                action += "\"title\":" + jsonString(title) + ",";
                                action += "\"kind\":\"quickfix\",";
                                action += "\"isPreferred\":true,";
                                action += "\"diagnostics\":[{";
                                action += "\"range\":{\"start\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0},";
                                action += "\"end\":{\"line\":" + jsonInt(diagLine) + ",\"character\":0}},";
                                action += "\"severity\":" + jsonInt(mapSeverity(diag.severity)) + ",";
                                action += "\"message\":" + jsonString(diag.message);
                                action += "}],";
                                action += "\"edit\":{\"changes\":{" + jsonString(uri) + ":[{";
                                action += "\"range\":{";
                                action += "\"start\":{\"line\":0,\"character\":0},";
                                action += "\"end\":{\"line\":0,\"character\":0}";
                                action += "},\"newText\":" + jsonString(importLine);
                                action += "}]}}";
                                action += "}";

                                actions.push_back(action);
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        logMessage("codeAction analysis failed: " + std::string(e.what()));
    } catch (...) {
        logMessage("codeAction analysis failed (unknown exception)");
    }

    // Build response array
    std::string result = "[";
    for (size_t i = 0; i < actions.size(); i++) {
        if (i > 0) result += ",";
        result += actions[i];
    }
    result += "]";

    sendResponse(id, result);
}
// ============================================================================
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
            entry += "}";
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

    // Fallback: look up in stdlib registry
    for (int i = 0; STDLIB_METHODS[i].ns != nullptr; i++) {
        if (funcName == STDLIB_METHODS[i].name) {
            for (int p = 0; STDLIB_METHODS[i].params[p].name != nullptr; p++) {
                paramNames.push_back(STDLIB_METHODS[i].params[p].name);
            }
            if (!paramNames.empty()) return paramNames;
        }
    }

    return paramNames;
}

// Helper: infer type from an initializer expression
static std::string inferTypeFromExpr(const gard::Expression* expr, const DocumentState& doc) {
    if (!expr) return "";
    switch (expr->kind) {
        case gard::ExprKind::IntLiteral:
            return "int";
        case gard::ExprKind::DoubleLiteral:
            return "double";
        case gard::ExprKind::FloatLiteral:
            return "float";
        case gard::ExprKind::LongLiteral:
            return "long";
        case gard::ExprKind::StringLiteral:
        case gard::ExprKind::TemplateLiteral:
            return "string";
        case gard::ExprKind::CharLiteral:
            return "char";
        case gard::ExprKind::BoolLiteral:
            return "bool";
        case gard::ExprKind::NullLiteral:
            return "null";
        case gard::ExprKind::Array:
            return "array";
        case gard::ExprKind::Map:
            return "map";
        case gard::ExprKind::New: {
            auto* newExpr = static_cast<const gard::NewExpr*>(expr);
            return newExpr->className;
        }
        case gard::ExprKind::Call: {
            // Try to look up the function's return type
            auto* call = static_cast<const gard::CallExpr*>(expr);
            std::string funcName = getCalleeName(call->callee.get());
            if (!funcName.empty() && doc.sema && doc.analysisValid) {
                gard::Symbol* sym = doc.sema->getSymbolTable().resolve(funcName);
                if (sym && (sym->kind == gard::SymbolKind::Function || sym->kind == gard::SymbolKind::Method)) {
                    if (!sym->returnType.empty() && sym->returnType != "void") {
                        return sym->returnType;
                    }
                }
            }
            return "";
        }
        case gard::ExprKind::Await: {
            // Infer from the inner expression
            auto* aw = static_cast<const gard::AwaitExpr*>(expr);
            return inferTypeFromExpr(aw->operand.get(), doc);
        }
        case gard::ExprKind::Identifier: {
            // Look up the variable's type in the symbol table
            auto* ident = static_cast<const gard::IdentifierExpr*>(expr);
            if (doc.sema && doc.analysisValid) {
                gard::Symbol* sym = doc.sema->getSymbolTable().resolve(ident->name);
                if (sym && !sym->typeName.empty()) {
                    return sym->typeName;
                }
            }
            return "";
        }
        default:
            return "";
    }
}

// Helper: collect VarDeclarationStmt nodes from statements (recursive)
static void collectVarDeclsFromStmts(const std::vector<gard::StmtPtr>& stmts,
                                      std::vector<const gard::VarDeclarationStmt*>& varDecls);

static void collectVarDeclsFromStmt(const gard::Statement* stmt,
                                     std::vector<const gard::VarDeclarationStmt*>& varDecls) {
    if (!stmt) return;

    switch (stmt->kind) {
        case gard::StmtKind::VarDeclaration: {
            varDecls.push_back(static_cast<const gard::VarDeclarationStmt*>(stmt));
            break;
        }
        case gard::StmtKind::Block: {
            auto* bs = static_cast<const gard::BlockStmt*>(stmt);
            collectVarDeclsFromStmts(bs->statements, varDecls);
            break;
        }
        case gard::StmtKind::If: {
            auto* ifs = static_cast<const gard::IfStmt*>(stmt);
            collectVarDeclsFromStmt(ifs->thenBranch.get(), varDecls);
            collectVarDeclsFromStmt(ifs->elseBranch.get(), varDecls);
            break;
        }
        case gard::StmtKind::For: {
            auto* fs = static_cast<const gard::ForStmt*>(stmt);
            collectVarDeclsFromStmt(fs->initializer.get(), varDecls);
            collectVarDeclsFromStmt(fs->body.get(), varDecls);
            break;
        }
        case gard::StmtKind::While: {
            auto* ws = static_cast<const gard::WhileStmt*>(stmt);
            collectVarDeclsFromStmt(ws->body.get(), varDecls);
            break;
        }
        case gard::StmtKind::FunctionDeclaration: {
            auto* fn = static_cast<const gard::FunctionDeclStmt*>(stmt);
            collectVarDeclsFromStmts(fn->body, varDecls);
            break;
        }
        case gard::StmtKind::Export: {
            auto* exp = static_cast<const gard::ExportStmt*>(stmt);
            collectVarDeclsFromStmt(exp->declaration.get(), varDecls);
            break;
        }
        case gard::StmtKind::Class: {
            auto* cls = static_cast<const gard::ClassDeclStmt*>(stmt);
            for (const auto& method : cls->methods) {
                collectVarDeclsFromStmts(method.body, varDecls);
            }
            if (cls->constructor) {
                collectVarDeclsFromStmts(cls->constructor->body, varDecls);
            }
            break;
        }
        default:
            break;
    }
}

static void collectVarDeclsFromStmts(const std::vector<gard::StmtPtr>& stmts,
                                      std::vector<const gard::VarDeclarationStmt*>& varDecls) {
    for (const auto& stmt : stmts) {
        collectVarDeclsFromStmt(stmt.get(), varDecls);
    }
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

    // --- Type inference hints for untyped variable declarations ---
    std::vector<const gard::VarDeclarationStmt*> varDecls;
    collectVarDeclsFromStmts(doc.program.statements, varDecls);

    for (const auto* varDecl : varDecls) {
        // Only show hint if there's no explicit type annotation but there IS an initializer
        if (varDecl->type != nullptr) continue;
        if (!varDecl->initializer) continue;

        std::string inferredType = inferTypeFromExpr(varDecl->initializer.get(), doc);
        if (inferredType.empty()) continue;

        // Position the hint after the variable name
        // Find the variable name token in the token list for exact positioning
        int hintLine = varDecl->location.line - 1;
        int hintChar = 0;
        // Search tokens for the variable name on this line
        for (const auto& tok : doc.tokens) {
            if (tok.location.line == varDecl->location.line &&
                tok.type == gard::TokenType::Identifier &&
                tok.value == varDecl->name) {
                // Found the variable name token — hint goes right after it
                hintChar = tok.location.column - 1 + (int)tok.value.size();
                break;
            }
        }
        // Fallback if token not found
        if (hintChar == 0) {
            hintChar = varDecl->location.column - 1 + 4 + (int)varDecl->name.size(); // "let " = 4
        }

        std::string hint = "{";
        hint += "\"position\":{\"line\":" + jsonInt(hintLine) + ",\"character\":" + jsonInt(hintChar) + "},";
        hint += "\"label\":" + jsonString(": " + inferredType) + ",";
        hint += "\"kind\":1,"; // Type hint
        hint += "\"paddingLeft\":false";
        hint += "}";
        hints.push_back(hint);
    }

    // --- Parameter name hints for function call arguments ---
    std::vector<const gard::CallExpr*> calls;
    collectCallsFromStmts(doc.program.statements, calls);

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
  std::string mRange = "{" + jsonString("startLine") + ":" + jsonInt(mStart) +
                                "," + jsonString("endLine") + ":" + jsonInt(mEnd) +
                                "," + jsonString("kind") + ":" + jsonString("region") + "}";
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
 std::string range = "{" + jsonString("startLine") + ":" + jsonInt(startLine) +
                    "," + jsonString("endLine") + ":" + jsonInt(endLine) +
                    "," + jsonString("kind") + ":" + jsonString(kind) + "}";
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
std::string range = "{" + jsonString("startLine") + ":" + jsonInt(commentStart) +
                    "," + jsonString("endLine") + ":" + jsonInt(commentEnd) +
                    "," + jsonString("kind") + ":" + jsonString("comment") + "}";
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
        std::string range = "{" + jsonString("startLine") + ":" + jsonInt(importStart) +
            "," + jsonString("endLine") + ":" + jsonInt(importEnd) +
            "," + jsonString("kind") + ":" + jsonString("imports") + "}";
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

// LSP Method Handlers
// ============================================================================

static void handleInitialize(int id, const std::string& /*params*/) {
    logMessage("Handling initialize");

    std::string capabilities = "{"
        "\"capabilities\":{"
            "\"textDocumentSync\":{"
                "\"openClose\":true,"
                "\"change\":1"
            "},"
            "\"hoverProvider\":true,"
            "\"definitionProvider\":true,"
            "\"referencesProvider\":true,"
            "\"renameProvider\":{"
                "\"prepareProvider\":true"
            "},"
            "\"codeActionProvider\":true,"
            "\"documentSymbolProvider\":true,"
            "\"documentFormattingProvider\":true,"
            "\"completionProvider\":{"
                "\"triggerCharacters\":[\".\"],"
                "\"resolveProvider\":false"
            "},"
            "\"signatureHelpProvider\":{"
                "\"triggerCharacters\":[\"(\",\",\"]"
            "},"
            "\"semanticTokensProvider\":{"
                "\"full\":true,"
                "\"legend\":{"
                    "\"tokenTypes\":[\"namespace\",\"type\",\"class\",\"enum\",\"interface\",\"struct\",\"typeParameter\",\"parameter\",\"variable\",\"property\",\"enumMember\",\"function\",\"method\",\"keyword\",\"comment\",\"string\",\"number\",\"operator\",\"decorator\"],"
                    "\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\",\"async\"]"
                "}"
            "},"
            "\"workspaceSymbolProvider\":true,"
            "\"inlayHintProvider\":true,"
            "\"foldingRangeProvider\":true"
        "},"
        "\"serverInfo\":{"
            "\"name\":\"gard-lsp\","
            "\"version\":\"0.5.0\""
        "}"
    "}";

    sendResponse(id, capabilities);
}
static void handleShutdown(int id) {
    logMessage("Handling shutdown");
    sendResponse(id, "null");
}

static void handleTextDocumentDidOpen(const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    std::string text = jsonGetDocumentText(params, false);

    logMessage("didOpen: " + uri + " (" + std::to_string(text.size()) + " bytes)");

    // Extract filename from URI
    std::string filename = uri;
    if (filename.rfind("file://", 0) == 0) {
        filename = filename.substr(7);
    }

    DocumentState& doc = documents[uri];
    doc.content = text;
    doc.uri = uri;

    // Analyze for Tier 2 features (tokens, AST, symbol table)
    analyzeDocument(doc, filename);

    // Publish diagnostics
    publishDiagnostics(uri, doc);
}

static void handleTextDocumentDidChange(const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    std::string text = jsonGetDocumentText(params, true);

    logMessage("didChange: " + uri + " (" + std::to_string(text.size()) + " bytes)");

    // Extract filename from URI
    std::string filename = uri;
    if (filename.rfind("file://", 0) == 0) {
        filename = filename.substr(7);
    }

    DocumentState& doc = documents[uri];
    doc.content = text;
    doc.uri = uri;

    // Re-analyze for Tier 2 features
    analyzeDocument(doc, filename);

    // Publish diagnostics
    publishDiagnostics(uri, doc);
}

static void handleTextDocumentDidClose(const std::string& params) {
    std::string uri = jsonGetTextDocumentUri(params);
    logMessage("didClose: " + uri);

    documents.erase(uri);

    // Clear diagnostics for closed document
    std::string clearParams = "{\"uri\":" + jsonString(uri) + ",\"diagnostics\":[]}";
    sendNotification("textDocument/publishDiagnostics", clearParams);
}

// ============================================================================
// Main Loop
// ============================================================================

int main() {
    logMessage("Gard LSP server starting (Tier 5)...");

    // Disable stdout buffering for proper LSP communication
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool running = true;
    bool shutdownRequested = false;

    while (running) {
        try {
        std::string message = readMessage();
        if (message.empty()) continue;

        logMessage("Received: " + message.substr(0, 200) + (message.size() > 200 ? "..." : ""));

        // Extract method and id
        std::string method = jsonGetString(message, "method");
        int id = jsonGetInt(message, "id");

        if (method == "initialize") {
            handleInitialize(id, message);
        } else if (method == "initialized") {
            logMessage("Client initialized");
        } else if (method == "shutdown") {
            shutdownRequested = true;
            handleShutdown(id);
        } else if (method == "exit") {
            logMessage("Exit requested");
            running = false;
            std::exit(shutdownRequested ? 0 : 1);
        } else if (method == "textDocument/didOpen") {
            handleTextDocumentDidOpen(message);
        } else if (method == "textDocument/didChange") {
            handleTextDocumentDidChange(message);
        } else if (method == "textDocument/didClose") {
            handleTextDocumentDidClose(message);
        } else if (method == "textDocument/hover") {
            handleTextDocumentHover(id, message);
        } else if (method == "textDocument/definition") {
            handleTextDocumentDefinition(id, message);
        } else if (method == "textDocument/documentSymbol") {
            handleTextDocumentDocumentSymbol(id, message);
        } else if (method == "textDocument/formatting") {
            handleTextDocumentFormatting(id, message);
        } else if (method == "textDocument/completion") {
            handleTextDocumentCompletion(id, message);
        } else if (method == "textDocument/signatureHelp") {
            handleTextDocumentSignatureHelp(id, message);
        } else if (method == "textDocument/references") {
            handleTextDocumentReferences(id, message);
        } else if (method == "textDocument/rename") {
            handleTextDocumentRename(id, message);
        } else if (method == "textDocument/prepareRename") {
            handleTextDocumentPrepareRename(id, message);
        } else if (method == "textDocument/codeAction") {
            handleTextDocumentCodeAction(id, message);
        } else if (method == "textDocument/semanticTokens/full") {
            handleSemanticTokensFull(id, message);
        } else if (method == "workspace/symbol") {
            handleWorkspaceSymbol(id, message);
        } else if (method == "textDocument/inlayHint") {
            handleInlayHint(id, message);
        } else if (method == "textDocument/foldingRange") {
            handleFoldingRange(id, message);
        } else if (method == "textDocument/diagnostic") {
            // Pull-based diagnostics — return items from last analysis
            std::string diagUri = jsonGetTextDocumentUri(message);
            auto dit = documents.find(diagUri);
            if (dit != documents.end()) {
                // Re-use the push diagnostics we already computed — just return empty
                // (we already pushed them via publishDiagnostics on didOpen/didChange)
    sendResponse(id, "{\"kind\":\"full\",\"items\":[]}");
            } else {
                sendResponse(id, "{\"kind\":\"full\",\"items\":[]}");
            }
        } else if (method == "workspace/didChangeWatchedFiles") {
            // Notification — no response needed
            logMessage("Watched files changed (ignored)");
        } else if (!method.empty()) {
            logMessage("Unhandled method: " + method);
            // $/ prefixed methods are notifications that can be safely ignored
            if (method.rfind("$/", 0) == 0) {
                // Ignore $/ notifications (cancelRequest, progress, etc.)
            } else if (id >= 0) {
                // It's a request — send method not found
               std::string error = "{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) +
                    ",\"error\":{\"code\":-32601,\"message\":" +
                    jsonString("Method not found: " + method) + "}}";
                sendMessage(error);
            }
        } else {
            logMessage("Message without method field, ignoring");
        }
        } catch (const std::exception& e) {
            logMessage("Exception in main loop: " + std::string(e.what()));
        } catch (...) {
            logMessage("Unknown exception in main loop");
        }
    }

    return 0;
}
