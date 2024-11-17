# Gard Programming Language

Gard is a high-performance, concurrent, and memory-safe programming language designed for asynchronous programming. It is ideal for developing blockchain applications, real-time services, and WebAssembly modules. Gard leverages Rust's safety and performance capabilities, offering a developer-friendly syntax inspired by languages like Java, TypeScript, and Dart.

## Key Features

- **Asynchronous Programming**: Support for async, await, and synchronization constructs like lock, sync, and semaphore
- **Named Parameters**: Provides flexibility and readability in function calls with named parameters
- **Local Functions**: Define functions within other functions for encapsulation and better code organization
- **WebAssembly Support**: Gard programs can be compiled to .ga files for running in WebAssembly environments
- **Class-Based System**: Supports OOP with classes, inheritance, and encapsulation
- **Blockchain-Specific Keywords**: Built-in support for blockchain concepts
- **Modular Code**: Use export and import to manage modular code across files
- **Synchronization**: Built-in keywords for synchronization tasks
- **Native WebSocket Stream**: Integrated WebSocket support using stream for real-time communication

## File Extensions

- Source files: `.gard`
- Compiled output: `.ga`

## Installation

1. Install Rust:

```
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

2. Clone the Gard repository:

```bash
git clone https://github.com/yourusername/gard.git
cd gard
```

3. Build the compiler:

```bash
cargo build --release
```

4. Run your first Gard program:

```bash
./target/release/gard run your_program.gard
```

## Language Syntax

### Variable Declaration
```gard
let x: int = 10;
const pi: double = 3.14;
var message: string = "Hello, Gard!";
```

### Function Definition with Local Functions
```gard
function calculate(a: int, b: int): int {
    // Local function definition
    function add(x: int, y: int): int {
        return x + y;
    }

    // Local function assigned to a variable
    let multiply = function(x: int, y: int): int {
        return x * y;
    };

    return add(a, b) + multiply(a, b);
}
```

### Named Parameters
```gard
function createUser({name: string, age: int, email: string = "Not Provided"}): void {
    print("Name: " + name);
    print("Age: " + age);
    print("Email: " + email);
}
```

### Synchronization Example
```gard
sync class Counter {
    private var count: int = 0;

    public function increment(): void {
        lock(this);
        this.count += 1;
        unlock(this);
    }

    public function getCount(): int {
        return this.count;
    }
}
```

### Blockchain Example
```gard
blockchain function createTransaction(sender: string, receiver: string, amount: double): transaction {
    return new transaction(sender, receiver, amount);
}
```

## Synchronization Keywords

- `sync`: Marks a synchronized block
- `lock/unlock`: Acquire and release locks
- `semaphore`: Control access to a resource
- `mutex`: Mutual exclusion lock
- `wait/signal`: Task waiting and signaling
- `barrier`: Synchronization point for tasks
- `task`: Defines an asynchronous task
- `sync class`: Defines a synchronized class

## Blockchain Keywords

- `blockchain`: Defines a blockchain instance
- `transaction`: Blockchain transaction
- `contract`: Smart contract definition
- `ledger`: Storage for transactions
- `validate/mine/sign`: Actions for transaction processing
- `block/hash`: Block and hash utilities

## Project Structure

```
/gard
├── src/
│   ├── main.rs               # Compiler entry point
│   ├── lexer.rs              # Tokenizer
│   ├── parser.rs             # Syntax analysis
│   ├── interpreter.rs        # Interpreter for execution
├── examples/                 # Example programs
│   ├── async_example.gard
│   ├── blockchain_example.gard
│   └── sync_example.gard
├── target/                   # Compiled binaries
├── README.md
└── Cargo.toml
```

## Usage

1. Write a program in a `.gard` file
2. Compile the program:

```bash
./target/release/gard run your_program.gard
```

3. The compiled program will have a `.ga` extension

## Contributing

Contributions are welcome! Follow these steps:

1. Fork the repository
2. Create a new branch (`feature-branch`)
3. Make changes and commit
4. Push to your fork and submit a pull request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.