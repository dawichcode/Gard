# Gard Language Support

Full IDE support for the [Gard programming language](https://github.com/gard-lang/gard).

## Features

- **Syntax Highlighting** — Full TextMate grammar for all Gard syntax
- **Diagnostics** — Real-time error squiggles from lexer, parser, semantic analyzer, and type checker
- **Hover** — Type information and documentation on hover
- **Go to Definition** — Jump to symbol declarations (F12)
- **Find References** — Find all usages of a symbol (Shift+F12)
- **Rename Symbol** — Rename across the file (F2)
- **Autocomplete** — Context-aware completion with member access, keywords, and snippets
- **Signature Help** — Parameter hints while typing function calls
- **Document Symbols** — Outline panel with classes, functions, and variables
- **Formatting** — Format on save or on demand (Shift+Alt+F)
- **Code Actions** — Quick fixes for common issues
- **Semantic Highlighting** — Rich coloring based on symbol types
- **Folding Ranges** — Collapse functions, classes, and blocks
- **Inlay Hints** — Inline type annotations for untyped variables
- **Test Explorer** — Discover and run `@Test` annotated functions
- **Task Integration** — Build, run, check, and test from the command palette

## Commands

| Command | Keybinding | Description |
|---------|-----------|-------------|
| Gard: Build | Ctrl+Shift+B | Build the current file |
| Gard: Build (Release) | — | Build with optimizations |
| Gard: Run | Ctrl+F5 | Run the current file |
| Gard: Check | — | Type check without building |
| Gard: Format | — | Format the current file |
| Gard: Run Tests | — | Run all tests |
| Gard: Restart Language Server | — | Restart the LSP |

## Requirements

- `gard` compiler binary in PATH or configured via `gard.gardPath`
- `gard-lsp` binary in PATH or configured via `gard.lspPath`

Build both from the Gard source:
```bash
cd engine
cmake --build build --target gard
cmake --build build --target gard-lsp
```

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `gard.lspPath` | (auto) | Path to gard-lsp binary |
| `gard.gardPath` | `gard` | Path to gard compiler |
| `gard.runOnSave` | `false` | Run check on save |
| `gard.testPattern` | `**/*.test.gard` | Glob for test files |
