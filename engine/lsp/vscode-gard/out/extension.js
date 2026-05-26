"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const path = __importStar(require("path"));
const fs = __importStar(require("fs"));
const cp = __importStar(require("child_process"));
const vscode_1 = require("vscode");
const node_1 = require("vscode-languageclient/node");
let client;
// ============================================================================
// Binary Discovery
// ============================================================================
function findBinary(name, context) {
    // 1. User config
    const config = vscode_1.workspace.getConfiguration('gard');
    const configPath = name === 'gard-lsp'
        ? config.get('lspPath')
        : config.get('gardPath');
    if (configPath && fs.existsSync(configPath))
        return configPath;
    // 2. Workspace folders
    const workspaceFolders = vscode_1.workspace.workspaceFolders;
    if (workspaceFolders) {
        for (const folder of workspaceFolders) {
            const candidates = [
                path.join(folder.uri.fsPath, 'build', name),
                path.join(folder.uri.fsPath, 'engine', 'build', name),
                path.join(folder.uri.fsPath, '..', 'engine', 'build', name),
            ];
            for (const c of candidates) {
                if (fs.existsSync(c))
                    return c;
            }
        }
    }
    // 3. Relative to extension
    const extDir = context.extensionPath;
    const relCandidates = [
        path.join(extDir, '..', '..', 'build', name),
        path.join(extDir, '..', 'build', name),
    ];
    for (const c of relCandidates) {
        if (fs.existsSync(c))
            return c;
    }
    // 4. PATH
    const pathDirs = (process.env.PATH || '').split(path.delimiter);
    for (const dir of pathDirs) {
        const c = path.join(dir, name);
        if (fs.existsSync(c))
            return c;
    }
    return undefined;
}
function getGardBin(context) {
    return findBinary('gard', context) || 'gard';
}
function createGardTask(context, taskName, args, group) {
    const gardBin = getGardBin(context);
    const def = { type: 'gard', task: taskName };
    const task = new vscode_1.Task(def, vscode_1.TaskScope.Workspace, taskName, 'gard', new vscode_1.ShellExecution(gardBin, args), '$gard');
    if (group)
        task.group = group;
    return task;
}
// ============================================================================
// Test Explorer
// ============================================================================
function setupTestExplorer(context) {
    const ctrl = vscode_1.tests.createTestController('gardTests', 'Gard Tests');
    context.subscriptions.push(ctrl);
    // Discover tests from @Test annotations
    const discoverTests = async () => {
        ctrl.items.replace([]);
        const files = await vscode_1.workspace.findFiles('**/*.test.gard');
        const allGardFiles = await vscode_1.workspace.findFiles('**/*.gard');
        // Also check regular .gard files for @Test annotations
        const testFiles = [...files];
        for (const file of allGardFiles) {
            if (!testFiles.find(f => f.fsPath === file.fsPath)) {
                try {
                    const content = fs.readFileSync(file.fsPath, 'utf-8');
                    if (content.includes('@Test') || content.includes('@TestClass')) {
                        testFiles.push(file);
                    }
                }
                catch { /* ignore */ }
            }
        }
        for (const file of testFiles) {
            const content = fs.readFileSync(file.fsPath, 'utf-8');
            const fileName = path.basename(file.fsPath);
            const fileItem = ctrl.createTestItem(file.fsPath, fileName, file);
            fileItem.canResolveChildren = true;
            // Find @Test annotated functions
            const lines = content.split('\n');
            let inTestClass = false;
            for (let i = 0; i < lines.length; i++) {
                const line = lines[i].trim();
                if (line.startsWith('@TestClass')) {
                    inTestClass = true;
                }
                if (line.startsWith('@Test')) {
                    // Next non-annotation line should be the function
                    let j = i + 1;
                    while (j < lines.length && lines[j].trim().startsWith('@'))
                        j++;
                    if (j < lines.length) {
                        const funcLine = lines[j].trim();
                        const match = funcLine.match(/function\s+(\w+)/);
                        if (match) {
                            const testName = match[1];
                            const testItem = ctrl.createTestItem(`${file.fsPath}::${testName}`, testName, file);
                            testItem.range = new vscode_1.Range(new vscode_1.Position(j, 0), new vscode_1.Position(j, funcLine.length));
                            fileItem.children.add(testItem);
                        }
                    }
                }
            }
            if (fileItem.children.size > 0 || fileName.endsWith('.test.gard')) {
                ctrl.items.add(fileItem);
            }
        }
    };
    // Run profile
    const runProfile = ctrl.createRunProfile('Run', vscode_1.TestRunProfileKind.Run, async (request, token) => {
        const run = ctrl.createTestRun(request);
        const gardBin = getGardBin(context);
        const itemsToRun = [];
        if (request.include) {
            request.include.forEach((item) => itemsToRun.push(item));
        }
        else {
            ctrl.items.forEach((item) => itemsToRun.push(item));
        }
        for (const item of itemsToRun) {
            if (token.isCancellationRequested)
                break;
            run.started(item);
            const filePath = item.uri?.fsPath;
            if (!filePath) {
                run.skipped(item);
                continue;
            }
            try {
                const result = cp.execSync(`${gardBin} run ${filePath}`, {
                    encoding: 'utf-8',
                    timeout: 30000,
                    cwd: vscode_1.workspace.workspaceFolders?.[0]?.uri.fsPath,
                });
                run.passed(item);
            }
            catch (err) {
                const message = new vscode_1.TestMessage(err.stderr || err.message || 'Test failed');
                run.failed(item, message);
            }
        }
        run.end();
    });
    // Watch for file changes
    const watcher = vscode_1.workspace.createFileSystemWatcher('**/*.gard');
    watcher.onDidChange(() => discoverTests());
    watcher.onDidCreate(() => discoverTests());
    watcher.onDidDelete(() => discoverTests());
    context.subscriptions.push(watcher);
    // Initial discovery
    discoverTests();
    return ctrl;
}
// ============================================================================
// Extension Activation
// ============================================================================
function activate(context) {
    const lspBinary = findBinary('gard-lsp', context);
    // --- LSP Client ---
    if (lspBinary) {
        const serverOptions = {
            command: lspBinary,
            transport: node_1.TransportKind.stdio,
        };
        const clientOptions = {
            documentSelector: [{ scheme: 'file', language: 'gard' }],
            synchronize: {
                fileEvents: vscode_1.workspace.createFileSystemWatcher('**/*.gard'),
            },
        };
        client = new node_1.LanguageClient('gardLanguageServer', 'Gard Language Server', serverOptions, clientOptions);
        client.start().catch((err) => {
            vscode_1.window.showErrorMessage('Gard LSP failed: ' + err.message);
        });
    }
    else {
        vscode_1.window.showWarningMessage('Gard LSP not found. Build with: cmake --build build --target gard-lsp');
    }
    // --- Commands ---
    context.subscriptions.push(vscode_1.commands.registerCommand('gard.build', () => {
        const editor = vscode_1.window.activeTextEditor;
        if (!editor)
            return;
        const file = editor.document.fileName;
        const task = createGardTask(context, 'Build', ['build', file], vscode_1.TaskGroup.Build);
        vscode_1.tasks.executeTask(task);
    }), vscode_1.commands.registerCommand('gard.buildRelease', () => {
        const editor = vscode_1.window.activeTextEditor;
        if (!editor)
            return;
        const file = editor.document.fileName;
        const task = createGardTask(context, 'Build (Release)', ['build', '--release', file], vscode_1.TaskGroup.Build);
        vscode_1.tasks.executeTask(task);
    }), vscode_1.commands.registerCommand('gard.run', () => {
        const editor = vscode_1.window.activeTextEditor;
        if (!editor)
            return;
        const file = editor.document.fileName;
        const task = createGardTask(context, 'Run', ['run', file]);
        vscode_1.tasks.executeTask(task);
    }), vscode_1.commands.registerCommand('gard.check', () => {
        const editor = vscode_1.window.activeTextEditor;
        if (!editor)
            return;
        const file = editor.document.fileName;
        const task = createGardTask(context, 'Check', ['check', file]);
        vscode_1.tasks.executeTask(task);
    }), vscode_1.commands.registerCommand('gard.fmt', () => {
        const editor = vscode_1.window.activeTextEditor;
        if (!editor)
            return;
        const file = editor.document.fileName;
        const task = createGardTask(context, 'Format', ['fmt', file]);
        vscode_1.tasks.executeTask(task);
    }), vscode_1.commands.registerCommand('gard.test', () => {
        const task = createGardTask(context, 'Test', ['test'], vscode_1.TaskGroup.Test);
        vscode_1.tasks.executeTask(task);
    }), vscode_1.commands.registerCommand('gard.restartLsp', async () => {
        if (client) {
            await client.stop();
            await client.start();
            vscode_1.window.showInformationMessage('Gard LSP restarted');
        }
    }));
    // --- Test Explorer ---
    setupTestExplorer(context);
    // --- Task Provider ---
    const taskProvider = vscode_1.tasks.registerTaskProvider('gard', {
        provideTasks: () => {
            return [
                createGardTask(context, 'build', ['build', '${file}'], vscode_1.TaskGroup.Build),
                createGardTask(context, 'build --release', ['build', '--release', '${file}'], vscode_1.TaskGroup.Build),
                createGardTask(context, 'run', ['run', '${file}']),
                createGardTask(context, 'check', ['check', '${file}']),
                createGardTask(context, 'test', ['test'], vscode_1.TaskGroup.Test),
                createGardTask(context, 'fmt', ['fmt', '${file}']),
            ];
        },
        resolveTask: () => undefined,
    });
    context.subscriptions.push(taskProvider);
}
function deactivate() {
    if (!client)
        return undefined;
    return client.stop();
}
//# sourceMappingURL=extension.js.map