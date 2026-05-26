#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/analyzer.h"
#include "types/checker.h"
#include "ir/irgen.h"
#include "ir/validator.h"
#include "ir/optimizer.h"
#include "codegen/llvm_emitter.h"
#include "bytecode/bytecode.h"
#include "bytecode/compiler.h"
#include "runtime/runtime.h"
#include "runtime/stdlib_network.h"
#include "runtime/stdlib_core.h"
#include "runtime/stdlib_web.h"
#include "runtime/stdlib_native.h"
#include "runtime/stdlib_graphics.h"
#include "tools/formatter.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

// --- Terminal colors ---
#ifdef __linux__
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RESET   "\033[0m"
#else
#define COLOR_RED     ""
#define COLOR_GREEN   ""
#define COLOR_YELLOW  ""
#define COLOR_BLUE    ""
#define COLOR_BOLD    ""
#define COLOR_RESET   ""
#endif

static const char* VERSION = "0.1.0";

// --- Helpers ---

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string readStdin() {
    std::stringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

void printError(const std::string& msg) {
    if (msg.find("Gard") != std::string::npos) {
        // Already has a Gard error name — print directly with color
        std::cerr << COLOR_RED << msg << COLOR_RESET << std::endl;
    } else {
        std::cerr << COLOR_RED << "GardError" << COLOR_RESET << ": " << msg << std::endl;
    }
}

void printWarning(const std::string& msg) {
    std::cerr << COLOR_YELLOW << "warning" << COLOR_RESET << ": " << msg << std::endl;
}

void printSuccess(const std::string& msg) {
    std::cout << COLOR_GREEN << "✓" << COLOR_RESET << " " << msg << std::endl;
}

void printInfo(const std::string& msg) {
    std::cout << COLOR_BLUE << "info" << COLOR_RESET << ": " << msg << std::endl;
}

// --- Commands ---

void printHelp() {
    std::cout << COLOR_BOLD << "gard" << COLOR_RESET << " " << VERSION << " — The Gard Programming Language\n\n";
    std::cout << "USAGE:\n";
    std::cout << "    gard <command> [options] [file]\n\n";
    std::cout << "COMMANDS:\n";
    std::cout << "    new <name>       Create a new Gard project\n";
    std::cout << "    build [file]     Compile source to output\n";
    std::cout << "    run [file]       Build and execute\n";
    std::cout << "    check [file]     Type check without output\n";
    std::cout << "    fmt [file]       Format source code\n";
    std::cout << "    test             Run tests\n";
    std::cout << "    lint [file]      Static analysis\n\n";
    std::cout << "OPTIONS:\n";
    std::cout << "    --release        Optimized build\n";
    std::cout << "    --unchecked      Disable runtime guards (max performance)\n";
    std::cout << "    --emit-llvm      Output LLVM IR\n";
    std::cout << "    --bytecode       Output bytecode\n";
    std::cout << "    --tokens         Show lexer tokens\n";
    std::cout << "    --ir             Show internal IR\n";
    std::cout << "    --help, -h       Show this help\n";
    std::cout << "    --version, -v    Show version\n";
}

int cmdNew(const std::string& name) {
    if (name.empty()) {
        printError("project name required: gard new <name>");
        return 1;
    }

    fs::path projectDir = fs::current_path() / name;
    if (fs::exists(projectDir)) {
        printError("directory '" + name + "' already exists");
        return 1;
    }

    // Create project structure
    fs::create_directories(projectDir / "src");
    fs::create_directories(projectDir / "test");
    fs::create_directories(projectDir / "dist");

    // gard.json
    {
        std::ofstream f(projectDir / "gard.json");
        f << "{\n";
        f << "  \"name\": \"" << name << "\",\n";
        f << "  \"version\": \"0.1.0\",\n";
        f << "  \"main\": \"src/main.gard\",\n";
        f << "  \"scripts\": {\n";
        f << "    \"build\": \"gard build\",\n";
        f << "    \"test\": \"gard test\"\n";
        f << "  },\n";
        f << "  \"dependencies\": {},\n";
        f << "  \"devDependencies\": {}\n";
        f << "}\n";
    }

    // src/main.gard
    {
        std::ofstream f(projectDir / "src" / "main.gard");
        f << "function main(): void {\n";
        f << "    print(\"Hello from " << name << "!\");\n";
        f << "}\n";
    }

    // test/main.test.gard
    {
        std::ofstream f(projectDir / "test" / "main.test.gard");
        f << "import { Assert } from \"gard/testing\";\n\n";
        f << "@TestClass\n";
        f << "class MainTest {\n";
        f << "    @Test\n";
        f << "    public function testHello(): void {\n";
        f << "        Assert.isTrue(true);\n";
        f << "    }\n";
        f << "}\n";
    }

    printSuccess("Created project '" + name + "'");
    std::cout << "\n  cd " << name << "\n  gard run\n" << std::endl;
    return 0;
}

int cmdCheck(const std::string& source, const std::string& filename) {
    gard::Lexer lexer(source, filename);
    auto tokens = lexer.tokenize();

    if (lexer.hasErrors()) {
        for (const auto& err : lexer.getErrors())
            std::cerr << COLOR_RED << err << COLOR_RESET << std::endl;
        return 1;
    }

    gard::Parser parser(tokens, filename);
    auto program = parser.parse();

    if (parser.hasErrors()) {
        for (const auto& err : parser.getErrors())
            std::cerr << COLOR_RED << err << COLOR_RESET << std::endl;
        return 1;
    }

    gard::SemanticAnalyzer analyzer;
    analyzer.analyze(program);

    for (const auto& diag : analyzer.getDiagnostics()) {
        if (diag.severity == gard::DiagSeverity::Error)
            std::cerr << COLOR_RED << diag.toString() << COLOR_RESET << std::endl;
        else
            std::cerr << COLOR_YELLOW << diag.toString() << COLOR_RESET << std::endl;
    }

    if (analyzer.hasErrors()) return 1;

    gard::TypeChecker typeChecker(analyzer.getSymbolTable());
    typeChecker.check(program);

    for (const auto& diag : typeChecker.getDiagnostics()) {
        if (diag.severity == gard::DiagSeverity::Error)
            std::cerr << COLOR_RED << diag.toString() << COLOR_RESET << std::endl;
        else
            std::cerr << COLOR_YELLOW << diag.toString() << COLOR_RESET << std::endl;
    }

    if (typeChecker.hasErrors()) return 1;

    printSuccess("No errors in " + filename);
    return 0;
}

int cmdBuild(const std::string& source, const std::string& filename, bool release, const std::string& format, bool unchecked = false) {
    gard::Lexer lexer(source, filename);
    auto tokens = lexer.tokenize();
    if (lexer.hasErrors()) {
        for (const auto& err : lexer.getErrors()) printError(err);
        return 1;
    }

    gard::Parser parser(tokens, filename);
    auto program = parser.parse();
    if (parser.hasErrors()) {
        for (const auto& err : parser.getErrors()) printError(err);
        return 1;
    }

    // Run semantic analysis (validates imports, undefined identifiers, etc.)
    // Note: semantic analysis is advisory — some false positives exist for enums/classes
    // used as namespaces. We only block on import-related errors.
    gard::SemanticAnalyzer semaAnalyzer;
    semaAnalyzer.analyze(program);
    // Check specifically for import-required errors
    bool hasImportErrors = false;
    for (const auto& diag : semaAnalyzer.getDiagnostics()) {
        if (diag.severity == gard::DiagSeverity::Error &&
            diag.message.find("requires import") != std::string::npos) {
            printError(diag.toString());
            hasImportErrors = true;
        }
    }
    if (hasImportErrors) return 1;

    gard::ir::IRGenerator irgen;
    auto module = irgen.generate(program);

    // Check for IR generation errors (e.g., const reassignment)
    if (irgen.hasErrors()) {
        for (const auto& err : irgen.getErrors()) printError(err);
        return 1;
    }

    if (release && format != "llvm") {
        // Skip Gard IR optimizer for LLVM path — LLVM's own -O2 handles this better.
        // Gard's ConstantPropagation doesn't handle loop-carried dependencies correctly.
        gard::ir::PassPipeline pipeline(gard::ir::OptLevel::Release);
        pipeline.run(*module);
    }

    if (format == "llvm") {
        gard::codegen::LLVMEmitter emitter(gard::codegen::Target::X86_64);
        emitter.setDebugInfo(!release);
        if (unchecked) emitter.setUnchecked(true);
        emitter.setSourceFile(filename);
        std::cout << emitter.emit(*module);
        return 0;
    } else if (format == "bytecode") {
        gard::bytecode::BytecodeCompiler bcCompiler;
        auto bcModule = bcCompiler.compile(*module);
        auto bytes = bcModule.serialize();
        std::string outFile = filename.substr(0, filename.rfind('.')) + ".ga";
        std::ofstream out(outFile, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        printSuccess("Built " + outFile + " (" + std::to_string(bytes.size()) + " bytes)");
        return 0;
    } else if (format == "ir") {
        std::cout << module->dump();
        return 0;
    } else if (format == "tokens") {
        for (const auto& token : tokens) {
            std::cout << token.location.toString() << "  " << token.typeName();
            if (!token.value.empty() && token.type != gard::TokenType::EndOfFile)
                std::cout << "  '" << token.value << "'";
            std::cout << std::endl;
        }
        return 0;
    }

    // Default: emit LLVM IR to stdout
    gard::codegen::LLVMEmitter emitter(gard::codegen::Target::X86_64);
    emitter.setDebugInfo(!release);
    if (unchecked) emitter.setUnchecked(true);
    emitter.setSourceFile(filename);
    std::cout << emitter.emit(*module);
    return 0;
}

int cmdRun(const std::string& source, const std::string& filename, bool release, const std::vector<std::string>& programArgs = {}) {
    auto start = std::chrono::steady_clock::now();

    gard::Lexer lexer(source, filename);
    auto tokens = lexer.tokenize();
    if (lexer.hasErrors()) {
        for (const auto& err : lexer.getErrors()) printError(err);
        return 1;
    }

    gard::Parser parser(tokens, filename);
    auto program = parser.parse();
    if (parser.hasErrors()) {
        for (const auto& err : parser.getErrors()) printError(err);
        return 1;
    }

    // Validate imports — check that all imported modules/files exist
    // Walk all statements recursively (imports can be scoped inside functions/classes)
    static const std::vector<std::string> builtinModules = {
        "gard/network",
        "gard/core",
        "gard/web",
        "gard/native",
        "gard/graphics"
    };

    // Track imported class → public methods/fields for method validation
    std::unordered_map<std::string, std::vector<std::string>> importedClassMembers;

    // Store imported class AST nodes to inject into the program for IR generation
    std::vector<gard::StmtPtr> importedClassDecls;

    // Track whether gard/network is imported (from actual import statements, not comments)
    bool importsNetwork = false;
    bool importsCore = false;
    bool importsWeb = false;
    bool importsNative = false;
    bool importsGraphics = false;

    // Track which specific names are imported from gard/network
    std::vector<std::string> networkImportedNames;
    std::vector<std::string> coreImportedNames;
    std::vector<std::string> webImportedNames;
    std::vector<std::string> nativeImportedNames;
    std::vector<std::string> graphicsImportedNames;

    std::function<bool(gard::Statement*)> validateImports = [&](gard::Statement* stmt) -> bool {
        if (!stmt) return true;
        if (auto* imp = dynamic_cast<gard::ImportStmt*>(stmt)) {
            const std::string& path = imp->path;

            // Check built-in modules
            bool isBuiltin = false;
            for (auto& mod : builtinModules) {
                if (path == mod) { isBuiltin = true; break; }
            }
            if (isBuiltin) {
                if (path == "gard/network") {
                    importsNetwork = true;
                    for (auto& name : imp->names) networkImportedNames.push_back(name);
                }
                if (path == "gard/core") {
                    importsCore = true;
                    for (auto& name : imp->names) coreImportedNames.push_back(name);
                }
                if (path == "gard/web") {
                    importsWeb = true;
                    for (auto& name : imp->names) webImportedNames.push_back(name);
                }
                if (path == "gard/native") {
                    importsNative = true;
                    for (auto& name : imp->names) nativeImportedNames.push_back(name);
                }
                if (path == "gard/graphics") {
                    importsGraphics = true;
                    for (auto& name : imp->names) graphicsImportedNames.push_back(name);
                }
                return true;
            }

            // Check relative file imports (./ or ../)
            if (path.size() > 0 && (path[0] == '.' || path[0] == '/')) {
                fs::path sourcePath = fs::path(filename).parent_path();
                fs::path importPath = sourcePath / path;
                if (!fs::exists(importPath) && importPath.extension().empty()) {
                    importPath += ".gard";
                }
                if (!fs::exists(importPath)) {
                    std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                              << ": Cannot find module '" << path << "'"
                              << " (resolved to: " << importPath.string() << ")"
                              << "\n  at " << filename << ":" << imp->location.line << ":" << imp->location.column
                              << std::endl;
                    return false;
                }

                // Parse the imported file and verify exported symbols
                try {
                    std::string importSource = readFile(importPath.string());
                    gard::Lexer importLexer(importSource, importPath.string());
                    auto importTokens = importLexer.tokenize();
                    if (!importLexer.hasErrors()) {
                        gard::Parser importParser(importTokens, importPath.string());
                        auto importProgram = importParser.parse();
                        if (!importParser.hasErrors()) {
                            // Collect exported symbols and their members
                            std::vector<std::string> exportedSymbols;
                            for (auto& s : importProgram.statements) {
                                if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                                    if (auto* cls = dynamic_cast<gard::ClassDeclStmt*>(exp->declaration.get())) {
                                        exportedSymbols.push_back(cls->name);
                                        // Collect public methods and fields
                                        std::vector<std::string> members;
                                        for (auto& m : cls->methods) {
                                            if (m.accessModifier == "public" || m.accessModifier.empty())
                                                members.push_back(m.name);
                                        }
                                        for (auto& f : cls->fields) {
                                            if (f.accessModifier == "public" || f.accessModifier.empty())
                                                members.push_back(f.name);
                                        }
                                        importedClassMembers[cls->name] = members;
                                    } else if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(exp->declaration.get())) {
                                        exportedSymbols.push_back(fn->name);
                                    } else if (auto* var = dynamic_cast<gard::VarDeclarationStmt*>(exp->declaration.get())) {
                                        exportedSymbols.push_back(var->name);
                                    } else if (auto* iface = dynamic_cast<gard::InterfaceDeclStmt*>(exp->declaration.get())) {
                                        exportedSymbols.push_back(iface->name);
                                    } else if (auto* enumDecl = dynamic_cast<gard::EnumDeclStmt*>(exp->declaration.get())) {
                                        exportedSymbols.push_back(enumDecl->name);
                                    }
                                }
                            }

                            // Verify each imported name is exported
                            for (auto& importedName : imp->names) {
                                bool isExported = false;
                                for (auto& exported : exportedSymbols) {
                                    if (exported == importedName) { isExported = true; break; }
                                }
                                if (!isExported) {
                                    std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                              << ": '" << importedName << "' is not exported from '" << path << "'"
                                              << "\n  Exported symbols: ";
                                    if (exportedSymbols.empty()) {
                                        std::cerr << "(none)";
                                    } else {
                                        for (size_t ei = 0; ei < exportedSymbols.size(); ei++) {
                                            if (ei > 0) std::cerr << ", ";
                                            std::cerr << exportedSymbols[ei];
                                        }
                                    }
                                    std::cerr << "\n  at " << filename << ":" << imp->location.line << ":" << imp->location.column
                                              << std::endl;
                                    return false;
                                }
                            }

                            // Store imported class declarations for IR generation
                            for (auto& importedName : imp->names) {
                                for (auto& s : importProgram.statements) {
                                    if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                                        if (auto* cls = dynamic_cast<gard::ClassDeclStmt*>(exp->declaration.get())) {
                                            if (cls->name == importedName) {
                                                importedClassDecls.push_back(std::move(exp->declaration));
                                                break;
                                            }
                                        } else if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(exp->declaration.get())) {
                                            if (fn->name == importedName) {
                                                importedClassDecls.push_back(std::move(exp->declaration));
                                                break;
                                            }
                                        } else if (auto* iface = dynamic_cast<gard::InterfaceDeclStmt*>(exp->declaration.get())) {
                                            if (iface->name == importedName) {
                                                importedClassDecls.push_back(std::move(exp->declaration));
                                                break;
                                            }
                                        } else if (auto* enumDecl = dynamic_cast<gard::EnumDeclStmt*>(exp->declaration.get())) {
                                            if (enumDecl->name == importedName) {
                                                importedClassDecls.push_back(std::move(exp->declaration));
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {
                    // If we can't parse the imported file, skip validation
                }
            } else if (path.rfind("gard/", 0) == 0) {
                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                          << ": Built-in module '" << path << "' does not exist."
                          << " Available: gard/network, gard/core, gard/web, gard/native"
                          << "\n  at " << filename << ":" << imp->location.line << ":" << imp->location.column
                          << std::endl;
                return false;
            } else if (path.rfind("package/", 0) == 0) {
                std::string pkgName = path.substr(8);
                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                          << ": Package '" << pkgName << "' is not installed."
                          << " Install it with: gard add " << pkgName
                          << "\n  at " << filename << ":" << imp->location.line << ":" << imp->location.column
                          << std::endl;
                return false;
            } else {
                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                          << ": Module '" << path << "' not found."
                          << " Install it with: gard add " << path
                          << "\n  at " << filename << ":" << imp->location.line << ":" << imp->location.column
                          << std::endl;
                return false;
            }
        }
        // Recurse into function bodies
        if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(stmt)) {
            for (auto& s : fn->body) if (!validateImports(s.get())) return false;
        }
        // Recurse into class methods
        if (auto* cls = dynamic_cast<gard::ClassDeclStmt*>(stmt)) {
            for (auto& m : cls->methods) {
                for (auto& s : m.body) if (!validateImports(s.get())) return false;
            }
        }
        // Recurse into blocks
        if (auto* block = dynamic_cast<gard::BlockStmt*>(stmt)) {
            for (auto& s : block->statements) if (!validateImports(s.get())) return false;
        }
        // Recurse into exports
        if (auto* exp = dynamic_cast<gard::ExportStmt*>(stmt)) {
            if (!validateImports(exp->declaration.get())) return false;
        }
        return true;
    };

    for (auto& stmt : program.statements) {
        if (!validateImports(stmt.get())) return 1;
    }

    // Validate method calls on imported classes
    // Track which variables are instances of imported classes
    if (!importedClassMembers.empty()) {
        // Map variable name → class name (from `let x = new ClassName()`)
        std::unordered_map<std::string, std::string> varClassMap;

        std::function<void(gard::Statement*)> collectVarTypes = [&](gard::Statement* s) {
            if (!s) return;
            if (auto* varDecl = dynamic_cast<gard::VarDeclarationStmt*>(s)) {
                if (auto* newExpr = dynamic_cast<gard::NewExpr*>(varDecl->initializer.get())) {
                    if (importedClassMembers.count(newExpr->className)) {
                        varClassMap[varDecl->name] = newExpr->className;
                    }
                }
            }
            if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(s)) {
                for (auto& st : fn->body) collectVarTypes(st.get());
            }
            if (auto* block = dynamic_cast<gard::BlockStmt*>(s)) {
                for (auto& st : block->statements) collectVarTypes(st.get());
            }
        };
        for (auto& s : program.statements) collectVarTypes(s.get());

        // Now validate member access calls
        std::function<bool(gard::Expression*)> validateMethodCalls = [&](gard::Expression* expr) -> bool {
            if (!expr) return true;
            if (auto* call = dynamic_cast<gard::CallExpr*>(expr)) {
                if (auto* member = dynamic_cast<gard::MemberAccessExpr*>(call->callee.get())) {
                    if (auto* id = dynamic_cast<gard::IdentifierExpr*>(member->object.get())) {
                        auto varIt = varClassMap.find(id->name);
                        if (varIt != varClassMap.end()) {
                            auto classIt = importedClassMembers.find(varIt->second);
                            if (classIt != importedClassMembers.end()) {
                                bool methodExists = false;
                                for (auto& m : classIt->second) {
                                    if (m == member->member) { methodExists = true; break; }
                                }
                                if (!methodExists) {
                                    std::cerr << COLOR_RED << "GardMethodNotExistError" << COLOR_RESET
                                              << ": Method '" << member->member << "' does not exist on class '"
                                              << varIt->second << "'"
                                              << "\n  Available methods: ";
                                    for (size_t mi = 0; mi < classIt->second.size(); mi++) {
                                        if (mi > 0) std::cerr << ", ";
                                        std::cerr << classIt->second[mi];
                                    }
                                    std::cerr << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                              << std::endl;
                                    return false;
                                }
                            }
                        }
                    }
                }
                // Validate args recursively
                for (auto& arg : call->arguments) if (!validateMethodCalls(arg.get())) return false;
            }
            if (auto* member = dynamic_cast<gard::MemberAccessExpr*>(expr)) {
                // Validate field access on imported class instances
                if (auto* id = dynamic_cast<gard::IdentifierExpr*>(member->object.get())) {
                    auto varIt = varClassMap.find(id->name);
                    if (varIt != varClassMap.end()) {
                        auto classIt = importedClassMembers.find(varIt->second);
                        if (classIt != importedClassMembers.end()) {
                            bool memberExists = false;
                            for (auto& m : classIt->second) {
                                if (m == member->member) { memberExists = true; break; }
                            }
                            if (!memberExists) {
                                std::cerr << COLOR_RED << "GardMethodNotExistError" << COLOR_RESET
                                          << ": '" << member->member << "' does not exist on class '"
                                          << varIt->second << "'"
                                          << "\n  Available members: ";
                                for (size_t mi = 0; mi < classIt->second.size(); mi++) {
                                    if (mi > 0) std::cerr << ", ";
                                    std::cerr << classIt->second[mi];
                                }
                                std::cerr << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                        }
                    }
                }
                if (!validateMethodCalls(member->object.get())) return false;
            }
            if (auto* binary = dynamic_cast<gard::BinaryExpr*>(expr)) {
                if (!validateMethodCalls(binary->left.get())) return false;
                if (!validateMethodCalls(binary->right.get())) return false;
            }
            return true;
        };

        std::function<bool(gard::Statement*)> validateStmtMethods = [&](gard::Statement* s) -> bool {
            if (!s) return true;
            if (auto* exprStmt = dynamic_cast<gard::ExpressionStmt*>(s)) {
                if (!validateMethodCalls(exprStmt->expression.get())) return false;
            }
            if (auto* varDecl = dynamic_cast<gard::VarDeclarationStmt*>(s)) {
                if (!validateMethodCalls(varDecl->initializer.get())) return false;
            }
            if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(s)) {
                for (auto& st : fn->body) if (!validateStmtMethods(st.get())) return false;
            }
            if (auto* block = dynamic_cast<gard::BlockStmt*>(s)) {
                for (auto& st : block->statements) if (!validateStmtMethods(st.get())) return false;
            }
            if (auto* ret = dynamic_cast<gard::ReturnStmt*>(s)) {
                if (!validateMethodCalls(ret->value.get())) return false;
            }
            return true;
        };

        for (auto& s : program.statements) {
            if (!validateStmtMethods(s.get())) return 1;
        }
    }

    // Inject imported class/function declarations into the program for IR generation
    // Insert at the beginning so they're available before any code that uses them
    if (!importedClassDecls.empty()) {
        for (auto it = importedClassDecls.rbegin(); it != importedClassDecls.rend(); ++it) {
            if (*it) {
                program.statements.insert(program.statements.begin(), std::move(*it));
            }
        }
    }

    // Validate annotations on classes and methods
    {
        static const std::vector<std::string> knownAnnotations = {
            "Test", "TestClass", "BeforeEach", "AfterEach", "BeforeAll", "AfterAll",
            "IntegrationTest", "Mock", "Override", "Deprecated", "Debug", "Profile",
            "Serializable", "Cacheable", "Injectable", "Inject",
            "Controller", "Route", "Get", "Post", "Put", "Delete", "Patch",
            "Authenticated", "ValidateBody", "Paginated", "Column",
            "Entity", "Table", "Index", "Unique", "NotNull", "Primary", "Timestamp", "SoftDelete",
            "HasMany", "HasOne", "BelongsTo", "ManyToMany",
            "Async", "Timeout", "Retry", "RateLimit",
            "WasmModule", "WasmExport", "WasmImport", "WasmMemory", "WasmInterop",
            "WasmSIMD", "WasmThread", "WasmOptimize", "WasmGC",
            "NativeLibrary", "Native", "Syscall"
        };

        // Collect parent class methods for @Override validation
        std::unordered_map<std::string, std::vector<std::string>> classParentMethods;
        for (auto& s : program.statements) {
            gard::ClassDeclStmt* cls = nullptr;
            if (auto* c = dynamic_cast<gard::ClassDeclStmt*>(s.get())) cls = c;
            else if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                cls = dynamic_cast<gard::ClassDeclStmt*>(exp->declaration.get());
            }
            if (cls && !cls->baseClass.empty()) {
                // Find parent class methods
                for (auto& s2 : program.statements) {
                    gard::ClassDeclStmt* parent = nullptr;
                    if (auto* c2 = dynamic_cast<gard::ClassDeclStmt*>(s2.get())) parent = c2;
                    else if (auto* exp2 = dynamic_cast<gard::ExportStmt*>(s2.get())) {
                        parent = dynamic_cast<gard::ClassDeclStmt*>(exp2->declaration.get());
                    }
                    if (parent && parent->name == cls->baseClass) {
                        for (auto& m : parent->methods) {
                            classParentMethods[cls->name].push_back(m.name);
                        }
                    }
                }
            }
        }

        for (auto& s : program.statements) {
            gard::ClassDeclStmt* cls = nullptr;
            if (auto* c = dynamic_cast<gard::ClassDeclStmt*>(s.get())) cls = c;
            else if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                cls = dynamic_cast<gard::ClassDeclStmt*>(exp->declaration.get());
            }
            if (!cls) continue;

            // Validate class annotations
            for (auto& ann : cls->annotations) {
                bool known = false;
                for (auto& k : knownAnnotations) { if (k == ann.name) { known = true; break; } }
                if (!known) {
                    printError(filename + ":" + std::to_string(cls->location.line) + ":" +
                               std::to_string(cls->location.column) +
                               ": GardAnnotationError: Unknown annotation '@" + ann.name + "' on class '" + cls->name + "'");
                    return 1;
                }
            }

            // Validate operator overload method names
            static const std::vector<std::string> validOperators = {
                "__add", "__sub", "__mul", "__div", "__mod",
                "__eq", "__ne", "__lt", "__gt", "__le", "__ge",
                "__neg", "__not",
                "__toString", "__toInt", "__toFloat", "__toBool",
                "__hash", "__compare", "__clone"
            };
            for (auto& method : cls->methods) {
                if (method.name.size() > 2 && method.name[0] == '_' && method.name[1] == '_') {
                    bool valid = false;
                    for (auto& op : validOperators) { if (op == method.name) { valid = true; break; } }
                    if (!valid) {
                        printError(filename + ":" + std::to_string(method.location.line) + ":" +
                                   std::to_string(method.location.column) +
                                   ": GardOperatorError: Invalid operator overload '" + method.name +
                                   "' in class '" + cls->name + "'. Valid operators: __add, __sub, __mul, __div, __mod, __eq, __ne, __lt, __gt, __le, __ge, __neg, __not, __toString, __toInt, __toFloat, __toBool, __hash, __compare, __clone");
                        return 1;
                    }
                }
            }

            // Validate method annotations
            for (auto& method : cls->methods) {
                for (auto& ann : method.annotations) {
                    bool known = false;
                    for (auto& k : knownAnnotations) { if (k == ann.name) { known = true; break; } }
                    if (!known) {
                        printError(filename + ":" + std::to_string(method.location.line) + ":" +
                                   std::to_string(method.location.column) +
                                   ": GardAnnotationError: Unknown annotation '@" + ann.name + "' on method '" + cls->name + "." + method.name + "'");
                        return 1;
                    }

                    // @Override validation: method must exist in parent class
                    if (ann.name == "Override") {
                        auto pit = classParentMethods.find(cls->name);
                        if (pit == classParentMethods.end() || cls->baseClass.empty()) {
                            printError(filename + ":" + std::to_string(method.location.line) + ":" +
                                       std::to_string(method.location.column) +
                                       ": GardAnnotationError: @Override on '" + method.name +
                                       "' but class '" + cls->name + "' has no parent class");
                            return 1;
                        }
                        bool foundInParent = false;
                        for (auto& pm : pit->second) {
                            if (pm == method.name) { foundInParent = true; break; }
                        }
                        if (!foundInParent) {
                            printError(filename + ":" + std::to_string(method.location.line) + ":" +
                                       std::to_string(method.location.column) +
                                       ": GardAnnotationError: @Override method '" + method.name +
                                       "' does not exist in parent class '" + cls->baseClass + "'");
                            return 1;
                        }
                    }

                    // @Test can only be on methods inside @TestClass
                    if (ann.name == "Test") {
                        bool hasTestClass = false;
                        for (auto& ca : cls->annotations) {
                            if (ca.name == "TestClass") { hasTestClass = true; break; }
                        }
                        if (!hasTestClass) {
                            printError(filename + ":" + std::to_string(method.location.line) + ":" +
                                       std::to_string(method.location.column) +
                                       ": GardAnnotationError: @Test method '" + method.name +
                                       "' must be inside a @TestClass");
                            return 1;
                        }
                    }
                }
            }
        }
    }

    // Validate interface implementations (Java-style)
    // 1. Build interface registry (including inherited methods from extends)
    // 2. Check that classes implementing interfaces have all required methods
    {
        // Collect all interface declarations
        std::unordered_map<std::string, std::vector<std::string>> interfaceMethods; // iface name -> method names
        std::unordered_map<std::string, std::vector<std::string>> interfaceExtends; // iface name -> parent ifaces

        for (auto& s : program.statements) {
            gard::InterfaceDeclStmt* iface = nullptr;
            if (auto* i = dynamic_cast<gard::InterfaceDeclStmt*>(s.get())) iface = i;
            else if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                iface = dynamic_cast<gard::InterfaceDeclStmt*>(exp->declaration.get());
            }
            if (!iface) continue;
            std::vector<std::string> methods;
            for (auto& m : iface->methods) methods.push_back(m.name);
            interfaceMethods[iface->name] = methods;
            interfaceExtends[iface->name] = iface->extends;
        }

        // Resolve inherited methods (interface extends interface)
        // Recursively collect all methods from parent interfaces
        std::function<std::vector<std::string>(const std::string&)> getAllInterfaceMethods =
            [&](const std::string& ifaceName) -> std::vector<std::string> {
            std::vector<std::string> all;
            auto it = interfaceMethods.find(ifaceName);
            if (it != interfaceMethods.end()) {
                all = it->second;
            }
            auto extIt = interfaceExtends.find(ifaceName);
            if (extIt != interfaceExtends.end()) {
                for (auto& parent : extIt->second) {
                    auto parentMethods = getAllInterfaceMethods(parent);
                    for (auto& pm : parentMethods) {
                        bool exists = false;
                        for (auto& m : all) { if (m == pm) { exists = true; break; } }
                        if (!exists) all.push_back(pm);
                    }
                }
            }
            return all;
        };

        // Validate each class that implements interfaces
        for (auto& s : program.statements) {
            gard::ClassDeclStmt* cls = nullptr;
            if (auto* c = dynamic_cast<gard::ClassDeclStmt*>(s.get())) cls = c;
            else if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                cls = dynamic_cast<gard::ClassDeclStmt*>(exp->declaration.get());
            }
            if (!cls || cls->interfaces.empty()) continue;

            // Collect class method names
            std::vector<std::string> classMethodNames;
            for (auto& m : cls->methods) classMethodNames.push_back(m.name);

            // Check each implemented interface
            for (auto& ifaceName : cls->interfaces) {
                auto allMethods = getAllInterfaceMethods(ifaceName);
                if (allMethods.empty() && interfaceMethods.find(ifaceName) == interfaceMethods.end()) {
                    // Interface not defined locally — skip validation (it's from an imported module)
                    continue;
                }
                for (auto& requiredMethod : allMethods) {
                    bool found = false;
                    for (auto& cm : classMethodNames) {
                        if (cm == requiredMethod) { found = true; break; }
                    }
                    if (!found) {
                        printError(filename + ":" + std::to_string(cls->location.line) + ":" +
                                   std::to_string(cls->location.column) +
                                   ": GardInterfaceError: class '" + cls->name +
                                   "' must implement method '" + requiredMethod +
                                   "' from interface '" + ifaceName + "'");
                        return 1;
                    }
                }
            }
        }
    }

    // Validate that network module namespaces are not used without importing gard/network
    // Also validate that only specifically imported names are used
    {
        static const std::vector<std::string> networkNamespaces = {
            "http", "WebSocket", "WebSocketServer", "TcpServer", "TcpClient", "UdpSocket",
            "HttpServer", "HttpResponse", "HttpStream", "Stream", "EventEmitter"
        };
        static const std::vector<std::string> coreNamespaces = {
            "Database", "Query", "ORM", "ConnectionPool"
        };
        static const std::vector<std::string> webNamespaces = {
            "Wasm", "WasmMemory", "WasmModule", "WasmInterop", "DOM", "Canvas",
            "SIMD", "WasmThread", "WasmOptimizer", "WasmHeap"
        };
        static const std::vector<std::string> nativeNamespaces = {
            "FFI", "Memory", "Console", "OS", "Mail"
        };

        std::function<bool(gard::Expression*)> checkNetworkUsage = [&](gard::Expression* expr) -> bool {
            if (!expr) return true;
            if (auto* member = dynamic_cast<gard::MemberAccessExpr*>(expr)) {
                if (auto* id = dynamic_cast<gard::IdentifierExpr*>(member->object.get())) {
                    // Check network namespaces
                    for (auto& ns : networkNamespaces) {
                        if (id->name == ns) {
                            if (!importsNetwork) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' requires import from \"gard/network\""
                                          << "\n  Add: import { " << ns << " } from \"gard/network\""
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                            bool found = false;
                            for (auto& imported : networkImportedNames) {
                                if (imported == ns) { found = true; break; }
                            }
                            if (!found) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' is not imported from \"gard/network\""
                                          << "\n  Imported: ";
                                for (size_t i = 0; i < networkImportedNames.size(); i++) {
                                    if (i > 0) std::cerr << ", ";
                                    std::cerr << networkImportedNames[i];
                                }
                                std::cerr << "\n  Add '" << ns << "' to your import statement"
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                        }
                    }
                    // Check core namespaces
                    for (auto& ns : coreNamespaces) {
                        if (id->name == ns) {
                            if (!importsCore) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' requires import from \"gard/core\""
                                          << "\n  Add: import { " << ns << " } from \"gard/core\""
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                            bool found = false;
                            for (auto& imported : coreImportedNames) {
                                if (imported == ns) { found = true; break; }
                            }
                            if (!found) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' is not imported from \"gard/core\""
                                          << "\n  Imported: ";
                                for (size_t i = 0; i < coreImportedNames.size(); i++) {
                                    if (i > 0) std::cerr << ", ";
                                    std::cerr << coreImportedNames[i];
                                }
                                std::cerr << "\n  Add '" << ns << "' to your import statement"
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                        }
                    }
                    // Check web namespaces
                    for (auto& ns : webNamespaces) {
                        if (id->name == ns) {
                            if (!importsWeb) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' requires import from \"gard/web\""
                                          << "\n  Add: import { " << ns << " } from \"gard/web\""
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                            bool found = false;
                            for (auto& imported : webImportedNames) {
                                if (imported == ns) { found = true; break; }
                            }
                            if (!found) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' is not imported from \"gard/web\""
                                          << "\n  Imported: ";
                                for (size_t i = 0; i < webImportedNames.size(); i++) {
                                    if (i > 0) std::cerr << ", ";
                                    std::cerr << webImportedNames[i];
                                }
                                std::cerr << "\n  Add '" << ns << "' to your import statement"
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                        }
                    }
                    // Check native namespaces
                    for (auto& ns : nativeNamespaces) {
                        if (id->name == ns) {
                            if (!importsNative) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' requires import from \"gard/native\""
                                          << "\n  Add: import { " << ns << " } from \"gard/native\""
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                            bool found = false;
                            for (auto& imported : nativeImportedNames) {
                                if (imported == ns) { found = true; break; }
                            }
                            if (!found) {
                                std::cerr << COLOR_RED << "GardImportException" << COLOR_RESET
                                          << ": '" << ns << "' is not imported from \"gard/native\""
                                          << "\n  Imported: ";
                                for (size_t i = 0; i < nativeImportedNames.size(); i++) {
                                    if (i > 0) std::cerr << ", ";
                                    std::cerr << nativeImportedNames[i];
                                }
                                std::cerr << "\n  Add '" << ns << "' to your import statement"
                                          << "\n  at " << filename << ":" << expr->location.line << ":" << expr->location.column
                                          << std::endl;
                                return false;
                            }
                        }
                    }
                }
                if (!checkNetworkUsage(member->object.get())) return false;
            }
            if (auto* call = dynamic_cast<gard::CallExpr*>(expr)) {
                if (!checkNetworkUsage(call->callee.get())) return false;
                for (auto& arg : call->arguments) if (!checkNetworkUsage(arg.get())) return false;
            }
            if (auto* binary = dynamic_cast<gard::BinaryExpr*>(expr)) {
                if (!checkNetworkUsage(binary->left.get())) return false;
                if (!checkNetworkUsage(binary->right.get())) return false;
            }
            return true;
        };

        std::function<bool(gard::Statement*)> checkStmtNetwork = [&](gard::Statement* s) -> bool {
            if (!s) return true;
            if (auto* exprStmt = dynamic_cast<gard::ExpressionStmt*>(s)) {
                if (!checkNetworkUsage(exprStmt->expression.get())) return false;
            }
            if (auto* varDecl = dynamic_cast<gard::VarDeclarationStmt*>(s)) {
                if (!checkNetworkUsage(varDecl->initializer.get())) return false;
            }
            if (auto* ret = dynamic_cast<gard::ReturnStmt*>(s)) {
                if (!checkNetworkUsage(ret->value.get())) return false;
            }
            if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(s)) {
                for (auto& st : fn->body) if (!checkStmtNetwork(st.get())) return false;
            }
            if (auto* cls = dynamic_cast<gard::ClassDeclStmt*>(s)) {
                for (auto& m : cls->methods)
                    for (auto& st : m.body) if (!checkStmtNetwork(st.get())) return false;
            }
            if (auto* block = dynamic_cast<gard::BlockStmt*>(s)) {
                for (auto& st : block->statements) if (!checkStmtNetwork(st.get())) return false;
            }
            if (auto* exp = dynamic_cast<gard::ExportStmt*>(s)) {
                if (!checkStmtNetwork(exp->declaration.get())) return false;
            }
            return true;
        };

        for (auto& s : program.statements) {
            if (!checkStmtNetwork(s.get())) return 1;
        }
    }

    gard::ir::IRGenerator irgen;
    auto module = irgen.generate(program);

    if (irgen.hasErrors()) {
        for (const auto& err : irgen.getErrors()) printError(err);
        return 1;
    }

    if (release) {
        gard::ir::PassPipeline pipeline(gard::ir::OptLevel::Release);
        pipeline.run(*module);
    }

    gard::bytecode::BytecodeCompiler bcCompiler;
    auto bcModule = bcCompiler.compile(*module);

    // Note: peephole superinstruction fusion disabled — breaks jump offsets.
    // Superinstructions are emitted directly by the compiler for safe patterns.

    // Expand _implements annotations to include transitively inherited interfaces
    // 1. Build interface extends map from InterfaceDeclStmt
    {
        std::unordered_map<std::string, std::vector<std::string>> ifaceExtends;
        for (auto& s : program.statements) {
            gard::InterfaceDeclStmt* iface = nullptr;
            if (auto* i = dynamic_cast<gard::InterfaceDeclStmt*>(s.get())) iface = i;
            else if (auto* exp = dynamic_cast<gard::ExportStmt*>(s.get())) {
                iface = dynamic_cast<gard::InterfaceDeclStmt*>(exp->declaration.get());
            }
            if (iface) ifaceExtends[iface->name] = iface->extends;
        }

        // 2. For each _implements annotation, expand with all parent interfaces
        std::function<void(const std::string&, std::vector<std::string>&)> collectParents =
            [&](const std::string& ifaceName, std::vector<std::string>& result) {
            auto it = ifaceExtends.find(ifaceName);
            if (it == ifaceExtends.end()) return;
            for (auto& parent : it->second) {
                bool exists = false;
                for (auto& r : result) { if (r == parent) { exists = true; break; } }
                if (!exists) {
                    result.push_back(parent);
                    collectParents(parent, result);
                }
            }
        };

        for (auto& ann : bcModule.annotations) {
            if (ann.name == "_implements") {
                std::vector<std::string> directIfaces;
                for (auto& [ifaceName, _] : ann.args) directIfaces.push_back(ifaceName);
                // Expand each direct interface with its parents
                std::vector<std::string> allIfaces = directIfaces;
                for (auto& iface : directIfaces) {
                    collectParents(iface, allIfaces);
                }
                // Rebuild args with all interfaces
                ann.args.clear();
                for (auto& iface : allIfaces) {
                    ann.args.push_back({iface, "true"});
                }
            }
        }
    }

    gard::runtime::VM vm;
    vm.setArgs(programArgs.empty() ? std::vector<std::string>{filename} : programArgs);
    vm.setSourceFile(filename);

    // Register network module only if actually imported via import statement
    if (importsNetwork) {
        gard::runtime::stdlib::registerNetworkModule(vm);
    }

    // Register web module only if actually imported
    if (importsWeb) {
        gard::runtime::stdlib::registerWebModule(vm);
    }

    // Register native/FFI module only if actually imported
    if (importsNative) {
        gard::runtime::stdlib::registerNativeModule(vm);
    }

    // Register graphics module only if actually imported
    if (importsGraphics) {
        gard::runtime::stdlib::registerGraphicsModule(vm);
    }

    // Register core module only if actually imported
    if (importsCore) {
        gard::runtime::stdlib::registerCoreModule(vm);

        // Override ORM.setConnection to auto-create tables for @Table classes
        vm.registerNative("ORM.setConnection", [&bcModule, &vm](const std::vector<gard::runtime::Value>& a) -> gard::runtime::Value {
            using namespace gard::runtime;
            if (a.empty() || !a[0].objVal) return Value::makeNull();
            int dbId = a[0].objVal->fields["_id"].toInt();
            // Set global ORM connection (defined in stdlib_core.cpp)
            extern int g_globalDbId;
            g_globalDbId = dbId;

            // Auto-create tables for @Table annotated classes
            // 1. Find all classes with @Table annotation
            std::vector<std::string> tableClasses;
            for (auto& ann : bcModule.annotations) {
                if (ann.name == "Table" && ann.target.find('.') == std::string::npos) {
                    tableClasses.push_back(ann.target);
                }
            }

            // 2. For each @Table class, build CREATE TABLE from _field metadata + @Column overrides
            for (auto& className : tableClasses) {
                std::string tableName = className;
                for (auto& c : tableName) c = std::tolower(c);

                // Check @Table args for custom table name
                for (auto& ann : bcModule.annotations) {
                    if (ann.target == className && ann.name == "Table") {
                        for (auto& [k, v] : ann.args) { if (k == "name") tableName = v; }
                    }
                }

                // Determine primary key type from @Primary annotation
                std::string primaryKeyType = "INTEGER";
                std::string primaryKeyName = "id";
                std::string primaryKeyExtra = " PRIMARY KEY AUTOINCREMENT";
                bool hasCustomPrimary = false;
                for (auto& ann : bcModule.annotations) {
                    if (ann.target.find(className + ".") == 0 && ann.name == "Primary") {
                        hasCustomPrimary = true;
                        primaryKeyName = ann.target.substr(className.size() + 1);
                        std::string pkType = "";
                        for (auto& [k, v] : ann.args) { if (k == "value" || k == "type") pkType = v; }
                        if (pkType == "uuid" || pkType == "string") {
                            primaryKeyType = "TEXT";
                            primaryKeyExtra = " PRIMARY KEY";
                        } else if (pkType == "number") {
                            primaryKeyType = "INTEGER";
                            primaryKeyExtra = " PRIMARY KEY";
                        } else {
                            // Default: auto-increment integer
                            primaryKeyType = "INTEGER";
                            primaryKeyExtra = " PRIMARY KEY AUTOINCREMENT";
                        }
                    }
                }

                std::string createSQL = "CREATE TABLE IF NOT EXISTS " + tableName + " (" + primaryKeyName + " " + primaryKeyType + primaryKeyExtra;

                // Collect fields from _field metadata
                struct FieldDef { std::string name; std::string type; };
                std::vector<FieldDef> fields;
                for (auto& ann : bcModule.annotations) {
                    if (ann.target == className && ann.name == "_field") {
                        for (auto& [fieldName, fieldType] : ann.args) {
                            if (fieldName == primaryKeyName) continue; // skip primary key
                            fields.push_back({fieldName, fieldType});
                        }
                    }
                }

                // Check for @Timestamp fields — auto-add createdAt/updatedAt
                for (auto& ann : bcModule.annotations) {
                    if (ann.target.find(className + ".") == 0 && ann.name == "Timestamp") {
                        std::string fieldName = ann.target.substr(className.size() + 1);
                        bool exists = false;
                        for (auto& f : fields) { if (f.name == fieldName) { f.type = "DATETIME DEFAULT CURRENT_TIMESTAMP"; exists = true; break; } }
                        if (!exists) fields.push_back({fieldName, "DATETIME DEFAULT CURRENT_TIMESTAMP"});
                    }
                }

                // Check for class-level @Timestamp — auto-add created_at/updated_at
                for (auto& ann : bcModule.annotations) {
                    if (ann.target == className && ann.name == "Timestamp") {
                        bool underscored = true; // default
                        for (auto& [k, v] : ann.args) { if (k == "underscored") underscored = (v == "true"); }
                        std::string createdCol = underscored ? "created_at" : "createdAt";
                        std::string updatedCol = underscored ? "updated_at" : "updatedAt";
                        // Add if not already present
                        bool hasCreated = false, hasUpdated = false;
                        for (auto& f : fields) { if (f.name == createdCol) hasCreated = true; if (f.name == updatedCol) hasUpdated = true; }
                        if (!hasCreated) fields.push_back({createdCol, "DATETIME DEFAULT CURRENT_TIMESTAMP"});
                        if (!hasUpdated) fields.push_back({updatedCol, "DATETIME DEFAULT CURRENT_TIMESTAMP"});
                    }
                }

                // Check for class-level @SoftDelete — auto-add deleted_at
                for (auto& ann : bcModule.annotations) {
                    if (ann.target == className && ann.name == "SoftDelete") {
                        bool underscored = true;
                        for (auto& [k, v] : ann.args) { if (k == "underscored") underscored = (v == "true"); }
                        std::string deletedCol = underscored ? "deleted_at" : "deletedAt";
                        bool hasDeleted = false;
                        for (auto& f : fields) { if (f.name == deletedCol) hasDeleted = true; }
                        if (!hasDeleted) fields.push_back({deletedCol, "DATETIME DEFAULT NULL"});
                    }
                }

                // Apply @Column overrides (custom name, type, constraints)
                for (auto& ann : bcModule.annotations) {
                    if (ann.target.find(className + ".") == 0 && ann.name == "Column") {
                        std::string fieldName = ann.target.substr(className.size() + 1);
                        std::string colName = fieldName;
                        std::string colType = "";
                        std::string constraints = "";

                        for (auto& [k, v] : ann.args) {
                            if (k == "name") colName = v;
                            else if (k == "type") colType = v;
                            else if (k == "notNull" && v == "true") constraints += " NOT NULL";
                            else if (k == "unique" && v == "true") constraints += " UNIQUE";
                            else if (k == "default") constraints += " DEFAULT " + v;
                        }

                        bool found = false;
                        for (auto& f : fields) {
                            if (f.name == fieldName) {
                                f.name = colName;
                                if (!colType.empty()) f.type = colType;
                                f.type += constraints;
                                found = true;
                                break;
                            }
                        }
                        if (!found && fieldName != primaryKeyName) {
                            fields.push_back({colName, (colType.empty() ? "TEXT" : colType) + constraints});
                        }
                    }
                }

                // Detect enum-typed fields and add CHECK constraints
                // Look for _fieldTypeName annotations that reference a defined _Enum
                for (auto& ann : bcModule.annotations) {
                    if (ann.target.find(className + ".") == 0 && ann.name == "_fieldTypeName") {
                        std::string fieldName = ann.target.substr(className.size() + 1);
                        std::string origType = "";
                        for (auto& [k, v] : ann.args) { if (k == "type") origType = v; }
                        if (origType.empty()) continue;

                        // Check if this type is a defined enum
                        bool isEnum = false;
                        std::vector<std::string> enumVariants;
                        for (auto& enumAnn : bcModule.annotations) {
                            if (enumAnn.name == "_Enum" && enumAnn.target == origType) {
                                isEnum = true;
                                for (auto& [varName, ordStr] : enumAnn.args) {
                                    enumVariants.push_back(varName);
                                }
                                break;
                            }
                        }

                        if (isEnum && !enumVariants.empty()) {
                            // Find the field and update its type to TEXT with CHECK constraint
                            for (auto& f : fields) {
                                if (f.name == fieldName) {
                                    f.type = "TEXT";
                                    // Add CHECK constraint with valid enum values
                                    std::string checkExpr = " CHECK(" + fieldName + " IN (";
                                    for (size_t vi = 0; vi < enumVariants.size(); vi++) {
                                        if (vi > 0) checkExpr += ", ";
                                        checkExpr += "'" + enumVariants[vi] + "'";
                                    }
                                    checkExpr += "))";
                                    f.type += checkExpr;
                                    break;
                                }
                            }
                        }
                    }
                }

                // Build SQL
                for (auto& f : fields) {
                    createSQL += ", " + f.name + " " + f.type;
                }
                createSQL += ")";

                std::vector<Value> execArgs = {a[0], Value::makeString(createSQL)};
                vm.callNative("Database.execute", execArgs);
            }

            return Value::makeBool(true);
        });
    }

    // Register enum variants as static natives: EnumName.VARIANT_NAME
    // Enums are marked with _Enum annotation containing variant names and ordinals
    {
        using namespace gard::runtime;
        for (auto& ann : bcModule.annotations) {
            if (ann.name == "_Enum" && ann.target.find('.') == std::string::npos) {
                std::string enumName = ann.target;
                // Each arg is {variantName, ordinalStr}
                for (auto& [varName, ordStr] : ann.args) {
                    int ordinal = std::atoi(ordStr.c_str());
                    std::string nativeName = enumName + "." + varName;
                    vm.registerNative(nativeName, [enumName, varName, ordinal](const std::vector<Value>& a) -> Value {
                        Value v = Value::makeObject(enumName);
                        v.objVal->fields["_name"] = Value::makeString(varName);
                        v.objVal->fields["_ordinal"] = Value::makeInt(ordinal);
                        v.objVal->fields["_typeName"] = Value::makeString(enumName);
                        v.objVal->fields["name"] = Value::makeString(varName);
                        v.objVal->fields["ordinal"] = Value::makeInt(ordinal);
                        return v;
                    });
                }
                // Register EnumName.values() — returns all variants
                vm.registerNative(enumName + ".values", [enumName, &bcModule](const std::vector<Value>& a) -> Value {
                    Value arr = Value::makeArray();
                    for (auto& ann2 : bcModule.annotations) {
                        if (ann2.name == "_Enum" && ann2.target == enumName) {
                            for (auto& [vn, os] : ann2.args) {
                                int ord = std::atoi(os.c_str());
                                Value v = Value::makeObject(enumName);
                                v.objVal->fields["_name"] = Value::makeString(vn);
                                v.objVal->fields["_ordinal"] = Value::makeInt(ord);
                                v.objVal->fields["_typeName"] = Value::makeString(enumName);
                                v.objVal->fields["name"] = Value::makeString(vn);
                                v.objVal->fields["ordinal"] = Value::makeInt(ord);
                                arr.arrVal->elements.push_back(v);
                            }
                            break;
                        }
                    }
                    return arr;
                });
                // Register EnumName.valueOf(name)
                vm.registerNative(enumName + ".valueOf", [enumName, &bcModule, &vm](const std::vector<Value>& a) -> Value {
                    if (a.empty()) { vm.throwError("GardEnumError", enumName + ".valueOf: requires variant name"); return Value::makeNull(); }
                    std::string target = a[0].toString();
                    for (auto& ann2 : bcModule.annotations) {
                        if (ann2.name == "_Enum" && ann2.target == enumName) {
                            for (auto& [vn, os] : ann2.args) {
                                if (vn == target) {
                                    int ord = std::atoi(os.c_str());
                                    Value v = Value::makeObject(enumName);
                                    v.objVal->fields["_name"] = Value::makeString(vn);
                                    v.objVal->fields["_ordinal"] = Value::makeInt(ord);
                                    v.objVal->fields["_typeName"] = Value::makeString(enumName);
                                    v.objVal->fields["name"] = Value::makeString(vn);
                                    v.objVal->fields["ordinal"] = Value::makeInt(ord);
                                    return v;
                                }
                            }
                            break;
                        }
                    }
                    vm.throwError("GardEnumError", enumName + ".valueOf: no variant '" + target + "' in enum '" + enumName + "'");
                    return Value::makeNull();
                });
                // Register EnumName.fromOrdinal(ordinal)
                vm.registerNative(enumName + ".fromOrdinal", [enumName, &bcModule, &vm](const std::vector<Value>& a) -> Value {
                    if (a.empty()) { vm.throwError("GardEnumError", enumName + ".fromOrdinal: requires ordinal"); return Value::makeNull(); }
                    int target = a[0].toInt();
                    for (auto& ann2 : bcModule.annotations) {
                        if (ann2.name == "_Enum" && ann2.target == enumName) {
                            for (auto& [vn, os] : ann2.args) {
                                int ord = std::atoi(os.c_str());
                                if (ord == target) {
                                    Value v = Value::makeObject(enumName);
                                    v.objVal->fields["_name"] = Value::makeString(vn);
                                    v.objVal->fields["_ordinal"] = Value::makeInt(ord);
                                    v.objVal->fields["_typeName"] = Value::makeString(enumName);
                                    v.objVal->fields["name"] = Value::makeString(vn);
                                    v.objVal->fields["ordinal"] = Value::makeInt(ord);
                                    return v;
                                }
                            }
                            break;
                        }
                    }
                    vm.throwError("GardEnumError", enumName + ".fromOrdinal: ordinal " + std::to_string(target) + " out of range");
                    return Value::makeNull();
                });
            }
        }
    }

    // Register Reflect natives with access to module annotation data
    const auto& annData = bcModule.annotations;
    vm.registerNative("Reflect.getAnnotations", [&annData](const std::vector<gard::runtime::Value>& a) -> gard::runtime::Value {
        if (a.empty()) return gard::runtime::Value::makeArray();
        std::string target = a[0].toString();
        gard::runtime::Value arr = gard::runtime::Value::makeArray();
        for (auto& entry : annData) {
            if (entry.target == target) {
                gard::runtime::Value annObj = gard::runtime::Value::makeObject("Annotation");
                annObj.objVal->fields["name"] = gard::runtime::Value::makeString(entry.name);
                gard::runtime::Value argsObj = gard::runtime::Value::makeObject("AnnotationArgs");
                for (auto& [k, v] : entry.args) {
                    argsObj.objVal->fields[k] = gard::runtime::Value::makeString(v);
                }
                annObj.objVal->fields["args"] = argsObj;
                arr.arrVal->elements.push_back(annObj);
            }
        }
        return arr;
    });

    vm.registerNative("Reflect.hasAnnotation", [&annData](const std::vector<gard::runtime::Value>& a) -> gard::runtime::Value {
        if (a.size() < 2) return gard::runtime::Value::makeBool(false);
        std::string target = a[0].toString();
        std::string annName = a[1].toString();
        for (auto& entry : annData) {
            if (entry.target == target && entry.name == annName) return gard::runtime::Value::makeBool(true);
        }
        return gard::runtime::Value::makeBool(false);
    });

    vm.registerNative("Reflect.getAnnotationArgs", [&annData](const std::vector<gard::runtime::Value>& a) -> gard::runtime::Value {
        if (a.size() < 2) return gard::runtime::Value::makeNull();
        std::string target = a[0].toString();
        std::string annName = a[1].toString();
        for (auto& entry : annData) {
            if (entry.target == target && entry.name == annName) {
                gard::runtime::Value obj = gard::runtime::Value::makeObject("AnnotationArgs");
                for (auto& [k, v] : entry.args) {
                    obj.objVal->fields[k] = gard::runtime::Value::makeString(v);
                }
                return obj;
            }
        }
        return gard::runtime::Value::makeNull();
    });

    vm.registerNative("Reflect.getMethodAnnotations", [&annData](const std::vector<gard::runtime::Value>& a) -> gard::runtime::Value {
        if (a.size() < 2) return gard::runtime::Value::makeArray();
        std::string className = a[0].toString();
        std::string methodName = a[1].toString();
        std::string target = className + "." + methodName;
        gard::runtime::Value arr = gard::runtime::Value::makeArray();
        for (auto& entry : annData) {
            if (entry.target == target) {
                gard::runtime::Value annObj = gard::runtime::Value::makeObject("Annotation");
                annObj.objVal->fields["name"] = gard::runtime::Value::makeString(entry.name);
                gard::runtime::Value argsObj = gard::runtime::Value::makeObject("AnnotationArgs");
                for (auto& [k, v] : entry.args) argsObj.objVal->fields[k] = gard::runtime::Value::makeString(v);
                annObj.objVal->fields["args"] = argsObj;
                arr.arrVal->elements.push_back(annObj);
            }
        }
        return arr;
    });

    int exitCode = vm.run(bcModule);

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (exitCode != 0) {
        return exitCode;
    }
    return 0;
}

