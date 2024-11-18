use chumsky::prelude::*;
use chumsky::Parser;
use gard_ast::{
    Node, Type, BinaryOp, UnaryOp, Parameter,
    SupervisionStrategy, MatchCase
};
use gard_lexer::{Token, TokenWithSpan};

pub trait GardParserTrait {
    fn parse(tokens: Vec<TokenWithSpan>) -> Result<Node, Vec<Simple<TokenWithSpan>>>;
}

pub struct GardParser;

impl GardParserTrait for GardParser {
   fn parse(tokens: Vec<TokenWithSpan>) -> Result<Node, Vec<Simple<TokenWithSpan>>> {
        let parser = Self::program();
        parser.parse(tokens)
    }
}

impl GardParser {
    fn program() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        recursive(|_| {
            Self::declaration()
                .repeated()
                .map(Node::Program)
        }).boxed()
    }

    fn declaration() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        choice((
            Self::class_declaration(),
            Self::function_declaration(),
            Self::contract_declaration(),
        )).boxed()
    }

    fn class_declaration() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Class, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::Extends, .. } => () }
                    .ignore_then(Self::identifier())
                    .or_not()
            )
            .then(
                select! { TokenWithSpan { token: Token::Implements, .. } => () }
                    .ignore_then(Self::identifier())
                    .separated_by(select! { TokenWithSpan { token: Token::Comma, .. } => () })
                    .or_not()
            )
            .then(Self::block())
            .map(|(((name, extends), implements), body)| Node::Class {
                name,
                extends,
                implements: implements.unwrap_or_default(),
                members: if let Node::Block(members) = body {
                    members
                } else {
                    vec![]
                }
            })
            .boxed()
    }

    fn identifier() -> impl chumsky::Parser<TokenWithSpan, String, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Identifier, .. } => "identifier".to_string() }
            .boxed()
    }

    fn block() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::LeftBrace, .. } => () }
            .ignore_then(Self::statement().repeated())
            .then_ignore(select! { TokenWithSpan { token: Token::RightBrace, .. } => () })
            .map(Node::Block)
            .boxed()
    }

    fn statement() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        choice((
            Self::let_statement(),
            Self::expression_statement(),
        )).boxed()
    }

    fn let_statement() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Let, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::Colon, .. } => () }
                    .ignore_then(Self::type_annotation())
                    .or_not()
            )
            .then(
                select! { TokenWithSpan { token: Token::Assign, .. } => () }
                    .ignore_then(Self::expression())
                    .or_not()
            )
            .map(|((name, type_annotation), initializer)| Node::Let {
                name,
                type_annotation,
                initializer: initializer.map(Box::new),
                is_mutable: false,
            })
    }

    fn expression() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        recursive(|expr| {
            let atom = choice((
                Self::identifier().map(Node::Identifier),
                select! { TokenWithSpan { token: Token::IntLiteral, .. } => () }
                    .map(|_| Node::IntLiteral(0)),
                select! { TokenWithSpan { token: Token::StringLiteral, .. } => () }
                    .map(|_| Node::StringLiteral("".to_string())),
                select! { TokenWithSpan { token: Token::True, .. } => () }
                    .map(|_| Node::BooleanLiteral(true)),
                select! { TokenWithSpan { token: Token::False, .. } => () }
                    .map(|_| Node::BooleanLiteral(false)),
                select! { TokenWithSpan { token: Token::Null, .. } => () }
                    .map(|_| Node::NullLiteral),
                select! { TokenWithSpan { token: Token::This, .. } => () }
                    .map(|_| Node::This),
                select! { TokenWithSpan { token: Token::Super, .. } => () }
                    .map(|_| Node::Super),
                select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                    .ignore_then(expr.clone())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () }),
            ))
            .boxed();

            let member = atom.clone()
                .then(
                    select! { TokenWithSpan { token: Token::Dot, .. } => () }
                        .ignore_then(Self::identifier())
                        .repeated()
                )
                .map(|(obj, props)| {
                    props.into_iter().fold(obj, |obj, prop| Node::Member {
                        object: Box::new(obj),
                        property: prop,
                    })
                })
                .boxed();

            let call = member.clone()
                .then(
                    select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                        .ignore_then(expr.clone()
                            .separated_by(select! { TokenWithSpan { token: Token::Comma, .. } => () }))
                        .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
                        .or_not()
                )
                .map(|(callee, args)| match args {
                    Some(args) => Node::Call {
                        callee: Box::new(callee),
                        arguments: args,
                    },
                    None => callee,
                })
                .boxed();

            let unary = choice((
                select! { TokenWithSpan { token: Token::Not, .. } => UnaryOp::Not },
                select! { TokenWithSpan { token: Token::Minus, .. } => UnaryOp::Minus },
                select! { TokenWithSpan { token: Token::Increment, .. } => UnaryOp::Increment },
                select! { TokenWithSpan { token: Token::Decrement, .. } => UnaryOp::Decrement },
            ))
            .then(call.clone())
            .map(|(op, expr)| Node::Unary {
                operator: op,
                operand: Box::new(expr),
            })
            .or(call)
            .boxed();

            let product = unary.clone()
                .then(
                    choice((
                        select! { TokenWithSpan { token: Token::Multiply, .. } => BinaryOp::Mul },
                        select! { TokenWithSpan { token: Token::Divide, .. } => BinaryOp::Div },
                        select! { TokenWithSpan { token: Token::Modulo, .. } => BinaryOp::Mod },
                    ))
                    .then(unary)
                    .repeated()
                )
                .map(|(first, rest)| {
                    rest.into_iter().fold(first, |lhs, (op, rhs)| Node::Binary {
                        left: Box::new(lhs),
                        operator: op,
                        right: Box::new(rhs),
                    })
                })
                .boxed();

            let sum = product.clone()
                .then(
                    choice((
                        select! { TokenWithSpan { token: Token::Plus, .. } => BinaryOp::Add },
                        select! { TokenWithSpan { token: Token::Minus, .. } => BinaryOp::Sub },
                    ))
                    .then(product)
                    .repeated()
                )
                .map(|(first, rest)| {
                    rest.into_iter().fold(first, |lhs, (op, rhs)| Node::Binary {
                        left: Box::new(lhs),
                        operator: op,
                        right: Box::new(rhs),
                    })
                })
                .boxed();

            let comparison = sum.clone()
                .then(
                    choice((
                        select! { TokenWithSpan { token: Token::Equals, .. } => BinaryOp::Eq },
                        select! { TokenWithSpan { token: Token::NotEquals, .. } => BinaryOp::NotEq },
                        select! { TokenWithSpan { token: Token::LessThan, .. } => BinaryOp::Lt },
                        select! { TokenWithSpan { token: Token::LessEquals, .. } => BinaryOp::LtEq },
                        select! { TokenWithSpan { token: Token::GreaterThan, .. } => BinaryOp::Gt },
                        select! { TokenWithSpan { token: Token::GreaterEquals, .. } => BinaryOp::GtEq },
                    ))
                    .then(sum)
                    .repeated()
                )
                .map(|(first, rest)| {
                    rest.into_iter().fold(first, |lhs, (op, rhs)| Node::Binary {
                        left: Box::new(lhs),
                        operator: op,
                        right: Box::new(rhs),
                    })
                })
                .boxed();

            let logical = comparison.clone()
                .then(
                    choice((
                        select! { TokenWithSpan { token: Token::And, .. } => BinaryOp::And },
                        select! { TokenWithSpan { token: Token::Or, .. } => BinaryOp::Or },
                    ))
                    .then(comparison)
                    .repeated()
                )
                .map(|(first, rest)| {
                    rest.into_iter().fold(first, |lhs, (op, rhs)| Node::Binary {
                        left: Box::new(lhs),
                        operator: op,
                        right: Box::new(rhs),
                    })
                })
                .boxed();

            logical
        }).boxed()
    }

    fn type_annotation() -> impl chumsky::Parser<TokenWithSpan, Type, Error = Simple<TokenWithSpan>> {
        Self::identifier().map(Type::Custom)
    }

    fn expression_statement() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        Self::expression()
            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            .map(|expr| Node::Block(vec![expr]))
    }

    fn contract_declaration() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Contract, .. } => () }
            .then(Self::identifier())
            .then(Self::block())
            .map(|((_, name), body)| Node::Contract {
                name,
                members: if let Node::Block(members) = body {
                    members
                } else {
                    vec![]
                },
            })
    }

    fn function_declaration() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Function, .. } => () }
            .then(Self::identifier())
            .then(Self::block())
            .map(|((_, name), body)| Node::Function {
                name,
                params: vec![],
                return_type: Type::Void,
                body: Box::new(body),
                modifiers: vec![],
            })
    }

    fn try_statement() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Try, .. } => () }
            .ignore_then(Self::block())
            .then(
                select! { TokenWithSpan { token: Token::Catch, .. } => () }
                    .ignore_then(Self::identifier())
                    .then(
                        select! { TokenWithSpan { token: Token::Colon, .. } => () }
                            .ignore_then(Self::type_annotation())
                    )
                    .then(Self::block())
                    .map(|((param_name, param_type), body)| Node::CatchClause {
                        param_name,
                        param_type,
                        body: Box::new(body),
                    })
                    .repeated()
            )
            .then(
                select! { TokenWithSpan { token: Token::Finally, .. } => () }
                    .ignore_then(Self::block())
                    .or_not()
            )
            .map(|((try_block, catch_clauses), finally)| Node::Try {
                body: Box::new(try_block),
                catch_clauses,
                finally: finally.map(Box::new),
            })
    }

    fn if_statement() -> impl chumsky::Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        recursive(|if_stmt| {
            select! { TokenWithSpan { token: Token::If, .. } => () }
                .ignore_then(
                    select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                        .ignore_then(Self::expression())
                        .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
                )
                .then(Self::block())
                .then(
                    select! { TokenWithSpan { token: Token::Else, .. } => () }
                        .ignore_then(
                            Self::block()
                                .or(if_stmt)
                        )
                        .or_not()
                )
                .map(|((condition, then_branch), else_branch)| Node::If {
                    condition: Box::new(condition),
                    then_branch: Box::new(then_branch),
                    else_branch: else_branch.map(Box::new),
                })
        }).boxed()
    }

    fn while_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::While, .. } => () }
            .ignore_then(
                select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                    .ignore_then(Self::expression())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
            )
            .then(Self::block())
            .map(|(condition, body)| Node::While {
                condition: Box::new(condition),
                body: Box::new(body),
            })
            .boxed()
    }

    fn for_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::For, .. } => () }
            .ignore_then(
                select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                    .ignore_then(
                        Self::let_statement()
                            .or(Self::expression_statement())
                            .or_not()
                            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
                            .then(Self::expression().or_not())
                            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
                            .then(Self::expression().or_not())
                    )
                    .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
            )
            .then(Self::block())
            .map(|(((init, cond), inc), body)| Node::For {
                initializer: init.map(Box::new),
                condition: cond.map(Box::new),
                increment: inc.map(Box::new),
                body: Box::new(body),
            })
            .boxed()
    }

    fn foreach_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Foreach, .. } => () }
            .ignore_then(
                select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                    .ignore_then(Self::identifier())
                    .then_ignore(select! { TokenWithSpan { token: Token::In, .. } => () })
                    .then(Self::expression())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
            )
            .then(Self::block())
            .map(|((item, collection), body)| Node::Foreach {
                item,
                collection: Box::new(collection),
                body: Box::new(body),
            })
            .boxed()
    }

    fn match_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Match, .. } => () }
            .ignore_then(Self::expression())
            .then(
                select! { TokenWithSpan { token: Token::LeftBrace, .. } => () }
                    .ignore_then(Self::match_case().repeated())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightBrace, .. } => () })
            )
            .map(|(value, cases)| Node::Match {
                value: Box::new(value),
                cases,
            })
            .boxed()
    }

    fn match_case() -> impl Parser<TokenWithSpan, MatchCase, Error = Simple<TokenWithSpan>> {
        Self::expression()
            .then_ignore(select! { TokenWithSpan { token: Token::Arrow, .. } => () })
            .then(Self::block())
            .map(|(pattern, body)| MatchCase {
                pattern,
                body,
            })
            .boxed()
    }

    fn return_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Return, .. } => () }
            .ignore_then(
                Self::expression()
                    .or_not()
                    .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            )
            .map(|expr| Node::Return(expr.map(Box::new)))
            .boxed()
    }

    fn throw_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Throw, .. } => () }
            .ignore_then(
                Self::expression()
                    .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            )
            .map(|expr| Node::Throw(Box::new(expr)))
            .boxed()
    }

    fn do_while_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Do, .. } => () }
            .ignore_then(Self::block())
            .then_ignore(select! { TokenWithSpan { token: Token::While, .. } => () })
            .then(
                select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                    .ignore_then(Self::expression())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
            )
            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            .map(|(body, condition)| Node::DoWhile {
                body: Box::new(body),
                condition: Box::new(condition),
            })
            .boxed()
    }

    fn break_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Break, .. } => () }
            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            .map(|_| Node::Break)
            .boxed()
    }

    fn continue_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Continue, .. } => () }
            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            .map(|_| Node::Continue)
            .boxed()
    }

    fn actor_system_declaration() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Actor, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::LessThan, .. } => () }
                    .ignore_then(Self::identifier())
                    .then_ignore(select! { TokenWithSpan { token: Token::GreaterThan, .. } => () })
                    .map(|type_param| Type::Custom(type_param))
                    .or_not()
            )
            .then(Self::block())
            .map(|((name, type_param), body)| Node::Actor {
                name,
                type_param,
                mailbox: Box::new(Node::Identifier("MessageQueue".to_string())),
                behavior: Box::new(Node::Identifier("ActorBehavior".to_string())),
                members: if let Node::Block(members) = body {
                    members
                } else {
                    vec![]
                },
            })
            .boxed()
    }

    fn stm_declaration() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::TVar, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::LessThan, .. } => () }
                    .ignore_then(Self::identifier())
                    .then_ignore(select! { TokenWithSpan { token: Token::GreaterThan, .. } => () })
                    .map(|type_param| Type::Custom(type_param))
            )
            .then(
                select! { TokenWithSpan { token: Token::Assign, .. } => () }
                    .ignore_then(Self::expression())
                    .or_not()
            )
            .map(|((name, value_type), initial_value)| Node::TVar {
                name,
                value_type,
                initial_value: initial_value.map(Box::new),
            })
            .boxed()
    }

    fn atomic_block() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Atomic, .. } => () }
            .ignore_then(Self::block())
            .map(|body| Node::Atomic {
                body: Box::new(body),
            })
            .boxed()
    }

    fn actor_declaration() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Actor, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::LessThan, .. } => () }
                    .ignore_then(Self::identifier())
                    .then_ignore(select! { TokenWithSpan { token: Token::GreaterThan, .. } => () })
                    .map(|type_param| Type::Custom(type_param))
                    .or_not()
            )
            .then(Self::block())
            .map(|((name, type_param), body)| Node::Actor {
                name,
                type_param,
                mailbox: Box::new(Node::Identifier("MessageQueue".to_string())),
                behavior: Box::new(Node::Identifier("ActorBehavior".to_string())),
                members: if let Node::Block(members) = body {
                    members
                } else {
                    vec![]
                },
            })
            .boxed()
    }

    fn message_handler() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Function, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::LeftParen, .. } => () }
                    .ignore_then(Self::parameter())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightParen, .. } => () })
            )
            .then(Self::block())
            .map(|((name, param), body)| Node::Receive {
                message_param: param,
                body: Box::new(body),
            })
            .boxed()
    }

    fn become_statement() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Become, .. } => () }
            .ignore_then(Self::expression())
            .then_ignore(select! { TokenWithSpan { token: Token::Semicolon, .. } => () })
            .map(|behavior| Node::Become {
                behavior: Box::new(behavior),
            })
            .boxed()
    }

    fn supervision_strategy() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::SupervisionStrategy, .. } => () }
            .ignore_then(
                choice((
                    select! { TokenWithSpan { token: Token::DecisionRestart, .. } => SupervisionStrategy::OneForOne },
                    select! { TokenWithSpan { token: Token::DecisionStop, .. } => SupervisionStrategy::OneForAll },
                    select! { TokenWithSpan { token: Token::DecisionEscalate, .. } => SupervisionStrategy::RestForOne },
                    Self::identifier().map(SupervisionStrategy::Custom),
                ))
            )
            .then(Self::block())
            .map(|(strategy, body)| Node::Supervise {
                strategy,
                children: if let Node::Block(children) = body {
                    children
                } else {
                    vec![]
                },
            })
            .boxed()
    }

    fn blockchain_contract_basic() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Blockchain, .. } => () }
            .ignore_then(select! { TokenWithSpan { token: Token::Contract, .. } => () })
            .ignore_then(Self::identifier())
            .then(Self::block())
            .map(|(name, body)| Node::Contract {
                name,
                members: if let Node::Block(members) = body {
                    members
                } else {
                    vec![]
                },
            })
            .boxed()
    }

    fn event_declaration() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Event, .. } => () }
            .ignore_then(Self::identifier())
            .then(
                select! { TokenWithSpan { token: Token::LeftBrace, .. } => () }
                    .ignore_then(Self::parameter().repeated())
                    .then_ignore(select! { TokenWithSpan { token: Token::RightBrace, .. } => () })
            )
            .map(|(name, fields)| Node::Event {
                name,
                fields,
            })
            .boxed()
    }

    fn transaction_declaration() -> impl Parser<TokenWithSpan, Node, Error = Simple<TokenWithSpan>> {
        select! { TokenWithSpan { token: Token::Transaction, .. } => () }
            .ignore_then(
                select! { TokenWithSpan { token: Token::LeftBrace, .. } => () }
                    .ignore_then(
                        Self::expression()
                            .then_ignore(select! { TokenWithSpan { token: Token::Arrow, .. } => () })
                            .then(Self::expression())
                            .then_ignore(select! { TokenWithSpan { token: Token::Colon, .. } => () })
                            .then(Self::expression())
                    )
                    .then_ignore(select! { TokenWithSpan { token: Token::RightBrace, .. } => () })
            )
            .map(|((from, to), amount)| Node::Transaction {
                from: Box::new(from),
                to: Box::new(to),
                amount: Box::new(amount),
            })
            .boxed()
    }

    fn parameter() -> impl Parser<TokenWithSpan, Parameter, Error = Simple<TokenWithSpan>> {
        Self::identifier()
            .then_ignore(select! { TokenWithSpan { token: Token::Colon, .. } => () })
            .then(Self::type_annotation())
            .map(|(name, type_annotation)| Parameter {
                name,
                type_annotation,
            })
            .boxed()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gard_lexer::Lexer;

    #[test]
    fn test_basic_class() {
        let input = r#"
            class Test {
                let x: int;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_error_recovery() {
        let input = r#"
            class Test {
                let x: @invalid_type;
                function test(): void {
                    return 42;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_do_while() {
        let input = r#"
            do {
                print("Hello");
            } while (x > 0);
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_break_continue() {
        let input = r#"
            while (true) {
                if (x > 10) break;
                if (x < 0) continue;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_class_with_error_recovery() {
        let input = r#"
            class Test {
                let x: @invalid_type;  // Invalid type
                function test(): void {
                    return 42;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_function_with_error_recovery() {
        let input = r#"
            function test(x: int, @invalid, y: string): void {
                let z = x + y;  // Type error but should parse
                return;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_statement_error_recovery() {
        let input = r#"
            function test(): void {
                let x = 42;
                @invalid_statement;  // Invalid statement
                let y = 10;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_expression_error_recovery() {
        let input = r#"
            function test(): void {
                let x = 1 + @invalid + 2;  // Invalid expression
                let y = x * 2;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_multiple_errors_recovery() {
        let input = r#"
            class Test {
                @invalid_field;
                function test(): @invalid_type {
                    let x = @invalid_expr;
                    return @invalid_return;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_complex_class() {
        let input = r#"
            class Test {
                let x: int = 42;
                
                function test(a: int, b: string): void {
                    if (a > 0) {
                        print(b);
                    } else {
                        return;
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_nested_blocks() {
        let input = r#"
            function test(): void {
                {
                    {
                        let x = 42;
                    }
                    let y = x;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_error_recovery_nested() {
        let input = r#"
            class Test {
                function test(): void {
                    let x = @invalid;
                    if (x > 0) {
                        let y = @another_invalid;
                        print(y);
                    }
                    return x;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_multiple_classes() {
        let input = r#"
            class A {
                function foo(): void {}
            }
            class B extends A {
                function bar(): void {}
            }
            class C implements B {
                function baz(): void {}
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_error_recovery_with_sync_points() {
        let input = r#"
            class Test {
                let x = @invalid_expr;  // Error but should recover at semicolon
                function test(): void {
                    let y = #another_invalid;  // Error but should recover
                    if (true) {
                        let z = $more_invalid  // Error but should recover
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_nested_error_recovery() {
        let input = r#"
            class Test {
                function test(): void {
                    if (@invalid) {  // Error in condition
                        while (#invalid) {  // Error in condition
                            let x = $invalid;  // Error in initializer
                        }
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_recovery_in_expressions() {
        let input = r#"
            function test(): void {
                let x = 1 + @invalid + 2 * #invalid + 3;
                let y = x + $invalid;
                return x + y;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_recovery_in_declarations() {
        let input = r#"
            class @Invalid {  // Should recover
                let x: @Invalid;  // Should recover
                function @Invalid(): void {  // Should recover
                    return;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_blockchain_contract_basic() {
        let input = r#"
            blockchain contract Token {
                @event
                public class Transfer {
                    public from: address;
                    public to: address;
                    public amount: uint;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_event_declaration() {
        let input = r#"
            @event
            class Transfer {
                from: address;
                to: address;
                amount: uint;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_transaction() {
        let input = r#"
            transaction {
                sender -> receiver: 100
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
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
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_stm_transaction_block() {
        let input = r#"
            atomic {
                let balance = account.balance;
                if (balance >= amount) {
                    account.balance -= amount;
                    recipient.balance += amount;
                    return true;
                }
                return false;
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_supervision_strategy() {
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
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_blockchain_contract() {
        let input = r#"
            blockchain contract Token {
                @event
                public class Transfer {
                    public from: address;
                    public to: address;
                    public amount: uint;
                }

                public function transfer(to: address, amount: uint): void {
                    if (this.balances[msg.sender] < amount) {
                        throw "Insufficient balance";
                    }
                    
                    this.balances[msg.sender] -= amount;
                    this.balances[to] += amount;
                    
                    emit Transfer(msg.sender, to, amount);
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
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
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
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
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_actor_message_handling() {
        let input = r#"
            class UserActor extends Actor<UserMessage> {
                private state: UserState;

                public function receive(msg: UserMessage): void {
                    match msg {
                        UpdateProfile(profile) => {
                            this.state.profile = profile;
                            become(new ActiveState(this.state));
                        },
                        Logout => {
                            this.state.clear();
                            become(new InactiveState());
                        }
                    }
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_supervision_hierarchy() {
        let input = r#"
            class RootSupervisor extends Supervisor {
                public function supervise(): void {
                    spawn(new WorkerActor())
                        .withStrategy(SupervisionStrategy.OneForOne)
                        .withBackoff(exponential(1000))
                        .withMaxRetries(3);
                    
                    spawn(new CriticalActor())
                        .withStrategy(SupervisionStrategy.OneForAll)
                        .withBackoff(constant(500))
                        .withMaxRetries(5);
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_blockchain_events() {
        let input = r#"
            contract TokenContract {
                @event
                class Transfer {
                    from: address;
                    to: address;
                    amount: uint;
                }

                @event
                class Approval {
                    owner: address;
                    spender: address;
                    amount: uint;
                }

                public function transfer(to: address, amount: uint): boolean {
                    if (this._transfer(msg.sender, to, amount)) {
                        emit Transfer(msg.sender, to, amount);
                        return true;
                    }
                    return false;
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_complex_type_annotations() {
        let input = r#"
            class DataStructures {
                let simpleMap: map<string, int>;
                let nestedMap: map<string, map<int, array<boolean>>>;
                let complexType: Result<Option<array<map<string, int>>>, Error>;
                
                function process<T, U>(data: array<T>, transformer: function(T): U): array<U> {
                    // Implementation
                    return [];
                }
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }

    #[test]
    fn test_error_recovery_in_complex_expressions() {
        let input = r#"
            function calculate(): int {
                let result = (1 + @invalid) * (2 - #error) / (3 * $mistake);
                let complex = array[1, @wrong, 3].map(x => x * #invalid);
                return result + complex.reduce((a, b) => a + @oops);
            }
        "#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let result = GardParser::parse(tokens);
        assert!(result.is_ok());
    }
} 