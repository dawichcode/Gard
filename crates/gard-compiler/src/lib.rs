use gard_ast::{Node, Type, BinaryOp, UnaryOp, Parameter};
use inkwell::context::Context;
use inkwell::module::Module;
use inkwell::builder::Builder;
use inkwell::values::{BasicValue, BasicValueEnum, FunctionValue, PointerValue};
use inkwell::types::{BasicType, BasicTypeEnum, BasicMetadataTypeEnum};
use inkwell::AddressSpace;
use std::collections::HashMap;

pub struct Compiler<'ctx> {
    context: &'ctx Context,
    module: Module<'ctx>,
    builder: Builder<'ctx>,
    variables: HashMap<String, PointerValue<'ctx>>,
    functions: HashMap<String, FunctionValue<'ctx>>,
}

impl<'ctx> Compiler<'ctx> {
    pub fn new(context: &'ctx Context, module_name: &str) -> Self {
        let module = context.create_module(module_name);
        let builder = context.create_builder();
        
        Self {
            context,
            module,
            builder,
            variables: HashMap::new(),
            functions: HashMap::new(),
        }
    }

    pub fn compile(&mut self, ast: Node) -> Result<(), String> {
        match ast {
            Node::Program(nodes) => {
                for node in nodes {
                    self.compile_node(node)?;
                }
                Ok(())
            },
            _ => Err("Expected program node".to_string()),
        }
    }