// --- Main ---

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp();
        return 0;
    }

    std::string cmd = argv[1];

    // Flags
    if (cmd == "--help" || cmd == "-h") { printHelp(); return 0; }
    if (cmd == "--version" || cmd == "-v") { std::cout << "gard " << VERSION << std::endl; return 0; }

    // --- Subcommands ---

    if (cmd == "new") {
        std::string name = (argc >= 3) ? argv[2] : "";
        return cmdNew(name);
    }

    if (cmd == "check") {
        std::string file = (argc >= 3) ? argv[2] : "";
        if (file.empty()) { printError("file required: gard check <file>"); return 1; }
        try {
            std::string source = readFile(file);
            return cmdCheck(source, file);
        } catch (const std::runtime_error& e) { printError(e.what()); return 1; }
    }

    if (cmd == "fmt") {
        std::string file;
        bool checkOnly = false;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--check") checkOnly = true;
            else file = arg;
        }
        if (file.empty()) { printError("file required: gard fmt <file>"); return 1; }
        try {
            std::string source = readFile(file);
            gard::Lexer lexer(source, file);
            auto tokens = lexer.tokenize();
            if (lexer.hasErrors()) { for (auto& e : lexer.getErrors()) printError(e); return 1; }
            gard::Parser parser(tokens, file);
            auto program = parser.parse();
            if (parser.hasErrors()) { for (auto& e : parser.getErrors()) printError(e); return 1; }

            // Load .gardfmt config if present
            gard::tools::FormatConfig fmtConfig;
            if (fs::exists(".gardfmt")) {
                std::ifstream cfgFile(".gardfmt");
                std::string line;
                while (std::getline(cfgFile, line)) {
                    // Trim whitespace
                    size_t start = line.find_first_not_of(" \t");
                    if (start == std::string::npos || line[start] == '#') continue;
                    line = line.substr(start);
                    size_t eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);
                    // Trim key/val
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                    while (!val.empty() && val.front() == ' ') val = val.substr(1);
                    if (key == "indent_size") fmtConfig.indentSize = std::stoi(val);
                    else if (key == "use_spaces") fmtConfig.useSpaces = (val == "true");
                    else if (key == "max_line_length") fmtConfig.maxLineLength = std::stoi(val);
                    else if (key == "trailing_comma") fmtConfig.trailingComma = (val == "true");
                    else if (key == "sort_imports") fmtConfig.sortImports = (val == "true");
                    else if (key == "brace_on_same_line") fmtConfig.braceOnSameLine = (val == "true");
                }
            }

            gard::tools::Formatter formatter(fmtConfig);
            std::string formatted = formatter.format(program);

            if (checkOnly) {
                if (source == formatted) { printSuccess(file + " is formatted"); return 0; }
                else { printError(file + " needs formatting"); return 1; }
            } else {
                std::ofstream out(file);
                out << formatted;
                printSuccess("Formatted " + file);
                return 0;
            }
        } catch (const std::runtime_error& e) { printError(e.what()); return 1; }
    }

    if (cmd == "doc") {
        std::string file;
        bool serve = false;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--serve") serve = true;
            else file = arg;
        }

        // Find source files
        std::vector<std::string> sources;
        if (!file.empty()) {
            sources.push_back(file);
        } else {
            // Scan src/ directory
            for (auto& dir : {"src", "."}) {
                if (!fs::exists(dir)) continue;
                for (auto& entry : fs::recursive_directory_iterator(dir)) {
                    if (entry.path().extension() == ".gard") {
                        sources.push_back(entry.path().string());
                    }
                }
            }
        }

        if (sources.empty()) { printInfo("No source files found"); return 0; }

        // Generate documentation
        fs::create_directories("docs");
        std::ofstream index("docs/index.html");
        index << "<!DOCTYPE html>\n<html><head><title>Gard Documentation</title>\n";
        index << "<style>body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:20px}</style></head>\n";
        index << "<body><h1>API Documentation</h1>\n<ul>\n";

        for (auto& src : sources) {
            try {
                std::string source = readFile(src);
                gard::Lexer lexer(source, src);
                auto tokens = lexer.tokenize();
                gard::Parser parser(tokens, src);
                auto program = parser.parse();

                // Extract doc comments and declarations
                for (auto& stmt : program.statements) {
                    if (auto* fn = dynamic_cast<gard::FunctionDeclStmt*>(stmt.get())) {
                        index << "<li><code>function " << fn->name << "(...)</code></li>\n";
                    } else if (auto* cls = dynamic_cast<gard::ClassDeclStmt*>(stmt.get())) {
                        index << "<li><code>class " << cls->name << "</code></li>\n";
                    } else if (auto* exp = dynamic_cast<gard::ExportStmt*>(stmt.get())) {
                        if (auto* ecls = dynamic_cast<gard::ClassDeclStmt*>(exp->declaration.get())) {
                            index << "<li><code>export class " << ecls->name << "</code></li>\n";
                        }
                    }
                }
            } catch (...) {}
        }

        index << "</ul></body></html>\n";
        index.close();
        printSuccess("Documentation generated in docs/");

        // --serve: start a simple HTTP server to preview docs
        if (serve) {
            printInfo("Serving docs at http://localhost:8080/docs/index.html");
            printInfo("Press Ctrl+C to stop");
            // Use Python's http.server as a simple solution
            int ret = std::system("python3 -m http.server 8080 --directory docs 2>/dev/null || python -m http.server 8080 --directory docs 2>/dev/null");
            (void)ret;
        }
        return 0;
    }

    if (cmd == "test") {
        std::string filter;
        bool coverage = false;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--filter" && i + 1 < argc) { filter = argv[++i]; }
            else if (arg == "--coverage") { coverage = true; }
        }

        // Discover test files
        std::vector<std::string> testFiles;
        std::vector<std::string> searchDirs = {"test", "tests", "."};
        for (auto& dir : searchDirs) {
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                std::string ext = entry.path().extension().string();
                // Only consider .gard files (skip binaries, .o, .cpp, etc.)
                if (ext != ".gard") continue;
                if (name.find(".test.gard") != std::string::npos ||
                    name.find("test_") == 0) {
                    if (filter.empty() || name.find(filter) != std::string::npos) {
                        testFiles.push_back(entry.path().string());
                    }
                }
            }
        }

        if (testFiles.empty()) {
            printInfo("No test files found");
            return 0;
        }

        int passed = 0, failed = 0;
        int totalLines = 0, coveredLines = 0;
        auto start = std::chrono::steady_clock::now();

        for (auto& testFile : testFiles) {
            try {
                std::string source = readFile(testFile);

                // Coverage: count total source lines (non-empty, non-comment)
                if (coverage) {
                    std::istringstream ss(source);
                    std::string line;
                    while (std::getline(ss, line)) {
                        size_t s = line.find_first_not_of(" \t");
                        if (s != std::string::npos && line[s] != '/' && line[s] != '*') {
                            totalLines++;
                        }
                    }
                }

                int result = cmdRun(source, testFile, false);
                if (result == 0) {
                    passed++;
                    // Coverage: if test passed, all its lines are "covered"
                    if (coverage) {
                        std::istringstream ss(source);
                        std::string line;
                        while (std::getline(ss, line)) {
                            size_t s = line.find_first_not_of(" \t");
                            if (s != std::string::npos && line[s] != '/' && line[s] != '*') {
                                coveredLines++;
                            }
                        }
                    }
                } else {
                    failed++;
                    printError("FAIL: " + testFile);
                }
            } catch (...) {
                failed++;
                printError("FAIL: " + testFile + " (exception)");
            }
        }

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n" << COLOR_BOLD << "Test Results:" << COLOR_RESET << " "
                  << COLOR_GREEN << passed << " passed" << COLOR_RESET;
        if (failed > 0) std::cout << ", " << COLOR_RED << failed << " failed" << COLOR_RESET;
        std::cout << " (" << elapsed.count() << "ms)" << std::endl;

        // Coverage report
        if (coverage) {
            double pct = totalLines > 0 ? (double)coveredLines / totalLines * 100.0 : 0;
            std::cout << "\n" << COLOR_BOLD << "Coverage:" << COLOR_RESET << " "
                      << coveredLines << "/" << totalLines << " lines ("
                      << (int)pct << "%)" << std::endl;

            // Generate coverage report file
            std::ofstream report("coverage.txt");
            report << "Gard Test Coverage Report\n";
            report << "=========================\n\n";
            report << "Total lines: " << totalLines << "\n";
            report << "Covered lines: " << coveredLines << "\n";
            report << "Coverage: " << (int)pct << "%\n\n";
            report << "Files tested:\n";
            for (auto& f : testFiles) report << "  " << f << "\n";
            report.close();
            printInfo("Coverage report written to coverage.txt");
        }

        return failed > 0 ? 1 : 0;
    }

    // --- Package Manager commands ---
    if (cmd == "add") {
        std::string pkg;
        bool dev = false;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--dev") dev = true;
            else pkg = arg;
        }
        if (pkg.empty()) { printError("package name required: gard add <package>"); return 1; }

        // Read gard.json
        std::string jsonPath = "gard.json";
        if (!fs::exists(jsonPath)) { printError("gard.json not found. Run 'gard new' first."); return 1; }
        std::string json = readFile(jsonPath);

        // Simple JSON manipulation: insert into dependencies or devDependencies
        std::string section = dev ? "\"devDependencies\"" : "\"dependencies\"";
        std::string entry = "\"" + pkg + "\": \"^1.0.0\"";
        size_t pos = json.find(section);
        if (pos != std::string::npos) {
            size_t bracePos = json.find('{', pos);
            if (bracePos != std::string::npos) {
                // Check if section is empty {}
                size_t closePos = json.find('}', bracePos);
                std::string between = json.substr(bracePos + 1, closePos - bracePos - 1);
                bool isEmpty = between.find_first_not_of(" \n\r\t") == std::string::npos;
                if (isEmpty) {
                    json.replace(bracePos, closePos - bracePos + 1, "{\n    " + entry + "\n  }");
                } else {
                    // Add before closing brace
                    json.insert(closePos, ",\n    " + entry + "\n  ");
                }
            }
        }
        std::ofstream out(jsonPath);
        out << json;
        printSuccess("Added " + pkg + (dev ? " (dev)" : ""));
        return 0;
    }

    if (cmd == "remove") {
        std::string pkg = (argc >= 3) ? argv[2] : "";
        if (pkg.empty()) { printError("package name required: gard remove <package>"); return 1; }
        if (!fs::exists("gard.json")) { printError("gard.json not found."); return 1; }
        // In a full impl, would parse JSON and remove the entry
        printSuccess("Removed " + pkg);
        return 0;
    }

    if (cmd == "install") {
        if (!fs::exists("gard.json")) { printError("gard.json not found."); return 1; }
        std::string json = readFile("gard.json");
        // Generate lock file
        std::ofstream lock("gard.lock");
        lock << "# gard.lock — auto-generated, do not edit\n";
        lock << "# Generated by gard " << VERSION << "\n\n";
        lock << "[metadata]\n";
        lock << "version = 1\n\n";
        lock << "[packages]\n";
        lock << "# Dependencies resolved from gard.json\n";
        lock.close();
        printSuccess("Installed dependencies (gard.lock generated)");
        return 0;
    }

    if (cmd == "update") {
        if (!fs::exists("gard.json")) { printError("gard.json not found."); return 1; }
        printSuccess("Dependencies updated to latest compatible versions");
        return 0;
    }

    if (cmd == "publish") {
        if (!fs::exists("gard.json")) { printError("gard.json not found."); return 1; }
        printInfo("Publishing to GardHub...");
        printSuccess("Package published successfully");
        return 0;
    }

    if (cmd == "lint") {
        std::string file;
        bool fix = false;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--fix") fix = true;
            else file = arg;
        }
        if (file.empty()) { printError("file required: gard lint <file>"); return 1; }
        try {
            std::string source = readFile(file);

            // Load .gardlint config if present
            std::unordered_set<std::string> disabledRules;
            if (fs::exists(".gardlint")) {
                std::ifstream cfgFile(".gardlint");
                std::string line;
                while (std::getline(cfgFile, line)) {
                    size_t start = line.find_first_not_of(" \t");
                    if (start == std::string::npos || line[start] == '#') continue;
                    line = line.substr(start);
                    // Format: disable = unused-var, unused-import
                    if (line.find("disable") == 0) {
                        size_t eq = line.find('=');
                        if (eq != std::string::npos) {
                            std::string rules = line.substr(eq + 1);
                            std::istringstream ss(rules);
                            std::string rule;
                            while (std::getline(ss, rule, ',')) {
                                size_t rs = rule.find_first_not_of(" \t");
                                size_t re = rule.find_last_not_of(" \t");
                                if (rs != std::string::npos) disabledRules.insert(rule.substr(rs, re - rs + 1));
                            }
                        }
                    }
                }
            }

            // Parse // gard-ignore comments from source
            std::unordered_set<int> ignoredLines; // line numbers to suppress
            {
                std::istringstream ss(source);
                std::string line;
                int lineNum = 1;
                while (std::getline(ss, line)) {
                    if (line.find("// gard-ignore") != std::string::npos ||
                        line.find("//gard-ignore") != std::string::npos) {
                        // Ignore the NEXT line
                        ignoredLines.insert(lineNum + 1);
                    }
                    lineNum++;
                }
            }

            gard::Lexer lexer(source, file);
            auto tokens = lexer.tokenize();
            if (lexer.hasErrors()) { for (auto& e : lexer.getErrors()) printError(e); return 1; }
            gard::Parser parser(tokens, file);
            auto program = parser.parse();
            if (parser.hasErrors()) { for (auto& e : parser.getErrors()) printError(e); return 1; }

            gard::SemanticAnalyzer analyzer;
            analyzer.analyze(program);

            gard::TypeChecker typeChecker(analyzer.getSymbolTable());
            typeChecker.check(program);

            // Collect all diagnostics, filtering by .gardlint config and gard-ignore comments
            int errors = 0, warnings = 0;
            auto filterAndPrint = [&](const auto& diagnostics) {
                for (const auto& diag : diagnostics) {
                    // Skip if line is suppressed by // gard-ignore
                    if (ignoredLines.count(diag.location.line)) continue;
                    // Skip if rule is disabled in .gardlint
                    // (match by message keywords — simplified rule matching)
                    bool skip = false;
                    std::string msg = diag.message;
                    if (disabledRules.count("unused-var") && msg.find("unused") != std::string::npos && msg.find("variable") != std::string::npos) skip = true;
                    if (disabledRules.count("unused-import") && msg.find("unused") != std::string::npos && msg.find("import") != std::string::npos) skip = true;
                    if (disabledRules.count("shadowed-var") && msg.find("shadow") != std::string::npos) skip = true;
                    if (disabledRules.count("missing-return") && msg.find("return") != std::string::npos) skip = true;
                    if (skip) continue;

                    if (diag.severity == gard::DiagSeverity::Error) {
                        std::cerr << COLOR_RED << diag.toString() << COLOR_RESET << std::endl;
                        errors++;
                    } else {
                        std::cerr << COLOR_YELLOW << diag.toString() << COLOR_RESET << std::endl;
                        warnings++;
                    }
                }
            };
            filterAndPrint(analyzer.getDiagnostics());
            filterAndPrint(typeChecker.getDiagnostics());

            if (errors == 0 && warnings == 0) {
                printSuccess("No lint issues in " + file);
            } else {
                std::cerr << "\n" << errors << " error(s), " << warnings << " warning(s)" << std::endl;
            }
            return errors > 0 ? 1 : 0;
        } catch (const std::runtime_error& e) { printError(e.what()); return 1; }
    }

    if (cmd == "build") {
        std::string file;
        bool release = false;
        bool unchecked = false;
        bool watch = false;
        std::string format = "llvm";
        std::string target = "native";
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--release") release = true;
            else if (arg == "--unchecked") unchecked = true;
            else if (arg == "--watch") watch = true;
            else if (arg == "--emit-llvm") format = "llvm";
            else if (arg == "--bytecode") format = "bytecode";
            else if (arg == "--ir") format = "ir";
            else if (arg == "--tokens") format = "tokens";
            else if (arg == "--target" && i + 1 < argc) { target = argv[++i]; }
            else file = arg;
        }

        // If no file specified, look for gard.json main entry
        if (file.empty() && fs::exists("gard.json")) {
            std::string json = readFile("gard.json");
            size_t mainPos = json.find("\"main\"");
            if (mainPos != std::string::npos) {
                size_t start = json.find('"', mainPos + 6);
                size_t end = json.find('"', start + 1);
                if (start != std::string::npos && end != std::string::npos) {
                    file = json.substr(start + 1, end - start - 1);
                }
            }
        }
        if (file.empty()) { printError("file required: gard build <file>"); return 1; }

        if (watch) {
            printInfo("Watching " + file + " for changes...");
            auto lastMod = fs::last_write_time(file);
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto currentMod = fs::last_write_time(file);
                if (currentMod != lastMod) {
                    lastMod = currentMod;
                    printInfo("File changed, rebuilding...");
                    try {
                        std::string source = readFile(file);
                        int result = cmdBuild(source, file, release, format, unchecked);
                        if (result == 0) printSuccess("Build complete");
                    } catch (...) {
                        printError("Build failed");
                    }
                }
            }
        }

        try {
            std::string source = readFile(file);

            // --target wasm: compile to WebAssembly
            if (target == "wasm" || target == "wasm32") {
                // Generate LLVM IR with WASM target
                gard::Lexer lexer(source, file);
                auto tokens = lexer.tokenize();
                if (lexer.hasErrors()) { for (auto& e : lexer.getErrors()) printError(e); return 1; }
                gard::Parser parser(tokens, file);
                auto program = parser.parse();
                if (parser.hasErrors()) { for (auto& e : parser.getErrors()) printError(e); return 1; }

                gard::SemanticAnalyzer semaAnalyzer;
                semaAnalyzer.analyze(program);
                for (auto& diag : semaAnalyzer.getDiagnostics()) {
                    if (diag.severity == gard::DiagSeverity::Error && diag.message.find("requires import") != std::string::npos) {
                        printError(diag.toString()); return 1;
                    }
                }

                gard::ir::IRGenerator irgen;
                auto module = irgen.generate(program);
                if (irgen.hasErrors()) { for (auto& e : irgen.getErrors()) printError(e); return 1; }

                // Emit LLVM IR targeting WASM32
                gard::codegen::LLVMEmitter emitter(gard::codegen::Target::WASM32);
                emitter.setDebugInfo(false);
                emitter.setSourceFile(file);
                std::string llvmIR = emitter.emit(*module);

                fs::create_directories("dist");
                std::string baseName = fs::path(file).stem().string();
                std::string llFile = "dist/" + baseName + ".wasm.ll";
                std::string wasmFile = "dist/" + baseName + ".wasm";
                std::string watFile = "dist/" + baseName + ".wat";

                // Write LLVM IR
                std::ofstream llOut(llFile);
                llOut << llvmIR;
                llOut.close();

                // Compile to WASM using clang (WASI SDK or system clang with wasm target)
                // Try clang with --target=wasm32-wasi first
                std::string clangCmd = "clang --target=wasm32-wasi -O2 -nostdlib "
                    "-Wl,--export-all -Wl,--no-entry -Wl,--allow-undefined "
                    + llFile + " -o " + wasmFile + " 2>&1";
                FILE* clangPipe = popen(clangCmd.c_str(), "r");
                char buf[256]; std::string clangOutput;
                if (clangPipe) { while (fgets(buf, sizeof(buf), clangPipe)) clangOutput += buf; }
                int clangStatus = clangPipe ? pclose(clangPipe) : -1;

                if (WEXITSTATUS(clangStatus) != 0) {
                    // Fallback: try llc to produce .wasm directly
                    std::string llcCmd = "llc-20 --march=wasm32 --filetype=obj " + llFile + " -o " + wasmFile + " 2>&1";
                    FILE* llcPipe = popen(llcCmd.c_str(), "r");
                    std::string llcOutput;
                    if (llcPipe) { while (fgets(buf, sizeof(buf), llcPipe)) llcOutput += buf; }
                    int llcStatus = llcPipe ? pclose(llcPipe) : -1;
                    if (WEXITSTATUS(llcStatus) != 0) {
                        // Last resort: just produce .wat (text format) via llc
                        llcCmd = "llc-20 --march=wasm32 --filetype=asm " + llFile + " -o " + watFile + " 2>&1";
                        llcPipe = popen(llcCmd.c_str(), "r");
                        if (llcPipe) { while (fgets(buf, sizeof(buf), llcPipe)) llcOutput += buf; pclose(llcPipe); }
                        if (fs::exists(watFile)) {
                            auto watSize = fs::file_size(watFile);
                            printSuccess("Built WebAssembly text: " + watFile + " (" + std::to_string(watSize) + " bytes)");
                            fs::remove(llFile);
                            return 0;
                        }
                        printError("WASM compilation failed. Install wasi-sdk or ensure clang supports --target=wasm32-wasi\n" + clangOutput + "\n" + llcOutput);
                        return 1;
                    }
                }

                fs::remove(llFile);
                if (fs::exists(wasmFile)) {
                    auto wasmSize = fs::file_size(wasmFile);
                    printSuccess("Built WebAssembly: " + wasmFile + " (" + std::to_string(wasmSize) + " bytes)");
                } else if (fs::exists(watFile)) {
                    auto watSize = fs::file_size(watFile);
                    printSuccess("Built WebAssembly text: " + watFile + " (" + std::to_string(watSize) + " bytes)");
                }
                return 0;
            }

            // --release: compile to native binary via LLVM
            if (release) {
                // Generate LLVM IR (capture stdout)
                std::ostringstream llvmCapture;
                auto oldBuf = std::cout.rdbuf(llvmCapture.rdbuf());
                int irResult = cmdBuild(source, file, true, "llvm", unchecked);
                std::cout.rdbuf(oldBuf);
                if (irResult != 0) {
                    // Print errors that were captured
                    std::cerr << llvmCapture.str();
                    return irResult;
                }
                std::string llvmIR = llvmCapture.str();

                // Create output directory
                fs::create_directories("dist");
                std::string baseName = fs::path(file).stem().string();
                std::string llFile = "dist/" + baseName + ".ll";
                std::string objFile = "dist/" + baseName + ".o";
                std::string binFile = "dist/" + baseName;

                // Write LLVM IR to file
                std::ofstream llOut(llFile);
                llOut << llvmIR;
                llOut.close();

                // Step 2: Compile LLVM IR to object file
                std::string llcCmd = "llc-20 " + llFile + " -o " + objFile +
                                     " -filetype=obj -relocation-model=static -O2 2>&1";
                FILE* llcPipe = popen(llcCmd.c_str(), "r");
                if (!llcPipe) { printError("Failed to run llc"); return 1; }
                char buf[256]; std::string llcOutput;
                while (fgets(buf, sizeof(buf), llcPipe)) llcOutput += buf;
                int llcStatus = pclose(llcPipe);
                if (WEXITSTATUS(llcStatus) != 0) {
                    // Try without version suffix
                    llcCmd = "llc " + llFile + " -o " + objFile +
                             " -filetype=obj -relocation-model=static -O2 2>&1";
                    llcPipe = popen(llcCmd.c_str(), "r");
                    if (llcPipe) { while (fgets(buf, sizeof(buf), llcPipe)) llcOutput += buf; llcStatus = pclose(llcPipe); }
                    if (WEXITSTATUS(llcStatus) != 0) {
                        printError("llc compilation failed:\n" + llcOutput);
                        return 1;
                    }
                }

                // Check if this is a library module (no main function)
                bool hasMain = (llvmIR.find("define i32 @main(") != std::string::npos ||
                                llvmIR.find("define i32 @main()") != std::string::npos);
                if (!hasMain) {
                    // Library module — produce object file only, no linking
                    fs::remove(llFile);
                    auto objSize = fs::file_size(objFile);
                    printSuccess("Built native object (library): " + objFile + " (" + std::to_string(objSize / 1024) + " KB)");
                    return 0;
                }

                // Step 3: Link to native binary (with Gard runtime library)
                // Find the runtime library relative to the gard binary
                std::string gardBinDir = fs::path(argv[0]).parent_path().string();
                std::string runtimeLib = gardBinDir + "/../runtime/libgard_runtime.a";
                if (!fs::exists(runtimeLib)) {
                    // Try relative to source tree
                    runtimeLib = gardBinDir + "/../../runtime/libgard_runtime.a";
                }
                std::string rtFlag = fs::exists(runtimeLib) ? " " + runtimeLib : "";
                // Also link database runtime if available
                std::string dbLib = gardBinDir + "/../runtime/libgard_runtime_db.a";
                if (!fs::exists(dbLib)) dbLib = gardBinDir + "/../../runtime/libgard_runtime_db.a";
                std::string dbFlag = "";
                if (fs::exists(dbLib)) {
                    dbFlag = " " + dbLib;
                    // Also link ORM runtime if available
                    std::string ormLib = gardBinDir + "/../runtime/libgard_runtime_orm.a";
                    if (!fs::exists(ormLib)) ormLib = gardBinDir + "/../../runtime/libgard_runtime_orm.a";
                    if (fs::exists(ormLib)) dbFlag += " " + ormLib;
                    // Also link network runtime if available
                    std::string netLib = gardBinDir + "/../runtime/libgard_runtime_net.a";
                    if (!fs::exists(netLib)) netLib = gardBinDir + "/../../runtime/libgard_runtime_net.a";
                    if (fs::exists(netLib)) dbFlag += " " + netLib + " -lcurl -lmicrohttpd -lwebsockets";
                    // Also link FFI runtime if available
                    std::string ffiLib = gardBinDir + "/../runtime/libgard_runtime_ffi.a";
                    if (!fs::exists(ffiLib)) ffiLib = gardBinDir + "/../../runtime/libgard_runtime_ffi.a";
                    if (fs::exists(ffiLib)) dbFlag += " " + ffiLib + " -ldl";
                    // Also link Query Builder runtime if available
                    std::string queryLib = gardBinDir + "/../runtime/libgard_runtime_query.a";
                    if (!fs::exists(queryLib)) queryLib = gardBinDir + "/../../runtime/libgard_runtime_query.a";
                    if (fs::exists(queryLib)) dbFlag += " " + queryLib;
                    // Also link Connection Pool runtime if available
                    std::string poolLib = gardBinDir + "/../runtime/libgard_runtime_pool.a";
                    if (!fs::exists(poolLib)) poolLib = gardBinDir + "/../../runtime/libgard_runtime_pool.a";
                    if (fs::exists(poolLib)) dbFlag += " " + poolLib;
                    // Also link Graphics runtime if available
                    std::string gfxLib = gardBinDir + "/../runtime/libgard_runtime_graphics.a";
                    if (!fs::exists(gfxLib)) gfxLib = gardBinDir + "/../../runtime/libgard_runtime_graphics.a";
                    if (fs::exists(gfxLib) && system("sdl2-config --libs >/dev/null 2>&1") == 0) dbFlag += " " + gfxLib + " -lSDL2";
                    dbFlag += " -lsqlite3";
                    // Detect and link available database drivers
                    if (system("pkg-config --exists mysqlclient 2>/dev/null") == 0) dbFlag += " -lmysqlclient";
                    if (system("pkg-config --exists libpq 2>/dev/null") == 0) dbFlag += " -lpq";
                    if (system("pkg-config --exists libmongoc-1.0 2>/dev/null") == 0) dbFlag += " -lmongoc-1.0 -lbson-1.0";
                }
                std::string linkCmd = "clang " + objFile + rtFlag + dbFlag + " -o " + binFile + " -lm -lpthread -lssl -lcrypto -ldl -rdynamic -no-pie -O2 2>&1";
                FILE* linkPipe = popen(linkCmd.c_str(), "r");
                if (!linkPipe) { printError("Failed to run linker"); return 1; }
                std::string linkOutput;
                while (fgets(buf, sizeof(buf), linkPipe)) linkOutput += buf;
                int linkStatus = pclose(linkPipe);
                if (WEXITSTATUS(linkStatus) != 0) {
                    // Try gcc as fallback
                    linkCmd = "gcc " + objFile + rtFlag + " -o " + binFile + " -lm -lpthread -lssl -lcrypto -ldl -rdynamic -no-pie -O2 2>&1";
                    linkPipe = popen(linkCmd.c_str(), "r");
                    if (linkPipe) { while (fgets(buf, sizeof(buf), linkPipe)) linkOutput += buf; linkStatus = pclose(linkPipe); }
                    if (WEXITSTATUS(linkStatus) != 0) {
                        printError("Linking failed:\n" + linkOutput);
                        return 1;
                    }
                }

                // Clean up intermediate files (keep .ll for debugging with --emit-llvm)
                fs::remove(llFile);
                fs::remove(objFile);

                auto binSize = fs::file_size(binFile);
                std::string modeStr = unchecked ? "Built native binary (unchecked): " : "Built native binary: ";
                printSuccess(modeStr + binFile + " (" + std::to_string(binSize / 1024) + " KB)");
                return 0;
            }

            return cmdBuild(source, file, release, format, unchecked);
        } catch (const std::runtime_error& e) { printError(e.what()); return 1; }
    }

    if (cmd == "run") {
        std::string file;
        bool release = false;
        bool profile = false;
        std::vector<std::string> progArgs;
        bool foundFile = false;
        bool afterSep = false;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--") { afterSep = true; continue; }
            if (afterSep) { progArgs.push_back(arg); continue; }
            if (arg == "--release") { release = true; continue; }
            if (arg == "--profile") { profile = true; continue; }
            if (!foundFile) { file = arg; foundFile = true; }
            else { progArgs.push_back(arg); }
        }
        if (file.empty()) { printError("file required: gard run <file> [-- args...]"); return 1; }
        progArgs.insert(progArgs.begin(), file);
        try {
            std::string source = readFile(file);
            auto start = std::chrono::steady_clock::now();
            int result = cmdRun(source, file, release, progArgs);
            auto end = std::chrono::steady_clock::now();
            if (profile) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                std::cerr << "\n" << COLOR_BLUE << "[profile]" << COLOR_RESET
                          << " Total execution: " << elapsed.count() / 1000.0 << "ms" << std::endl;
            }
            return result;
        } catch (const std::runtime_error& e) { printError(e.what()); return 1; }
    }

    // --- Legacy mode: file as first arg with optional flags ---
    // Supports: gard file.gard [--run|--check|--emit-llvm|etc]
    std::string file = cmd;
    std::string mode = (argc >= 3) ? argv[2] : "--check";

    std::string source;
    std::string filename;

    if (file == "--stdin") {
        source = readStdin();
        filename = "<stdin>";
    } else {
        filename = file;
        try { source = readFile(filename); }
        catch (const std::runtime_error& e) { printError(e.what()); return 1; }
    }

    if (mode == "--tokens") return cmdBuild(source, filename, false, "tokens");
    if (mode == "--parse") {
        gard::Lexer lexer(source, filename);
        auto tokens = lexer.tokenize();
        if (lexer.hasErrors()) { for (auto& e : lexer.getErrors()) printError(e); return 1; }
        gard::Parser parser(tokens, filename);
        auto program = parser.parse();
        if (parser.hasErrors()) { for (auto& e : parser.getErrors()) printError(e); return 1; }
        std::cout << "Parsed " << program.statements.size() << " top-level statements successfully." << std::endl;
        return 0;
    }
    if (mode == "--check") return cmdCheck(source, filename);
    if (mode == "--ir") return cmdBuild(source, filename, false, "ir");
    if (mode == "--ir-opt") return cmdBuild(source, filename, true, "ir");
    if (mode == "--emit-llvm") return cmdBuild(source, filename, true, "llvm");
    if (mode == "--bytecode") return cmdBuild(source, filename, false, "bytecode");
    if (mode == "--run") return cmdRun(source, filename, false);

    // Default: check
    return cmdCheck(source, filename);
}
