use logos::Logos;
use std::fmt;
use std::hash::Hash;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl fmt::Display for Span {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}..{}", self.start, self.end)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct TokenWithSpan {
    pub token: Token,
    pub span: Span,
}

#[derive(Logos, Debug, PartialEq, Eq, Hash, Clone)]
pub enum Token {
    // Skip whitespace and comments
    #[regex(r"[ \t\n\f]+", logos::skip)]
    Error,

    #[regex(r"//[^\n]*", logos::skip)]
    #[regex(r"/\*([^*]|\*[^/])*\*/", logos::skip)]
    Comment,

    // Keywords
    #[token("let")]
    Let,
    #[token("const")]
    Const,
    #[token("function")]
    Function,
    #[token("class")]
    Class,
    #[token("extends")]
    Extends,
    #[token("implements")]
    Implements,
    #[token("interface")]
    Interface,
    #[token("return")]
    Return,
    #[token("if")]
    If,
    #[token("else")]
    Else,
    #[token("while")]
    While,
    #[token("for")]
    For,
    #[token("break")]
    Break,
    #[token("continue")]
    Continue,
    #[token("async")]
    Async,
    #[token("await")]
    Await,
    #[token("blockchain")]
    Blockchain,
    #[token("contract")]
    Contract,
    #[token("transaction")]
    Transaction,

    // Types
    #[token("void")]
    Void,
    #[token("int")]
    Int,
    #[token("uint")]
    UInt,
    #[token("float")]
    Float,
    #[token("double")]
    Double,
    #[token("string")]
    String,
    #[token("boolean")]
    Boolean,
    #[token("array")]
    Array,
    #[token("map")]
    Map,
    #[token("set")]
    Set,
    #[token("address")]
    Address,
    #[token("char")]
    Char,

    // Literals
    #[regex(r"-?[0-9]+")]
    IntLiteral,
    #[regex(r"-?[0-9]+\.[0-9]+")]
    FloatLiteral,
    #[regex(r#""([^"\\]|\\['"\\nrt])*""#)]
    StringLiteral,
    #[regex("'[^']*'")]
    CharLiteral,
    #[regex(r"0x[0-9a-fA-F]+")]
    HexLiteral,
    #[regex(r"0b[01]+")]
    BinaryLiteral,
    #[regex(r"0o[0-7]+")]
    OctalLiteral,
    #[regex(r"-?[0-9]+(\.[0-9]+)?[eE][+-]?[0-9]+")]
    ScientificLiteral,
    #[token("true")]
    True,
    #[token("false")]
    False,
    #[token("null")]
    Null,

    // Identifiers
    #[regex("[a-zA-Z_][a-zA-Z0-9_]*", priority = 1)]
    Identifier,

    // Operators
    #[token("+")]
    Plus,
    #[token("-")]
    Minus,
    #[token("*")]
    Multiply,
    #[token("/")]
    Divide,
    #[token("%")]
    Modulo,
    #[token("=")]
    Assign,
    #[token("+=")]
    PlusAssign,
    #[token("-=")]
    MinusAssign,
    #[token("*=")]
    MultiplyAssign,
    #[token("/=")]
    DivideAssign,
    #[token("%=")]
    ModuloAssign,
    #[token("++")]
    Increment,
    #[token("--")]
    Decrement,
    #[token("==")]
    Equals,
    #[token("!=")]
    NotEquals,
    #[token("<")]
    LessThan,
    #[token("<=")]
    LessEquals,
    #[token(">")]
    GreaterThan,
    #[token(">=")]
    GreaterEquals,
    #[token("&&")]
    And,
    #[token("||")]
    Or,
    #[token("!")]
    Not,
    #[token("??")]
    NullCoalesce,
    #[token("?.")]
    OptionalChain,
    #[token("...")]
    Spread,

    // Delimiters
    #[token("(")]
    LeftParen,
    #[token(")")]
    RightParen,
    #[token("{")]
    LeftBrace,
    #[token("}")]
    RightBrace,
    #[token("[")]
    LeftBracket,
    #[token("]")]
    RightBracket,
    #[token(";")]
    Semicolon,
    #[token(",")]
    Comma,
    #[token(".")]
    Dot,
    #[token(":")]
    Colon,
    #[token("=>")]
    Arrow,
    #[token("::")]
    DoubleColon,

    // Template Strings
    #[regex(r"`[^`]*`")]
    TemplateString,
    #[regex(r"\$\{[^}]*\}")]
    TemplateInterpolation,

    // Documentation
    #[regex(r"///[^\n]*")]
    DocComment,
    #[regex(r"/\*\*([^*]|\*[^/])*\*/")]
    MultilineDocComment,

