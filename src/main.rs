use gard_lexer::Lexer;
use gard_parser::{GardParser, GardParserTrait};
use gard_ast::Node;

fn main() {
    // Example input
    let input = r#"
       class main {
        function main(): void {
            let x: int = 42;
            let y: int = 43;
            let msg: string = "Hello, World!";
            print("this works");
            if (x > 0) {
                print(msg);
                print(y);
            }

           let caller = function(a: string): void {
            print(a);
           }

           return caller("this works");
        }

        blockchain contract Token {
            @event
            public class Transfer {
                public from: address;
                public to: address;
                public amount: uint;
            }
        }
       }
    "#;

    // Step 1: Lexical Analysis
    let mut lexer = Lexer::new(input);
    match lexer.tokenize() {
        Ok(tokens) => {
            println!("Lexical Analysis Successful. Found {} tokens.", tokens.len());
            
            // Step 2: Parsing
            match GardParser::parse(tokens) {
                Ok(ast) => {
                    println!("\nParsing Successful. AST:");
                    print_ast(&ast, 0);
                },
                Err(errors) => {
                    eprintln!("\nParsing Errors:");
                    for error in errors {
                        eprintln!("  {:?}", error);
                    }
                }
            }
        },
        Err(error) => {
            eprintln!("Lexical Analysis Error: {}", error);
        }
    }
}

// Helper function to print AST with indentation
fn print_ast(node: &Node, indent: usize) {
    let indent_str = " ".repeat(indent * 2);
    match node {
        Node::Program(nodes) => {
            println!("{}Program:", indent_str);
            for node in nodes {
                print_ast(node, indent + 1);
            }
        },
        Node::Class { name, extends, implements, members } => {
            println!("{}Class: {}", indent_str, name);
            if let Some(ext) = extends {
                println!("{}  extends: {}", indent_str, ext);
            }
            if !implements.is_empty() {
                println!("{}  implements: {}", indent_str, implements.join(", "));
            }
            for member in members {
                print_ast(member, indent + 1);
            }
        },
        Node::Function { name, params, return_type, .. } => {
            println!("{}Function: {} -> {:?}", indent_str, name, return_type);
            for param in params {
                println!("{}  Param: {} : {:?}", indent_str, param.name, param.type_annotation);
            }
        },
        // Add more match arms for other Node variants as needed
        _ => println!("{}Node: {:?}", indent_str, node),
    }
} 