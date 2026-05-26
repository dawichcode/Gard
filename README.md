<p align="center">
  <img src="engine/lsp/vscode-gard/icon.png" alt="Gard Logo" width="120" />
</p>

<h1 align="center">Gard Programming Language</h1>

<p align="center">
  A statically-typed, compiled language with first-class async, generics, and native compilation via LLVM.
</p>

<p align="center">
  <strong>Native</strong> · <strong>WebAssembly</strong> · <strong>Bytecode VM</strong>
</p>

---

## Overview

Gard is a general-purpose programming language built from scratch in C++. It combines familiar C-family syntax with modern features like async/await, reified generics, pattern matching, and operator overloading — all compiled to native machine code through LLVM, WebAssembly, or executed on a custom bytecode VM with JIT support.

```gard
function main(): void {
    print("Hello, World!");
}
```

## Features

- **Static typing** with type inference
- **Native compilation** via LLVM (x64, ARM64)
- **WebAssembly** output (`.wasm` / `.wat`)
- **Async/await** with cooperative scheduling
- **Reified generics** — type arguments preserved at runtime
- **Classes** with inheritance, interfaces, and operator overloading
- **Pattern matching** via `match` expressions
- **String interpolation** with template literals
- **Concurrency primitives** — Mutex, Semaphore, Channel, Barrier, RWLock
- **FFI** — call C libraries directly
- **Pointers** — `&x` (address-of) and `*ptr` (dereference)
- **Annotations** — `@Test`, `@Route`, `@Injectable`, custom decorators
- **Built-in error types** with structured stack traces
- **Module system** with `import`/`export`

## Quick Start

```bash
# Build and run
gard run main.gard

# Release build (optimized via LLVM)
gard build --release main.gard

# Compile to WebAssembly
gard build --target wasm main.gard

# Type check only
gard check main.gard

# Format code
gard fmt main.gard
```

## Language Tour

### Variables & Types

```gard
let name: string = "Gard";
let version = 1;              // type inferred as int
const PI: float = 3.14159;    // immutable
let items: array<int> = [1, 2, 3, 4, 5];
```

### Functions

```gard
function add(a: int, b: int): int {
    return a + b;
}

function greet(name: string): void {
    print(`Hello, ${name}!`);
}
```

### Control Flow

```gard
// If/else
if (score >= 90) {
    return "A";
} else if (score >= 80) {
    return "B";
} else {
    return "C";
}

// For loop
for (let i = 0; i < 10; i = i + 1) {
    print(i);
}

// While loop
while (running) {
    process();
}

// Switch
switch (day) {
    case 1: return "Monday";
    case 2: return "Tuesday";
    default: return "Other";
}

// Pattern matching
match (value) {
    42 => print("the answer"),
    0 => print("zero"),
    default => print("something else")
}
```

### Classes & Inheritance

```gard
class Animal {
    public let name: string = "";

    constructor(name: string) {
        this.name = name;
    }

    public function speak(): string {
        return "...";
    }
}

class Dog extends Animal {
    constructor(name: string) {
        super(name);
    }

    public function speak(): string {
        return "Woof!";
    }
}
```

### Interfaces

```gard
interface Printable {
    describe(): string;
}

class Cat implements Printable {
    public name: string;

    constructor(name: string) {
        this.name = name;
    }

    public function describe(): string {
        return "Cat: " + this.name;
    }
}
```

### Generics

```gard
class Box<T> {
    let value: T;

    constructor(val: T) {
        this.value = val;
    }

    function get(): T {
        return this.value;
    }
}

let intBox = new Box<int>(42);
let strBox = new Box<string>("hello");
```

### Enums

```gard
enum Color {
    RED,
    GREEN,
    BLUE
}

enum Direction {
    NORTH,
    SOUTH,
    EAST,
    WEST
}

let c = Color.RED;
```

### Operator Overloading

```gard
class Vector2D {
    public let x: int = 0;
    public let y: int = 0;

    constructor(x: int, y: int) {
        this.x = x;
        this.y = y;
    }

    public function __add(other: Vector2D): Vector2D {
        return new Vector2D(this.x + other.x, this.y + other.y);
    }

    public function __eq(other: Vector2D): boolean {
        return this.x == other.x && this.y == other.y;
    }
}

let v1 = new Vector2D(3, 4);
let v2 = new Vector2D(1, 2);
let v3 = v1 + v2;  // Vector2D(4, 6)
```

### Async/Await

```gard
async function fetchData(id: int): string {
    // Non-blocking I/O
    let response = await HttpClient.get(`/api/data/${id}`);
    return Response.body(response);
}

function main(): void {
    let data = await fetchData(42);
    print(data);
}
```

### Higher-Order Functions

```gard
let numbers = [1, 2, 3, 4, 5];

let doubled = numbers.map((x) => x * 2);
let evens = numbers.filter((x) => x % 2 == 0);
let sum = numbers.reduce((acc, x) => acc + x, 0);
```

### String Interpolation