    // Blockchain Specific
    #[token("ledger")]
    Ledger,
    #[token("validate")]
    Validate,
    #[token("mine")]
    Mine,
    #[token("block")]
    Block,
    #[token("hash")]
    Hash,
    #[token("msg.sender")]
    MsgSender,
    #[token("new")]
    New,
    #[token("sign")]
    Sign,
    #[token("payable")]
    Payable,
    #[token("view")]
    View,
    #[token("pure")]
    Pure,
    #[token("emit")]
    Emit,
    #[token("constructor")]
    Constructor,
    #[token("this")]
    This,
    #[token("super")]
    Super,

    // Concurrency
    #[token("sync")]
    Sync,
    #[token("mutex")]
    Mutex,
    #[token("semaphore")]
    Semaphore,
    #[token("barrier")]
    Barrier,
    #[token("lock")]
    Lock,
    #[token("unlock")]
    Unlock,
    #[token("wait")]
    Wait,
    #[token("signal")]
    Signal,
    #[token("spawn")]
    Spawn,
    #[token("channel")]
    Channel,
    #[token("select")]
    Select,
    #[token("task")]
    Task,
    #[token("commit")]
    Commit,
    #[token("abort")]
    Abort,
    #[token("retry")]
    Retry,
    #[token("backoff")]
    Backoff,

    // WebAssembly
    #[token("@wasm")]
    Wasm,
    #[token("@WasmExport")]
    WasmExport,
    #[token("@WasmImport")]
    WasmImport,
    #[token("@WasmMemory")]
    WasmMemory,

    // Decorators
    #[token("@")]
    At,
    #[token("@event")]
    Event,
    #[token("@modifier")]
    Modifier,
    #[token("@scheduled")]
    Scheduled,

    // Control Flow
    #[token("foreach")]
    Foreach,
    #[token("do")]
    Do,
    #[token("match")]
    Match,
    #[token("case")]
    Case,
    #[token("_")]
    Underscore,
    #[token("in")]
    In,

    // Decision Tokens
    #[token("Decision.RESTART")]
    DecisionRestart,
    #[token("Decision.STOP")]
    DecisionStop,
    #[token("Decision.ESCALATE")]
    DecisionEscalate,

    // Additional Keywords
    #[token("readonly")]
    Readonly,
    #[token("Promise.all")]
    PromiseAll,
    #[token("become")]
    Become,
   

    // Module System
    #[token("import")]
    Import,
    #[token("export")]
    Export,
    #[token("from")]
    From,
    #[token("as")]
    As,

    // Actor System
    #[token("Actor")]
    Actor,
    #[token("MessageQueue")]
    MessageQueue,
    #[token("ActorBehavior")]
    ActorBehavior,
    #[token("Supervisor")]
    Supervisor,
    #[token("SupervisionStrategy")]
    SupervisionStrategy,
    #[token("Decision")]
    Decision,

    // STM
    #[token("TVar")]
    TVar,
    #[token("atomic", priority = 2)]
    Atomic,
  
    #[token("try")]
    Try,

    // Access modifiers
    #[token("public")]
    Public,
    #[token("private")]
    Private,

    #[token("catch")]
    Catch,
    #[token("throw")]
    Throw,
    #[token("finally")]
    Finally,
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", self)
    }
}

#[derive(Debug)]
pub enum LexerError {
    InvalidToken { 
        position: usize,
        found: String,
        expected: Vec<String>,
    },
    UnterminatedString { 
        position: usize,
        partial: String,
    },
    InvalidEscape { 
        position: usize,
        sequence: String,
    },
    InvalidNumber {
        position: usize,
        value: String,
    },
    UnterminatedComment {
        position: usize,
    },
    InvalidCharacter {
        position: usize,
        character: char,
    },
    InvalidActorMessage {
        position: usize,
        message: String,
    },
    InvalidTransactionState {
        position: usize,
        state: String,
    },
    InvalidDecisionType {
        position: usize,
        decision: String,
    },
    InvalidBehaviorType {
        position: usize,
        behavior: String,
    },
}

