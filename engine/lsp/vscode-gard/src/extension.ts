import * as path from 'path';
import * as fs from 'fs';
import * as cp from 'child_process';
import { workspace, ExtensionContext, window, commands, tasks, Task, TaskDefinition,
         ShellExecution, TaskScope, TaskGroup, Uri, tests, TestController, TestItem,
         TestRunProfileKind, TestRunRequest, CancellationToken, TestMessage,
         Range, Position } from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

// ============================================================================
// Binary Discovery
// ============================================================================

function findBinary(name: string, context: ExtensionContext): string | undefined {
    // 1. User config
    const config = workspace.getConfiguration('gard');
    const configPath = name === 'gard-lsp'
        ? config.get<string>('lspPath')
        : config.get<string>('gardPath');
    if (configPath && fs.existsSync(configPath)) return configPath;

    // 2. Workspace folders
    const workspaceFolders = workspace.workspaceFolders;
    if (workspaceFolders) {
        for (const folder of workspaceFolders) {
            const candidates = [
                path.join(folder.uri.fsPath, 'build', name),
                path.join(folder.uri.fsPath, 'engine', 'build', name),
                path.join(folder.uri.fsPath, '..', 'engine', 'build', name),
            ];
            for (const c of candidates) {
                if (fs.existsSync(c)) return c;
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
        if (fs.existsSync(c)) return c;
    }

    // 4. PATH
    const pathDirs = (process.env.PATH || '').split(path.delimiter);
    for (const dir of pathDirs) {
        const c = path.join(dir, name);
        if (fs.existsSync(c)) return c;
    }

    return undefined;
}

// ============================================================================
// Task Provider
// ============================================================================

interface GardTaskDefinition extends TaskDefinition {
    task: string;
    file?: string;
    release?: boolean;
}

function getGardBin(context: ExtensionContext): string {
    return findBinary('gard', context) || 'gard';
}

function createGardTask(context: ExtensionContext, taskName: string, args: string[], group?: TaskGroup): Task {
    const gardBin = getGardBin(context);
    const def: GardTaskDefinition = { type: 'gard', task: taskName };
    const task = new Task(
        def,
        TaskScope.Workspace,
        taskName,
        'gard',
        new ShellExecution(gardBin, args),
        '$gard'
    );
    if (group) task.group = group;
    return task;
}

// ============================================================================
// Test Explorer
// ============================================================================

function setupTestExplorer(context: ExtensionContext): TestController {
    const ctrl = tests.createTestController('gardTests', 'Gard Tests');
    context.subscriptions.push(ctrl);

    // Discover tests from @Test annotations
    const discoverTests = async () => {
        ctrl.items.replace([]);

        const files = await workspace.findFiles('**/*.test.gard');
        const allGardFiles = await workspace.findFiles('**/*.gard');

        // Also check regular .gard files for @Test annotations
        const testFiles = [...files];
        for (const file of allGardFiles) {
            if (!testFiles.find(f => f.fsPath === file.fsPath)) {
                try {
                    const content = fs.readFileSync(file.fsPath, 'utf-8');
                    if (content.includes('@Test') || content.includes('@TestClass')) {
                        testFiles.push(file);
                    }
                } catch { /* ignore */ }
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
                    while (j < lines.length && lines[j].trim().startsWith('@')) j++;
                    if (j < lines.length) {
                        const funcLine = lines[j].trim();
                        const match = funcLine.match(/function\s+(\w+)/);
                        if (match) {
                            const testName = match[1];
                            const testItem = ctrl.createTestItem(
                                `${file.fsPath}::${testName}`,
                                testName,
                                file
                            );
                            testItem.range = new Range(
                                new Position(j, 0),
                                new Position(j, funcLine.length)
                            );
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
    const runProfile = ctrl.createRunProfile('Run', TestRunProfileKind.Run, async (request: TestRunRequest, token: CancellationToken) => {
        const run = ctrl.createTestRun(request);
        const gardBin = getGardBin(context);

        const itemsToRun: TestItem[] = [];
        if (request.include) {
            request.include.forEach((item: TestItem) => itemsToRun.push(item));
        } else {
            ctrl.items.forEach((item: TestItem) => itemsToRun.push(item));
        }

        for (const item of itemsToRun) {
            if (token.isCancellationRequested) break;

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
                    cwd: workspace.workspaceFolders?.[0]?.uri.fsPath,
                });
                run.passed(item);
            } catch (err: any) {
                const message = new TestMessage(err.stderr || err.message || 'Test failed');
                run.failed(item, message);
            }
        }

        run.end();
    });

    // Watch for file changes
    const watcher = workspace.createFileSystemWatcher('**/*.gard');
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

export function activate(context: ExtensionContext): void {
    const lspBinary = findBinary('gard-lsp', context);

    // --- LSP Client ---
    if (lspBinary) {
        const serverOptions: ServerOptions = {
            command: lspBinary,
            transport: TransportKind.stdio,
        };

        const clientOptions: LanguageClientOptions = {
            documentSelector: [{ scheme: 'file', language: 'gard' }],
            synchronize: {
                fileEvents: workspace.createFileSystemWatcher('**/*.gard'),
            },
        };

        client = new LanguageClient(
            'gardLanguageServer',
            'Gard Language Server',
            serverOptions,
            clientOptions
        );

        client.start().catch((err: Error) => {
            window.showErrorMessage('Gard LSP failed: ' + err.message);
        });
    } else {
        window.showWarningMessage(
            'Gard LSP not found. Build with: cmake --build build --target gard-lsp'
        );
    }

    // --- Commands ---
    context.subscriptions.push(
        commands.registerCommand('gard.build', () => {
            const editor = window.activeTextEditor;
            if (!editor) return;
            const file = editor.document.fileName;
            const task = createGardTask(context, 'Build', ['build', file], TaskGroup.Build);
            tasks.executeTask(task);
        }),

        commands.registerCommand('gard.buildRelease', () => {
            const editor = window.activeTextEditor;
            if (!editor) return;
            const file = editor.document.fileName;
            const task = createGardTask(context, 'Build (Release)', ['build', '--release', file], TaskGroup.Build);
            tasks.executeTask(task);
        }),

        commands.registerCommand('gard.run', () => {
            const editor = window.activeTextEditor;
            if (!editor) return;
            const file = editor.document.fileName;
            const task = createGardTask(context, 'Run', ['run', file]);
            tasks.executeTask(task);
        }),

        commands.registerCommand('gard.check', () => {
            const editor = window.activeTextEditor;
            if (!editor) return;
            const file = editor.document.fileName;
            const task = createGardTask(context, 'Check', ['check', file]);
            tasks.executeTask(task);
        }),

        commands.registerCommand('gard.fmt', () => {
            const editor = window.activeTextEditor;
            if (!editor) return;
            const file = editor.document.fileName;
            const task = createGardTask(context, 'Format', ['fmt', file]);
            tasks.executeTask(task);
        }),

        commands.registerCommand('gard.test', () => {
            const task = createGardTask(context, 'Test', ['test'], TaskGroup.Test);
            tasks.executeTask(task);
        }),

        commands.registerCommand('gard.restartLsp', async () => {
            if (client) {
                await client.stop();
                await client.start();
                window.showInformationMessage('Gard LSP restarted');
            }
        })
    );

    // --- Test Explorer ---
    setupTestExplorer(context);

    // --- Task Provider ---
    const taskProvider = tasks.registerTaskProvider('gard', {
        provideTasks: () => {
            return [
                createGardTask(context, 'build', ['build', '${file}'], TaskGroup.Build),
                createGardTask(context, 'build --release', ['build', '--release', '${file}'], TaskGroup.Build),
                createGardTask(context, 'run', ['run', '${file}']),
                createGardTask(context, 'check', ['check', '${file}']),
                createGardTask(context, 'test', ['test'], TaskGroup.Test),
                createGardTask(context, 'fmt', ['fmt', '${file}']),
            ];
        },
        resolveTask: () => undefined,
    });
    context.subscriptions.push(taskProvider);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) return undefined;
    return client.stop();
}