```gard
let name = "Gard";
let version = "0.1.0";
let msg = `${name} v${version} is awesome`;
print(msg);  // Gard v0.1.0 is awesome
```

### Error Handling

```gard
try {
    throw GardNullPointerError("x is null");
} catch (e) {
    print(e.type);     // GardNullPointerError
    print(e.message);  // x is null
    print(e.line);     // source line number
} finally {
    cleanup();
}
```

Built-in error types: `GardNullPointerError`, `GardIOError`, `GardTimeoutError`, `GardValidationError`, `GardPermissionError`, `GardAssertionError`.

### Concurrency

```gard
import { Mutex } from "gard/core";

class SafeCounter {
    private let count: int = 0;
    private let mutex: Mutex = new Mutex();

    public function increment(): void {
        lock(mutex) {
            this.count = this.count + 1;
        }
    }
}
```

Primitives: `Mutex`, `Semaphore`, `Channel<T>`, `Barrier`, `RWLock`.

### Modules & Imports

```gard
// Import from local files
import { add, Calculator } from "./math_lib";

// Import from standard library
import { HttpClient } from "gard/network";
import { Database, ORM } from "gard/core";
import { FFI, Pointer } from "gard/native";
```

### Pointers

```gard
let x = 42;
let ptr = &x;       // address-of
let val = *ptr;     // dereference (42)
*ptr = 100;         // write through pointer
print(x);           // 100
```

### FFI (Foreign Function Interface)

```gard
import { FFI, Pointer } from "gard/native";

let libc = FFI.loadLibrary("libc.so.6");
let getpid = FFI.getFunction(libc, "getpid");
let pid = FFI.call(getpid);
print(pid);
FFI.closeLibrary(libc);
```

### Annotations

```gard
@TestClass
class MathTests {
    @BeforeEach
    public function setup(): void { }

    @Test
    public function testAddition(): void {
        assert.equals(2 + 2, 4);
    }

    @AfterEach
    public function teardown(): void { }
}
```

### HTTP Server

```gard
import { HttpServer, HttpClient } from "gard/network";

function main(): void {
    let server = HttpServer.create(8080);
    HttpServer.route(server, "GET", "/hello", "Hello from Gard!");
    HttpServer.listen(server);
}
```

### WebSocket

```gard
import { WebSocket } from "gard/network";

let ws = WebSocket.connect("ws://echo.websocket.events");
WebSocket.send(ws, "hello from gard");
let msg = WebSocket.receive(ws);
WebSocket.close(ws);
```

### Database & ORM

```gard
import { Database, ORM } from "gard/core";

let db = Database.connect("sqlite://:memory:");
ORM.createTable(db, "User", "name TEXT, age INTEGER, email TEXT");
Database.execute(db, "INSERT INTO user (name, age) VALUES ('Alice', 30)");
Database.close(db);
```

## Standard Library

| Module | Description |
|--------|-------------|
| `gard/core` | Collections, Math, String, Console, Database, ORM, JSON |
| `gard/network` | HttpClient, HttpServer, WebSocket, TCP, UDP |
| `gard/native` | OS, FFI, Pointer, Process, FileSystem |
| `gard/graphics` | Canvas, Window, Image (SDL2-based) |

## Compilation Targets

| Target | Output | Use Case |
|--------|--------|----------|
| Native (LLVM) | ELF/Mach-O binary | Applications, servers, CLI tools |
| WebAssembly | `.wasm` / `.wat` | Browser, edge computing |
| Bytecode | `.ga` | Development, scripting, JIT |

## Project Structure

```
my-project/
├── src/
│   └── main.gard          # Entry point
├── test/
│   └── *.test.gard        # Test files
├── dist/                   # Build output
├── gard.json               # Project manifest
└── gard.lock               # Dependency lockfile
```

## CLI Reference

```bash
gard new <project>          # Scaffold new project
gard run [file]             # Build and execute
gard build                  # Debug build
gard build --release        # Optimized build
gard build --target wasm    # WebAssembly output
gard check                  # Type check only
gard fmt                    # Format code
gard test                   # Run tests
gard test --coverage        # With coverage report
gard add <package>          # Add dependency
gard doc                    # Generate documentation
gard publish                # Publish to GardHub
```

## IDE Support

The [Gard VS Code Extension](engine/lsp/vscode-gard/) provides:

- Real-time diagnostics (error squiggles)
- Syntax highlighting with semantic tokens
- Go-to-definition and hover information
- Autocomplete with type-aware ranking
- Signature help for function calls
- Find references and rename symbol
- Code formatting
- Inlay hints for inferred types
- Document and workspace symbol search

## Building from Source

```bash
cd engine
mkdir -p build && cd build
cmake ..
cmake --build . --target gard        # Compiler
cmake --build . --target gard-lsp    # Language server
```

Requirements: C++17 compiler, CMake 3.16+, LLVM 15+.

## Testing

```bash
# Run a sample
./build/gard run tests/samples/hello.gard

# Compile to native binary
./build/gard build --release tests/samples/class.gard

# Run all test samples
gard test
```

## License

All rights reserved.