impl std::fmt::Display for LexerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LexerError::InvalidToken { position, found, expected } => {
                write!(f, "Invalid token '{}' at position {}, expected one of: {}", 
                    found, position, expected.join(", "))
            },
            LexerError::UnterminatedString { position, partial } => {
                write!(f, "Unterminated string literal starting at position {}: '{}'", 
                    position, partial)
            },
            LexerError::InvalidEscape { position, sequence } => {
                write!(f, "Invalid escape sequence '{}' at position {}", 
                    sequence, position)
            },
            LexerError::InvalidNumber { position, value } => {
                write!(f, "Invalid number format '{}' at position {}", 
                    value, position)
            },
            LexerError::UnterminatedComment { position } => {
                write!(f, "Unterminated comment starting at position {}", position)
            },
            LexerError::InvalidCharacter { position, character } => {
                write!(f, "Invalid character '{}' at position {}", character, position)
            },
            LexerError::InvalidActorMessage { position, message } => {
                write!(f, "Invalid actor message '{}' at position {}", message, position)
            },
            LexerError::InvalidTransactionState { position, state } => {
                write!(f, "Invalid transaction state '{}' at position {}", state, position)
            },
            LexerError::InvalidDecisionType { position, decision } => {
                write!(f, "Invalid supervision decision '{}' at position {}", decision, position)
            },
            LexerError::InvalidBehaviorType { position, behavior } => {
                write!(f, "Invalid actor behavior '{}' at position {}", behavior, position)
            },
        }
    }
}

impl std::error::Error for LexerError {}

pub struct Lexer<'a> {
    inner: logos::Lexer<'a, Token>,
}

impl<'a> Lexer<'a> {
    pub fn new(input: &'a str) -> Self {
        Self {
            inner: Token::lexer(input),
        }
    }

    pub fn tokenize(&mut self) -> Result<Vec<TokenWithSpan>, LexerError> {
        let mut tokens = Vec::new();
        
        while let Some(token) = self.inner.next() {
            let span = Span {
                start: self.inner.span().start,
                end: self.inner.span().end,
            };

            match token {
                Ok(token) => tokens.push(TokenWithSpan { token, span }),
                Err(_) => return Err(LexerError::InvalidToken {
                    position: self.inner.span().start,
                    found: self.inner.slice().to_string(),
                    expected: vec!["valid token".to_string()],
                }),
            }
        }

        Ok(tokens)
    }

    pub fn tokenize_with_errors(&mut self) -> (Vec<TokenWithSpan>, Vec<LexerError>) {
        let mut tokens = Vec::new();
        let mut errors = Vec::new();

        while let Some(token) = self.inner.next() {
            let span = Span {
                start: self.inner.span().start,
                end: self.inner.span().end,
            };

            match token {
                Ok(token) => tokens.push(TokenWithSpan { token, span }),
                Err(_) => errors.push(LexerError::InvalidToken {
                    position: self.inner.span().start,
                    found: self.inner.slice().to_string(),
                    expected: vec!["valid token".to_string()],
                }),
            }
        }

        (tokens, errors)
    }

