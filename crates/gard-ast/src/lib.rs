use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Node {
    // Top-level declarations
    Program(Vec<Node>),
    
    // Class and Contract declarations
    Class {
        name: String,
        extends: Option<String>,
        implements: Vec<String>,
        members: Vec<Node>,
    },
    Contract {
        name: String,
        members: Vec<Node>,
    },

    // Function declarations
    Function {
        name: String,
        params: Vec<Parameter>,
        return_type: Type,
        body: Box<Node>,
        modifiers: Vec<FunctionModifier>,
    },
    Constructor {
        params: Vec<Parameter>,
        body: Box<Node>,
    },

    // Statements
    Block(Vec<Node>),
    Let {
        name: String,
        type_annotation: Option<Type>,
        initializer: Option<Box<Node>>,
        is_mutable: bool,
    },
    If {
        condition: Box<Node>,
        then_branch: Box<Node>,
        else_branch: Option<Box<Node>>,
    },
    While {
        condition: Box<Node>,
        body: Box<Node>,
    },
    For {
        initializer: Option<Box<Node>>,
        condition: Option<Box<Node>>,
        increment: Option<Box<Node>>,
        body: Box<Node>,
    },
    Foreach {
        item: String,
        collection: Box<Node>,
        body: Box<Node>,
    },
    Match {
        value: Box<Node>,
        cases: Vec<MatchCase>,
    },
    Return(Option<Box<Node>>),
    Throw(Box<Node>),
    Try {
        body: Box<Node>,
        catch_clauses: Vec<Node>,
        finally: Option<Box<Node>>,
    },

    // Expressions
    Binary {
        left: Box<Node>,
        operator: BinaryOp,
        right: Box<Node>,
    },
    Unary {
        operator: UnaryOp,
        operand: Box<Node>,
    },
    Call {
        callee: Box<Node>,
        arguments: Vec<Node>,
    },
    Member {
        object: Box<Node>,
        property: String,
    },
    Array {
        elements: Vec<Node>,
    },
    Map {
        entries: Vec<(Node, Node)>,
    },
    Await(Box<Node>),
    
    // Literals and Identifiers
    Identifier(String),
    IntLiteral(i64),
    UIntLiteral(u64),
    FloatLiteral(f64),
    StringLiteral(String),
    BooleanLiteral(bool),
    NullLiteral,
    This,
    Super,

    // Blockchain specific
    Transaction {
        from: Box<Node>,
        to: Box<Node>,
        amount: Box<Node>,
    },
    Event {
        name: String,
        fields: Vec<Parameter>,
    },

    // Actor System
    Actor {
        name: String,
        type_param: Option<Type>,
        mailbox: Box<Node>,
        behavior: Box<Node>,
        members: Vec<Node>,
    },
    Behavior {
        name: String,
        handlers: Vec<Node>,
    },
    Receive {
        message_param: Parameter,
        body: Box<Node>,
    },
    Become {
        behavior: Box<Node>,
    },
    Supervise {
        strategy: SupervisionStrategy,
        children: Vec<Node>,
    },

    // STM (Software Transactional Memory)
    STMTransaction {
        variables: Vec<Node>,
        operations: Vec<Node>,
    },
    TVar {
        name: String,
        value_type: Type,
        initial_value: Option<Box<Node>>,
    },
    Atomic {
        body: Box<Node>,
    },
    CatchClause {
        param_name: String,
        param_type: Type,
        body: Box<Node>,
    },
    DoWhile {
        body: Box<Node>,
        condition: Box<Node>,
    },
    Break,
    Continue,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Parameter {
    pub name: String,
    pub type_annotation: Type,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Type {
    Int,
    UInt,
    Float,
    Double,
    String,
    Boolean,
    Void,
    Array(Box<Type>),
    Map { key: Box<Type>, value: Box<Type> },
    Set(Box<Type>),
    Address,
    Custom(String),
    Function {
        params: Vec<Type>,
        return_type: Box<Type>,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum BinaryOp {
    Add, Sub, Mul, Div, Mod,
    Eq, NotEq, Lt, LtEq, Gt, GtEq,
    And, Or,
    NullCoalesce,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum UnaryOp {
    Minus,
    Not,
    Increment,
    Decrement,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MatchCase {
    pub pattern: Node,
    pub body: Node,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CatchClause {
    pub error_type: Type,
    pub binding: String,
    pub body: Node,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum FunctionModifier {
    Public,
    Private,
    Protected,
    Static,
    Async,
    View,
    Pure,
    Payable,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum SupervisionStrategy {
    OneForOne,
    OneForAll,
    RestForOne,
    Custom(String),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum ParseError {
    UnexpectedToken {
        expected: Vec<String>,
        found: String,
        span: Span,
    },
    UnexpectedEof {
        expected: Vec<String>,
        span: Span,
    },
    Custom {
        message: String,
        span: Span,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ParseError::UnexpectedToken { expected, found, span } => {
                write!(f, "Unexpected token '{}' at position {}-{}, expected one of: {}", 
                    found, span.start, span.end, 
                    expected.join(", "))
            },
            ParseError::UnexpectedEof { expected, span } => {
                write!(f, "Unexpected end of file at position {}-{}, expected one of: {}", 
                    span.start, span.end,
                    expected.join(", "))
            },
            ParseError::Custom { message, span } => {
                write!(f, "{} at position {}-{}", message, span.start, span.end)
            },
        }
    }
} 