    fn compile_node(&mut self, node: Node) -> Result<BasicValueEnum<'ctx>, String> {
        match node {
            Node::Function { name, params, return_type, body, .. } => {
                self.compile_function(name, params, return_type, *body)
            },
            Node::Let { name, type_annotation, initializer, .. } => {
                self.compile_let(name, type_annotation, initializer)
            },
            Node::Binary { left, operator, right } => {
                self.compile_binary_op(*left, operator, *right)
            },
            Node::Call { callee, arguments } => {
                self.compile_call(*callee, arguments)
            },
            Node::If { condition, then_branch, else_branch } => {
                self.compile_if(*condition, *then_branch, else_branch.map(|b| *b))
            },
            Node::While { condition, body } => {
                self.compile_while(*condition, *body)
            },
            Node::Return(value) => {
                self.compile_return(value.map(|v| *v))
            },
            Node::Block(statements) => {
                self.compile_block(statements)
            },
            Node::Identifier(name) => {
                self.compile_identifier(name)
            },
            Node::IntLiteral(value) => {
                Ok(self.context.i64_type().const_int(value as u64, false).as_basic_value_enum())
            },
            Node::StringLiteral(value) => {
                self.compile_string_literal(value)
            },
            Node::Actor { name, type_param, mailbox, behavior, members } => {
                self.compile_actor_system(node)
            },
            Node::Atomic { body } => {
                self.compile_stm(node)
            },
            Node::TVar { name, value_type, initial_value } => {
                self.compile_stm(node)
            },
            Node::Supervise { strategy, children } => {
                self.compile_supervision(node)
            },
            _ => Err(format!("Unsupported node type: {:?}", node)),
        }
    }

    fn compile_function(&mut self, name: String, params: Vec<Parameter>, return_type: Type, body: Node) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        let fn_type = match self.get_llvm_type(&return_type)? {
            BasicTypeEnum::IntType(t) => t.fn_type(&[], false),
            BasicTypeEnum::FloatType(t) => t.fn_type(&[], false),
            BasicTypeEnum::PointerType(t) => t.fn_type(&[], false),
            _ => return Err("Unsupported return type".to_string()),
        };

        let function = self.module.add_function(&name, fn_type, None);
        let basic_block = self.context.append_basic_block(function, "entry");
        self.builder.position_at_end(basic_block);

        // Add parameters to variables map
        for (i, param) in params.iter().enumerate() {
            let param_value = function.get_nth_param(i as u32)
                .ok_or_else(|| format!("Failed to get parameter {}", i))?;
            let alloca = self.builder.build_alloca(param_value.get_type(), &param.name);
            self.builder.build_store(alloca, param_value);
            self.variables.insert(param.name.clone(), alloca);
        }

        // Compile function body
        let body_value = self.compile_node(body)?;
        self.builder.build_return(Some(&body_value));

        Ok(function.as_global_value().as_basic_value_enum())
    }

    fn compile_let(&mut self, name: String, type_annotation: Option<Type>, initializer: Option<Box<Node>>) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        let var_type = if let Some(ty) = type_annotation {
            self.get_llvm_type(&ty)?
        } else {
            // Infer type from initializer
            if let Some(init) = &initializer {
                self.get_node_type(init)?
            } else {
                return Err("Cannot infer type without type annotation or initializer".to_string());
            }
        };

        let alloca = self.builder.build_alloca(var_type, &name);
        self.variables.insert(name, alloca);

        if let Some(init) = initializer {
            let init_val = self.compile_node(*init)?;
            self.builder.build_store(alloca, init_val);
        }

        Ok(alloca.as_basic_value_enum())
    }

    fn compile_binary_op(&mut self, left: Node, operator: BinaryOp, right: Node) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        let lhs = self.compile_node(left)?;
        let rhs = self.compile_node(right)?;

        match operator {
            BinaryOp::Add => Ok(self.builder.build_int_add(lhs.into_int_value(), rhs.into_int_value(), "addtmp").into()),
            BinaryOp::Sub => Ok(self.builder.build_int_sub(lhs.into_int_value(), rhs.into_int_value(), "subtmp").into()),
            BinaryOp::Mul => Ok(self.builder.build_int_mul(lhs.into_int_value(), rhs.into_int_value(), "multmp").into()),
            BinaryOp::Div => Ok(self.builder.build_int_signed_div(lhs.into_int_value(), rhs.into_int_value(), "divtmp").into()),
            BinaryOp::Eq => Ok(self.builder.build_int_compare(inkwell::IntPredicate::EQ, lhs.into_int_value(), rhs.into_int_value(), "eqtmp").into()),
            BinaryOp::NotEq => Ok(self.builder.build_int_compare(inkwell::IntPredicate::NE, lhs.into_int_value(), rhs.into_int_value(), "netmp").into()),
            BinaryOp::Lt => Ok(self.builder.build_int_compare(inkwell::IntPredicate::SLT, lhs.into_int_value(), rhs.into_int_value(), "lttmp").into()),
            BinaryOp::LtEq => Ok(self.builder.build_int_compare(inkwell::IntPredicate::SLE, lhs.into_int_value(), rhs.into_int_value(), "letmp").into()),
            BinaryOp::Gt => Ok(self.builder.build_int_compare(inkwell::IntPredicate::SGT, lhs.into_int_value(), rhs.into_int_value(), "gttmp").into()),
            BinaryOp::GtEq => Ok(self.builder.build_int_compare(inkwell::IntPredicate::SGE, lhs.into_int_value(), rhs.into_int_value(), "getmp").into()),
            _ => Err(format!("Unsupported binary operator: {:?}", operator)),
        }
    }

    fn compile_identifier(&mut self, name: String) -> Result<BasicValueEnum<'ctx>, String> {
        if let Some(var) = self.variables.get(&name) {
            Ok(self.builder.build_load(*var, &name))
        } else {
            Err(format!("Undefined variable: {}", name))
        }
    }

    fn compile_string_literal(&mut self, value: String) -> Result<BasicValueEnum<'ctx>, String> {
        Ok(self.builder.build_global_string_ptr(&value, "str")
            .as_pointer_value()
            .as_basic_value_enum())
    }

    fn get_function_type(&self, return_type: &Type, params: &[Parameter]) -> Result<inkwell::types::FunctionType<'ctx>, String> {
        let return_type = self.get_llvm_type(return_type)?;
        let param_types: Vec<BasicTypeEnum> = params
            .iter()
            .map(|p| self.get_llvm_type(&p.type_annotation))
            .collect::<Result<Vec<_>, _>>()?;

        Ok(return_type.fn_type(&param_types, false))
    }

    fn compile_call(&mut self, callee: Node, arguments: Vec<Node>) -> Result<BasicValueEnum<'ctx>, String> {
        let callee_value = self.compile_node(callee)?;
        let mut compiled_args = Vec::new();

        for arg in arguments {
            compiled_args.push(self.compile_node(arg)?);
        }

        let function = callee_value.into_pointer_value();
        Ok(self.builder
            .build_call(function, &compiled_args, "calltmp")
            .try_as_basic_value()
            .left()
            .ok_or_else(|| "Invalid call result".to_string())?)
    }

    fn get_llvm_type(&self, ty: &Type) -> Result<BasicTypeEnum<'ctx>, String> {
        match ty {
            Type::Int => Ok(self.context.i64_type().as_basic_type_enum()),
            Type::Float => Ok(self.context.f64_type().as_basic_type_enum()),
            Type::String => Ok(self.context.i8_type().ptr_type(AddressSpace::default()).as_basic_type_enum()),
            Type::Boolean => Ok(self.context.bool_type().as_basic_type_enum()),
            Type::Array(elem_type) => {
                let elem_type = self.get_llvm_type(elem_type)?;
                Ok(elem_type.array_type(0).as_basic_type_enum())
            },
            Type::Custom(name) => {
                // Handle custom types (e.g., classes, interfaces)
                Err(format!("Custom type not yet supported: {}", name))
            },
            _ => Err(format!("Unsupported type: {:?}", ty)),
        }
    }

    fn get_node_type(&self, node: &Node) -> Result<BasicTypeEnum<'ctx>, String> {
        match node {
            Node::IntLiteral(_) => Ok(self.context.i64_type().as_basic_type_enum()),
            Node::FloatLiteral(_) => Ok(self.context.f64_type().as_basic_type_enum()),
            Node::StringLiteral(_) => Ok(self.context.i8_type().ptr_type(AddressSpace::default()).as_basic_type_enum()),
            Node::BooleanLiteral(_) => Ok(self.context.bool_type().as_basic_type_enum()),
            _ => Err(format!("Cannot infer type for node: {:?}", node)),
        }
    }

    fn compile_if(&mut self, condition: Node, then_branch: Node, else_branch: Option<Node>) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        let condition_value = self.compile_node(condition)?;
        let function = self.builder.get_insert_block().unwrap().get_parent().unwrap();
        
        let then_block = self.context.append_basic_block(function, "then");
        let else_block = self.context.append_basic_block(function, "else");
        let merge_block = self.context.append_basic_block(function, "merge");

        self.builder.build_conditional_branch(
            condition_value.into_int_value(),
            then_block,
            else_block
        );

        // Compile then branch
        self.builder.position_at_end(then_block);
        let then_value = self.compile_node(then_branch)?;
        self.builder.build_unconditional_branch(merge_block);

        // Compile else branch
        self.builder.position_at_end(else_block);
        let else_value = if let Some(else_branch) = else_branch {
            self.compile_node(else_branch)?
        } else {
            // Return void if no else branch
            self.context.i64_type().const_int(0, false).as_basic_value_enum()
        };
        self.builder.build_unconditional_branch(merge_block);

        // Merge block
        self.builder.position_at_end(merge_block);
        let phi = self.builder.build_phi(then_value.get_type(), "if_result");
        phi.add_incoming(&[(&then_value, then_block), (&else_value, else_block)]);

        Ok(phi.as_basic_value())
    }

    fn compile_while(&mut self, condition: Node, body: Node) -> Result<BasicValueEnum<'ctx>, String> {
        let function = self.builder.get_insert_block().unwrap().get_parent().unwrap();
        
        let cond_block = self.context.append_basic_block(function, "while.cond");
        let body_block = self.context.append_basic_block(function, "while.body");
        let end_block = self.context.append_basic_block(function, "while.end");

        // Jump to condition
        self.builder.build_unconditional_branch(cond_block);
        self.builder.position_at_end(cond_block);

        // Compile condition
        let condition_value = self.compile_node(condition)?;
        self.builder.build_conditional_branch(
            condition_value.into_int_value(),
            body_block,
            end_block
        );

        // Compile body
        self.builder.position_at_end(body_block);
        self.compile_node(body)?;
        self.builder.build_unconditional_branch(cond_block);

        // Continue at end block
        self.builder.position_at_end(end_block);
        
        Ok(self.context.i64_type().const_int(0, false).as_basic_value_enum())
    }

    fn compile_block(&mut self, statements: Vec<Node>) -> Result<BasicValueEnum<'ctx>, String> {
        let mut last_value = self.context.i64_type().const_int(0, false).as_basic_value_enum();
        
        for stmt in statements {
            last_value = self.compile_node(stmt)?;
        }
        
        Ok(last_value)
    }

    fn compile_return(&mut self, value: Option<Node>) -> Result<BasicValueEnum<'ctx>, String> {
        match value {
            Some(value) => {
                let return_value = self.compile_node(value)?;
                self.builder.build_return(Some(&return_value));
            },
            None => {
                self.builder.build_return(None);
            }
        }
        
        Ok(self.context.i64_type().const_int(0, false).as_basic_value_enum())
    }

    fn compile_actor_system(&mut self, node: Node) -> Result<BasicValueEnum<'ctx>, String> {
        match node {
            Node::Actor { name, type_param, mailbox, behavior, members } => {
                // Create actor class type
                let actor_type = self.context.opaque_struct_type(&name);
                let field_types = vec![
                    self.get_llvm_type(&Type::Custom("MessageQueue".to_string()))?,
                    self.get_llvm_type(&Type::Custom("ActorBehavior".to_string()))?,
                ];
                actor_type.set_body(&field_types, false);

                // Compile members
                for member in members {
                    self.compile_node(member)?;
                }

                // Create constructor
                let constructor_type = self.context.void_type().fn_type(&[], false);
                let constructor = self.module.add_function(
                    &format!("{}_new", name),
                    constructor_type,
                    None
                );

                // Initialize mailbox and behavior
                let entry = self.context.append_basic_block(constructor, "entry");
                self.builder.position_at_end(entry);
                self.compile_node(*mailbox)?;
                self.compile_node(*behavior)?;

                Ok(constructor.as_global_value().as_basic_value_enum())
            },
            _ => Err("Expected actor node".to_string()),
        }
    }

    fn compile_stm(&mut self, node: Node) -> Result<BasicValueEnum<'ctx>, String> {
        match node {
            Node::Atomic { body } => {
                // Create transaction context
                let transaction_type = self.context.opaque_struct_type("Transaction");
                let transaction = self.builder.build_alloca(transaction_type, "transaction");

                // Start transaction
                let start_transaction = self.module.add_function(
                    "stm_start_transaction",
                    self.context.void_type().fn_type(&[], false),
                    None
                );
                self.builder.build_call(start_transaction, &[], "start");

                // Compile transaction body
                let result = self.compile_node(*body)?;

                // Try to commit
                let commit_transaction = self.module.add_function(
                    "stm_commit_transaction",
                    self.context.bool_type().fn_type(&[], false),
                    None
                );
                let commit_result = self.builder.build_call(commit_transaction, &[], "commit");

                // Create success and failure blocks
                let success_block = self.context.append_basic_block(
                    self.builder.get_insert_block().unwrap().get_parent().unwrap(),
                    "commit.success"
                );
                let failure_block = self.context.append_basic_block(
                    self.builder.get_insert_block().unwrap().get_parent().unwrap(),
                    "commit.failure"
                );

                self.builder.build_conditional_branch(
                    commit_result.try_as_basic_value().left().unwrap().into_int_value(),
                    success_block,
                    failure_block
                );

                // Success path: return result
                self.builder.position_at_end(success_block);
                self.builder.build_return(Some(&result));

                // Failure path: retry
                self.builder.position_at_end(failure_block);
                let retry_transaction = self.module.add_function(
                    "stm_retry_transaction",
                    self.context.void_type().fn_type(&[], false),
                    None
                );
                self.builder.build_call(retry_transaction, &[], "retry");

                Ok(result)
            },
            Node::TVar { name, value_type, initial_value } => {
                let tvar_type = self.get_llvm_type(&value_type)?;
                let tvar = self.builder.build_alloca(tvar_type, &name);

                if let Some(init) = initial_value {
                    let init_val = self.compile_node(*init)?;
                    self.builder.build_store(tvar, init_val);
                }

                Ok(tvar.as_basic_value_enum())
            },
            _ => Err("Expected STM node".to_string()),
        }
    }

    fn compile_supervision(&mut self, node: Node) -> Result<BasicValueEnum<'ctx>, String> {
        match node {
            Node::Supervise { strategy, children } => {
                // Create supervisor context
                let supervisor_type = self.context.opaque_struct_type("Supervisor");
                let supervisor = self.builder.build_alloca(supervisor_type, "supervisor");

                // Set supervision strategy
                let strategy_val = match strategy {
                    SupervisionStrategy::OneForOne => 0,
                    SupervisionStrategy::OneForAll => 1,
                    SupervisionStrategy::RestForOne => 2,
                    SupervisionStrategy::Custom(_) => 3,
                };
                let strategy_type = self.context.i32_type();
                
                // Fix: Build struct GEP with correct type and index
                let strategy_ptr = unsafe {
                    self.builder.build_struct_gep(
                        supervisor_type,
                        supervisor,
                        0,  // Index for strategy field
                        "strategy_ptr"
                    ).unwrap()
                };

                self.builder.build_store(
                    strategy_ptr,
                    strategy_type.const_int(strategy_val, false)
                );

                // Compile child actors
                for child in children {
                    self.compile_node(child)?;
                }

                Ok(supervisor.as_basic_value_enum())
            },
            _ => Err("Expected supervision node".to_string()),
        }
    }

    fn compile_try_catch(&mut self, body: Node, catch_clauses: Vec<Node>, finally: Option<Node>) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        // Create basic blocks for try, catch, finally and continue
        let function = self.builder.get_insert_block().unwrap().get_parent().unwrap();
        let try_block = self.context.append_basic_block(function, "try");
        let catch_block = self.context.append_basic_block(function, "catch");
        let finally_block = self.context.append_basic_block(function, "finally");
        let continue_block = self.context.append_basic_block(function, "continue");

        // Build try block
        self.builder.position_at_end(try_block);
        let try_result = self.compile_node(body)?;
        self.builder.build_unconditional_branch(finally_block);

        // Build catch block
        self.builder.position_at_end(catch_block);
        for catch_clause in catch_clauses {
            self.compile_node(catch_clause)?;
        }
        self.builder.build_unconditional_branch(finally_block);

        // Build finally block
        self.builder.position_at_end(finally_block);
        if let Some(finally_body) = finally {
            self.compile_node(finally_body)?;
        }
        self.builder.build_unconditional_branch(continue_block);

        // Continue block
        self.builder.position_at_end(continue_block);
        Ok(try_result)
    }

    fn compile_match(&mut self, value: Node, cases: Vec<MatchCase>) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        let value_result = self.compile_node(value)?;
        let function = self.builder.get_insert_block().unwrap().get_parent().unwrap();
        
        let mut case_blocks = Vec::new();
        let default_block = self.context.append_basic_block(function, "match.default");
        let continue_block = self.context.append_basic_block(function, "match.continue");

        // Create blocks for each case
        for (i, _) in cases.iter().enumerate() {
            case_blocks.push(self.context.append_basic_block(function, &format!("match.case{}", i)));
        }

        // Build switch instruction
        self.builder.build_switch(
            value_result.into_int_value(),
            default_block,
            &cases.iter().enumerate().map(|(i, case)| {
                let pattern = self.compile_node(case.pattern.clone())
                    .unwrap()
                    .into_int_value();
                (pattern, case_blocks[i])
            }).collect::<Vec<_>>()
        );

        // Build case blocks
        for (i, case) in cases.iter().enumerate() {
            self.builder.position_at_end(case_blocks[i]);
            self.compile_node(case.body.clone())?;
            self.builder.build_unconditional_branch(continue_block);
        }

        // Build default block
        self.builder.position_at_end(default_block);
        self.builder.build_unconditional_branch(continue_block);

        // Continue block
        self.builder.position_at_end(continue_block);
        Ok(value_result)
    }

    fn compile_loop(&mut self, condition: Option<Node>, body: Node, is_do_while: bool) 
        -> Result<BasicValueEnum<'ctx>, String> 
    {
        let function = self.builder.get_insert_block().unwrap().get_parent().unwrap();
        
        let condition_block = self.context.append_basic_block(function, "loop.cond");
        let body_block = self.context.append_basic_block(function, "loop.body");
        let continue_block = self.context.append_basic_block(function, "loop.continue");

        // For do-while, enter body first
        if is_do_while {
            self.builder.build_unconditional_branch(body_block);
        } else {
            self.builder.build_unconditional_branch(condition_block);
        }

        // Build condition block
        self.builder.position_at_end(condition_block);
        let should_continue = if let Some(cond) = condition {
            self.compile_node(cond)?
        } else {
            self.context.bool_type().const_int(1, false).as_basic_value_enum()
        };

        self.builder.build_conditional_branch(
            should_continue.into_int_value(),
            body_block,
            continue_block
        );

        // Build body block
        self.builder.position_at_end(body_block);
        self.compile_node(body)?;
        self.builder.build_unconditional_branch(condition_block);

        // Continue block
        self.builder.position_at_end(continue_block);
        Ok(self.context.i64_type().const_int(0, false).as_basic_value_enum())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use inkwell::context::Context;

    #[test]
    fn test_compile_basic_function() {
        let context = Context::create();
        let mut compiler = Compiler::new(&context, "test");

        let input = Node::Function {
            name: "test".to_string(),
            params: vec![],
            return_type: Type::Int,
            body: Box::new(Node::IntLiteral(42)),
            modifiers: vec![],
        };

        let result = compiler.compile_node(input);
        assert!(result.is_ok());
    }

    #[test]
    fn test_compile_binary_operation() {
        let context = Context::create();
        let mut compiler = Compiler::new(&context, "test");

        let input = Node::Binary {
            left: Box::new(Node::IntLiteral(40)),
            operator: BinaryOp::Add,
            right: Box::new(Node::IntLiteral(2)),
        };

        let result = compiler.compile_node(input);
        assert!(result.is_ok());
    }
} 