    pub fn tokenize_with_recovery(&mut self) -> (Vec<TokenWithSpan>, Vec<LexerError>) {
        let mut tokens = Vec::new();
        let mut errors = Vec::new();
        let mut current_pos = 0;

        while let Some(result) = self.inner.next() {
            let span = Span {
                start: self.inner.span().start,
                end: self.inner.span().end,
            };

            match result {
                Ok(token) => {
                    tokens.push(TokenWithSpan { token, span });
                    current_pos = span.end;
                },
                Err(_) => {
                    // Try to recover from error
                    let remainder = self.inner.remainder();
                    let error = if remainder.starts_with("Decision.") {
                        LexerError::InvalidDecisionType {
                            position: current_pos,
                            decision: remainder[9..].split_whitespace().next()
                                .unwrap_or("").to_string(),
                        }
                    } else if remainder.starts_with("Actor") {
                        LexerError::InvalidBehaviorType {
                            position: current_pos,
                            behavior: remainder[5..].split_whitespace().next()
                                .unwrap_or("").to_string(),
                        }
                    } else if remainder.starts_with("Transaction") {
                        LexerError::InvalidTransactionState {
                            position: current_pos,
                            state: remainder[11..].split_whitespace().next()
                                .unwrap_or("").to_string(),
                        }
                    } else {
                        LexerError::InvalidCharacter {
                            position: current_pos,
                            character: remainder.chars().next().unwrap_or('\0'),
                        }
                    };

                    errors.push(error);

                    // Skip the invalid token
                    self.inner.bump(1);
                    current_pos += 1;
                }
            }
        }

        (tokens, errors)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn test_keywords() {
        let input = "let function class blockchain contract";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        assert_eq!(tokens, vec![
            TokenWithSpan { token: Token::Let, span: Span { start: 0, end: 3 } },
            TokenWithSpan { token: Token::Function, span: Span { start: 4, end: 11 } },
            TokenWithSpan { token: Token::Class, span: Span { start: 12, end: 17 } },
            TokenWithSpan { token: Token::Blockchain, span: Span { start: 18, end: 28 } },
            TokenWithSpan { token: Token::Contract, span: Span { start: 29, end: 36 } },
        ]);
    }

    #[test]
    fn test_literals() {
        let input = r#"42 3.14 "hello" true false null"#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        assert_eq!(tokens.iter().map(|t| &t.token).collect::<Vec<_>>(), vec![
            &Token::IntLiteral,
            &Token::FloatLiteral,
            &Token::StringLiteral,
            &Token::True,
            &Token::False,
            &Token::Null,
        ]);
    }

    #[test]
    fn test_operators() {
        let input = "+ - * / = == != < <= > >= && || !";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Create expected tokens with their spans
        let expected = vec![
            TokenWithSpan { token: Token::Plus, span: Span { start: 0, end: 1 } },
            TokenWithSpan { token: Token::Minus, span: Span { start: 2, end: 3 } },
            TokenWithSpan { token: Token::Multiply, span: Span { start: 4, end: 5 } },
            TokenWithSpan { token: Token::Divide, span: Span { start: 6, end: 7 } },
            TokenWithSpan { token: Token::Assign, span: Span { start: 8, end: 9 } },
            TokenWithSpan { token: Token::Equals, span: Span { start: 10, end: 12 } },
            TokenWithSpan { token: Token::NotEquals, span: Span { start: 13, end: 15 } },
            TokenWithSpan { token: Token::LessThan, span: Span { start: 16, end: 17 } },
            TokenWithSpan { token: Token::LessEquals, span: Span { start: 18, end: 20 } },
            TokenWithSpan { token: Token::GreaterThan, span: Span { start: 21, end: 22 } },
            TokenWithSpan { token: Token::GreaterEquals, span: Span { start: 23, end: 25 } },
            TokenWithSpan { token: Token::And, span: Span { start: 26, end: 28 } },
            TokenWithSpan { token: Token::Or, span: Span { start: 29, end: 31 } },
            TokenWithSpan { token: Token::Not, span: Span { start: 32, end: 33 } },
        ];
        
        assert_eq!(tokens, expected);
    }

    #[test]
    fn test_complex_code() {
        let input = r#"
            function calculateSum(a: int, b: int): int {
                let result = a + b;
                return result;
            }

            blockchain contract Token {
                let balance: map<string, int>;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify tokens contain expected sequence
        assert!(tokens.iter().any(|t| t.token == Token::Function));
        assert!(tokens.iter().any(|t| t.token == Token::Blockchain));
        assert!(tokens.iter().any(|t| t.token == Token::Contract));
        assert!(tokens.iter().any(|t| t.token == Token::Map));
    }

    #[test]
    fn test_blockchain_keywords() {
        let input = "blockchain contract ledger validate mine block hash";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 7);
        assert_eq!(tokens[0].token, Token::Blockchain);
        assert_eq!(tokens[1].token, Token::Contract);
        assert_eq!(tokens[2].token, Token::Ledger);
        assert_eq!(tokens[3].token, Token::Validate);
        assert_eq!(tokens[4].token, Token::Mine);
        assert_eq!(tokens[5].token, Token::Block);
        assert_eq!(tokens[6].token, Token::Hash);

        // Verify spans are correct
        assert_eq!(tokens[0].span.start, 0);
        assert_eq!(tokens[0].span.end, 10); // "blockchain"
        assert_eq!(tokens[1].span.start, 11);
        assert_eq!(tokens[1].span.end, 19); // "contract"
    }

    #[test]
    fn test_advanced_operators() {
        let input = "+= -= *= /= %= ++ -- ... ?. ??";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::PlusAssign);
        assert_eq!(tokens[1].token, Token::MinusAssign);
        assert_eq!(tokens[2].token, Token::MultiplyAssign);
        assert_eq!(tokens[3].token, Token::DivideAssign);
        assert_eq!(tokens[4].token, Token::ModuloAssign);
        assert_eq!(tokens[5].token, Token::Increment);
        assert_eq!(tokens[6].token, Token::Decrement);
        assert_eq!(tokens[7].token, Token::Spread);
        assert_eq!(tokens[8].token, Token::OptionalChain);
        assert_eq!(tokens[9].token, Token::NullCoalesce);
    }

    #[test]
    fn test_additional_literals() {
        let input = "'a' 0xFF 0b1010";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::CharLiteral);
        assert_eq!(tokens[1].token, Token::HexLiteral);
        assert_eq!(tokens[2].token, Token::BinaryLiteral);
    }

    #[test]
    fn test_error_handling() {
        let input = "let @ function";
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(tokens.len(), 2); // "let" and "function"
        assert_eq!(errors.len(), 1); // One error for "@"
    }

    #[test]
    fn test_span_tracking() {
        let input = "let x = 42";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].span.start, 0);  // "let" starts at 0
        assert_eq!(tokens[0].span.end, 3);    // "let" ends at 3
    }

    #[test]
    fn test_wasm_tokens() {
        let input = "@wasm @WasmExport @WasmImport @WasmMemory";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Wasm);
        assert_eq!(tokens[1].token, Token::WasmExport);
        assert_eq!(tokens[2].token, Token::WasmImport);
        assert_eq!(tokens[3].token, Token::WasmMemory);
    }

    #[test]
    fn test_decorators() {
        let input = "@event @modifier @scheduled";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Event);
        assert_eq!(tokens[1].token, Token::Modifier);
        assert_eq!(tokens[2].token, Token::Scheduled);
    }

    #[test]
    fn test_blockchain_specific() {
        let input = "msg.sender new sign mutex semaphore";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::MsgSender);
        assert_eq!(tokens[1].token, Token::New);
        assert_eq!(tokens[2].token, Token::Sign);
        assert_eq!(tokens[3].token, Token::Mutex);
        assert_eq!(tokens[4].token, Token::Semaphore);
    }

    #[test]
    fn test_smart_contract() {
        let input = r#"
            blockchain contract Token {
                @event
                public class Transfer {
                    public from: address;
                    public to: address;
                    public amount: uint;
                }

                @modifier
                private function onlyOwner(): void {
                    validate(msg.sender == owner, "Not authorized");
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify tokens contain expected sequence
        assert!(tokens.iter().any(|t| t.token == Token::Blockchain));
        assert!(tokens.iter().any(|t| t.token == Token::Contract));
        assert!(tokens.iter().any(|t| t.token == Token::Event));
        assert!(tokens.iter().any(|t| t.token == Token::Modifier));
        assert!(tokens.iter().any(|t| t.token == Token::MsgSender));
    }

    #[test]
    fn test_control_flow() {
        let input = "foreach item in items do while match case _";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Foreach);
        assert_eq!(tokens[1].token, Token::Identifier); // item
        assert_eq!(tokens[2].token, Token::In);
        assert_eq!(tokens[3].token, Token::Identifier); // items
        assert_eq!(tokens[4].token, Token::Do);
        assert_eq!(tokens[5].token, Token::While);
        assert_eq!(tokens[6].token, Token::Match);
        assert_eq!(tokens[7].token, Token::Case);
        assert_eq!(tokens[8].token, Token::Underscore);
    }

    #[test]
    fn test_blockchain_types() {
        let input = "address uint float double";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Address);
        assert_eq!(tokens[1].token, Token::UInt);
        assert_eq!(tokens[2].token, Token::Float);
        assert_eq!(tokens[3].token, Token::Double);
    }

    #[test]
    fn test_variable_declarations() {
        let input = "readonly MAX_SIZE: int = 100;";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Readonly);
        assert_eq!(tokens[1].token, Token::Identifier); // MAX_SIZE
        assert_eq!(tokens[2].token, Token::Colon);
        assert_eq!(tokens[3].token, Token::Int);
        assert_eq!(tokens[4].token, Token::Assign);
        assert_eq!(tokens[5].token, Token::IntLiteral); // 100
        assert_eq!(tokens[6].token, Token::Semicolon);
    }

    #[test]
    fn test_doc_comments() {
        let input = "/// Single line doc\n/** Multiline\ndoc */";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::DocComment);
        assert_eq!(tokens[1].token, Token::MultilineDocComment);
    }

    #[test]
    fn test_template_strings() {
        let input = "`User ${name} is ${age} years old`";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::TemplateString);
        assert!(tokens.iter().any(|t| t.token == Token::TemplateInterpolation));
    }

    #[test]
    fn test_method_calls() {
        let input = "barrier.await();";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Identifier); // barrier
        assert_eq!(tokens[1].token, Token::Dot);
        assert_eq!(tokens[2].token, Token::Identifier); // await
    }

    #[test]
    fn test_string_escapes() {
        let input = r#""Hello\nWorld\t\"Quote\"\\Backslash""#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::StringLiteral);
    }

    #[test]
    fn test_numeric_types() {
        let input = "int uint float double";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Int);
        assert_eq!(tokens[1].token, Token::UInt);
        assert_eq!(tokens[2].token, Token::Float);
        assert_eq!(tokens[3].token, Token::Double);
    }

    #[test]
    fn test_scientific_notation() {
        let input = "1.23e-4 5E10 -6.78E+9";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert!(tokens.iter().all(|t| matches!(t.token, Token::ScientificLiteral)));
    }

    #[test]
    fn test_smart_contract_keywords() {
        let input = "payable view pure emit constructor this super";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Payable);
        assert_eq!(tokens[1].token, Token::View);
        assert_eq!(tokens[2].token, Token::Pure);
        assert_eq!(tokens[3].token, Token::Emit);
        assert_eq!(tokens[4].token, Token::Constructor);
        assert_eq!(tokens[5].token, Token::This);
        assert_eq!(tokens[6].token, Token::Super);
    }

    #[test]
    fn test_concurrency_keywords() {
        let input = "spawn channel select task sync atomic";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens[0].token, Token::Spawn);
        assert_eq!(tokens[1].token, Token::Channel);
        assert_eq!(tokens[2].token, Token::Select);
        assert_eq!(tokens[3].token, Token::Task);
        assert_eq!(tokens[4].token, Token::Sync);
        assert_eq!(tokens[5].token, Token::Atomic);
    }

    #[test]
    fn test_all_operators() {
        let input = "+ - * / % == != < <= > >= && || ! ?? ?. += -= *= /= %= ++ --";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify each operator is correctly tokenized
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Plus)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Minus)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Multiply)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Divide)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Modulo)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Equals)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::NotEquals)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::LessThan)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::LessEquals)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::GreaterThan)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::GreaterEquals)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::And)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Or)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Not)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::NullCoalesce)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::OptionalChain)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::PlusAssign)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::MinusAssign)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::MultiplyAssign)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::DivideAssign)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::ModuloAssign)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Increment)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Decrement)));
    }

    #[test]
    fn test_module_system() {
        let input = "import { Component } from './component'; export class MyComponent";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        assert!(tokens.iter().any(|t| t.token == Token::Import));
        assert!(tokens.iter().any(|t| t.token == Token::Export));
        assert!(tokens.iter().any(|t| t.token == Token::From));
        assert!(tokens.iter().any(|t| t.token == Token::LeftBrace));
        assert!(tokens.iter().any(|t| t.token == Token::RightBrace));
        assert!(tokens.iter().any(|t| t.token == Token::Class));
    }

    #[test]
    fn test_actor_system() {
        let input = r#"
            class ActorSystem {
                public class Actor<T> {
                    private mailbox: MessageQueue<T>;
                    private behavior: ActorBehavior<T>;
                    
                    public async function receive(message: T): void {
                        await this.mailbox.enqueue(message);
                        await this.process();
                    }
                    
                    private async function process(): void {
                        while (true) {
                            let message = await this.mailbox.dequeue();
                            try {
                                await this.behavior.handle(message);
                            } catch (error: Error) {
                                await this.supervisor.handleError(error);
                            }
                        }
                    }
                    
                    public function become(behavior: ActorBehavior<T>): void {
                        this.behavior = behavior;
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify actor system tokens
        assert!(tokens.iter().any(|t| t.token == Token::Class));
        assert!(tokens.iter().any(|t| t.token == Token::Actor));
        assert!(tokens.iter().any(|t| t.token == Token::MessageQueue));
        assert!(tokens.iter().any(|t| t.token == Token::ActorBehavior));
        assert!(tokens.iter().any(|t| t.token == Token::Async));
        assert!(tokens.iter().any(|t| t.token == Token::Await));
    }

    #[test]
    fn test_supervision_strategy() {
        let input = r#"
            match this.strategy.decide(error) {
                Decision.RESTART => {
                    await this.restartActor(actor);
                },
                Decision.STOP => {
                    await this.stopActor(actor);
                },
                Decision.ESCALATE => {
                    await this.escalateError(error);
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify supervision tokens
        assert!(tokens.iter().any(|t| t.token == Token::Match));
        assert!(tokens.iter().any(|t| t.token == Token::DecisionRestart));
        assert!(tokens.iter().any(|t| t.token == Token::DecisionStop));
        assert!(tokens.iter().any(|t| t.token == Token::DecisionEscalate));
    }

    #[test]
    fn test_stm_features() {
        let input = r#"
            class STM {
                public class TVar<T> {
                    private value: T;
                    private version: int;
                    
                    public function read(transaction: Transaction): T {
                        transaction.track(this);
                        return this.value;
                    }
                    
                    public function write(transaction: Transaction, newValue: T): void {
                        transaction.modify(this, newValue);
                    }
                }
                
                public async function atomic<T>(action: function(): T): T {
                    while (true) {
                        let transaction = new Transaction();
                        try {
                            let result = await action();
                            if (await transaction.commit()) {
                                return result;
                            }
                        } catch (error: Error) {
                            await transaction.abort();
                            throw error;
                        }
                        await this.backoff();
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify STM tokens
        assert!(tokens.iter().any(|t| t.token == Token::Class));
        assert!(tokens.iter().any(|t| t.token == Token::Function));
        assert!(tokens.iter().any(|t| t.token == Token::Atomic));
        assert!(tokens.iter().any(|t| t.token == Token::Try));
        assert!(tokens.iter().any(|t| t.token == Token::Catch));
        assert!(tokens.iter().any(|t| t.token == Token::While));
    }

    #[test]
    fn test_actor_system_features() {
        let input = r#"
            class ActorSystem {
                public class Actor<T> {
                    private mailbox: MessageQueue<T>;
                    private behavior: ActorBehavior<T>;
                    
                    public async function receive(message: T): void {
                        await this.mailbox.enqueue(message);
                        await this.process();
                    }
                    
                    private async function process(): void {
                        while (true) {
                            let message = await this.mailbox.dequeue();
                            try {
                                await this.behavior.handle(message);
                            } catch (error: Error) {
                                await this.supervisor.handleError(error);
                            }
                        }
                    }
                    
                    public function become(behavior: ActorBehavior<T>): void {
                        this.behavior = behavior;
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify actor system tokens
        assert!(tokens.iter().any(|t| t.token == Token::Class));
        assert!(tokens.iter().any(|t| t.token == Token::Actor));
        assert!(tokens.iter().any(|t| t.token == Token::MessageQueue));
        assert!(tokens.iter().any(|t| t.token == Token::ActorBehavior));
        assert!(tokens.iter().any(|t| t.token == Token::Async));
        assert!(tokens.iter().any(|t| t.token == Token::Await));
        assert!(tokens.iter().any(|t| t.token == Token::Become));
    }

   

    #[test]
    fn test_unterminated_string() {
        let input = r#"let msg = "unterminated string;"#;
        let mut lexer = Lexer::new(input);
        let result = lexer.tokenize();
        
        assert!(matches!(
            result,
            Err(LexerError::UnterminatedString { position: _, partial: _ })
        ));
    }

    #[test]
    fn test_invalid_escape_sequence() {
        let input = r#"let msg = "invalid \z escape";"#;
        let mut lexer = Lexer::new(input);
        let result = lexer.tokenize();
        
        assert!(matches!(
            result,
            Err(LexerError::InvalidEscape { position: _, sequence: _ })
        ));
    }

    #[test]
    fn test_invalid_character() {
        let input = "let x = @;";  // @ is not a valid token
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(tokens.len(), 3); // let, x, =
        assert_eq!(errors.len(), 1); // one error for @
    }

    #[test]
    fn test_recovery_after_error() {
        let input = "let x = @; let y = 42;";
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert!(tokens.len() > 5); // Should continue lexing after error
        assert_eq!(errors.len(), 1); // One error for @
        
        // Verify it continues lexing correctly after error
        let valid_tokens: Vec<_> = tokens.iter()
            .skip_while(|t| t.token != Token::Let)
            .collect();
        assert!(!valid_tokens.is_empty());
    }

    #[test]
    fn test_multiple_errors() {
        let input = "let @ = #; let $ = %;";
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(errors.len(), 4); // @, #, $, %
        assert!(tokens.iter().any(|t| t.token == Token::Let));
    }

    #[test]
    fn test_unterminated_template_string() {
        let input = r#"let msg = `Hello ${name"#;
        let mut lexer = Lexer::new(input);
        let result = lexer.tokenize();
        
        assert!(matches!(
            result,
            Err(LexerError::UnterminatedString { position: _, partial: _ })
        ));
    }

    #[test]
    fn test_invalid_template_interpolation() {
        let input = r#"let msg = `Hello ${}`"#;
        let mut lexer = Lexer::new(input);
        let (_tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(errors.len(), 1);
    }

    #[test]
    fn test_invalid_number_format() {
        let input = "let x = 42.42.42;";
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(errors.len(), 1);
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Let)));
    }

    #[test]
    fn test_invalid_hex_literal() {
        let input = "let x = 0xGH;";
        let mut lexer = Lexer::new(input);
        let (_tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(errors.len(), 1);
    }

    #[test]
    fn test_invalid_binary_literal() {
        let input = "let x = 0b102;";
        let mut lexer = Lexer::new(input);
        let (_tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(errors.len(), 1);
    }

    #[test]
    fn test_unterminated_comment() {
        let input = "let x = 42; /* unterminated comment";
        let mut lexer = Lexer::new(input);
        let result = lexer.tokenize();
        
        assert!(matches!(
            result,
            Err(LexerError::UnterminatedString { position: _, partial: _ })
        ));
    }

    #[test]
    fn test_nested_comments() {
        let input = "/* outer /* inner */ comment */";
        let mut lexer = Lexer::new(input);
        let result = lexer.tokenize();
        
        assert!(result.is_ok());
        assert_eq!(result.unwrap().len(), 0); // All comments should be skipped
    }

    #[test]
    fn test_invalid_character_in_identifier() {
        let input = "let my@var = 42;";
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert_eq!(errors.len(), 1);
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Let)));
    }

    #[test]
    fn test_recovery_from_multiple_errors() {
        let input = "let @ = #; if $ then % else ^;";
        let mut lexer = Lexer::new(input);
        let (tokens, errors) = lexer.tokenize_with_errors();
        
        assert!(errors.len() > 1);
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Let)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::If)));
        assert!(tokens.iter().any(|t| matches!(t.token, Token::Else)));
    }

    #[test]
    fn test_error_positions() {
        let input = "let x = @; let y = #;";
        let mut lexer = Lexer::new(input);
        let (_, errors) = lexer.tokenize_with_errors();
        
        // Verify error positions are in ascending order
        let positions: Vec<_> = errors.iter()
            .map(|e| match e {
                LexerError::InvalidToken { position, .. } => *position,
                _ => panic!("Unexpected error type"),
            })
            .collect();
        
        assert!(positions.windows(2).all(|w| w[0] < w[1]));
    }

    #[test]
    fn test_actor_system_complete() {
        let input = r#"
            class ActorSystem {
                public class Actor<T> {
                    private mailbox: MessageQueue<T>;
                    private behavior: ActorBehavior<T>;
                    
                    public async function receive(message: T): void {
                        await this.mailbox.enqueue(message);
                        await this.process();
                    }
                    
                    private async function process(): void {
                        while (true) {
                            let message = await this.mailbox.dequeue();
                            try {
                                await this.behavior.handle(message);
                            } catch (error: Error) {
                                await this.supervisor.handleError(error);
                            }
                        }
                    }
                    
                    public function become(behavior: ActorBehavior<T>): void {
                        this.behavior = behavior;
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify all actor system tokens
        assert!(tokens.iter().any(|t| t.token == Token::Class));
        assert!(tokens.iter().any(|t| t.token == Token::Public));
        assert!(tokens.iter().any(|t| t.token == Token::Private));
        assert!(tokens.iter().any(|t| t.token == Token::Async));
        assert!(tokens.iter().any(|t| t.token == Token::Function));
        assert!(tokens.iter().any(|t| t.token == Token::Try));
        assert!(tokens.iter().any(|t| t.token == Token::Catch));
        assert!(tokens.iter().any(|t| t.token == Token::While));
        assert!(tokens.iter().any(|t| t.token == Token::True));
    }

    #[test]
    fn test_supervision_strategy_complete() {
        let input = r#"
            public class Supervisor {
                private children: map<string, Actor>;
                private strategy: SupervisionStrategy;
                
                public async function handleError(error: Error, actor: Actor): void {
                    match this.strategy.decide(error) {
                        Decision.RESTART => {
                            await this.restartActor(actor);
                        },
                        Decision.STOP => {
                            await this.stopActor(actor);
                        },
                        Decision.ESCALATE => {
                            await this.escalateError(error);
                        }
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify supervision tokens
        assert!(tokens.iter().any(|t| t.token == Token::Class));
        assert!(tokens.iter().any(|t| t.token == Token::Supervisor));
        assert!(tokens.iter().any(|t| t.token == Token::Match));
        assert!(tokens.iter().any(|t| t.token == Token::DecisionRestart));
        assert!(tokens.iter().any(|t| t.token == Token::DecisionStop));
        assert!(tokens.iter().any(|t| t.token == Token::DecisionEscalate));
        assert!(tokens.iter().any(|t| t.token == Token::Arrow));
    }

    #[test]
    fn test_stm_complete() {
        let input = r#"
            class STM {
                public class TVar<T> {
                    private value: T;
                    private version: int;
                    
                    public function read(transaction: Transaction): T {
                        transaction.track(this);
                        return this.value;
                    }
                    
                    public function write(transaction: Transaction, newValue: T): void {
                        transaction.modify(this, newValue);
                    }
                }
                
                public async function atomic<T>(action: function(): T): T {
                    while (true) {
                        let transaction = new Transaction();
                        try {
                            let result = await action();
                            if (await transaction.commit()) {
                                return result;
                            }
                        } catch (error: Error) {
                            await transaction.abort();
                            throw error;
                        }
                        await this.backoff();
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Verify STM tokens
        assert!(tokens.iter().any(|t| t.token == Token::Class));
        assert!(tokens.iter().any(|t| t.token == Token::TVar));
        assert!(tokens.iter().any(|t| t.token == Token::Atomic));
        assert!(tokens.iter().any(|t| t.token == Token::Transaction));
        assert!(tokens.iter().any(|t| t.token == Token::Commit));
        assert!(tokens.iter().any(|t| t.token == Token::Abort));
        assert!(tokens.iter().any(|t| t.token == Token::Backoff));
        assert!(tokens.iter().any(|t| t.token == Token::Try));
        assert!(tokens.iter().any(|t| t.token == Token::Catch));
        assert!(tokens.iter().any(|t| t.token == Token::Throw));
    }
} 