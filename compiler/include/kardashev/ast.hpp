// Untyped AST for the kardashev V1 surface syntax.
//
// Grammar (informal):
//   program       := (fn_decl | struct_decl)*
//   fn_decl       := 'fn' Ident '(' params? ')' '->' type_ref block_expr
//   struct_decl   := 'struct' Ident '{' field_decl (',' field_decl)* ','? '}'
//   field_decl    := Ident ':' type_ref
//   params        := param (',' param)*
//   param         := Ident ':' type_ref
//   type_ref      := Ident                       -- e.g. i64
//   block_expr    := '{' stmt* tail_expr? '}'
//   stmt          := let_stmt | return_stmt | expr_stmt
//   let_stmt      := 'let' Ident '=' expr ';'
//   return_stmt   := 'return' expr? ';'
//   expr_stmt     := expr ';'
//   tail_expr     := expr                        -- block's value (no trailing ';')
//   expr          := <pratt-parsed binary>
//                 |  postfix
//   postfix       := primary ('.' Ident)*        -- field access
//   primary       := Integer | Ident | call | struct_lit | '(' expr ')' | if_expr | block_expr
//   call          := Ident '(' arglist? ')'
//   arglist       := expr (',' expr)*
//   struct_lit    := Ident '{' (field_init (',' field_init)* ','?)? '}'
//   field_init    := Ident ':' expr
//   if_expr       := 'if' expr block_expr 'else' block_expr
//
// Precedence (low to high): comparisons < additive < multiplicative.
//
// All AST nodes carry the source position (1-based line / column) of their
// most informative anchor token.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kardashev::ast {

enum class BinOp {
    Add, Sub, Mul, Div,
    Lt, Le, Gt, Ge,
    Eq, NotEq,
};

// Base expression. Polymorphic by design — use `dynamic_cast<...*>` in
// consumers, or extend with a virtual visitor later.
struct Expr {
    std::size_t line = 1;
    std::size_t column = 1;
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

// Base statement.
struct Stmt {
    std::size_t line = 1;
    std::size_t column = 1;
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

// --- Expressions ---

struct IntLitExpr : Expr {
    std::int64_t value = 0;
};

struct IdentExpr : Expr {
    std::string name;
};

struct BinaryExpr : Expr {
    BinOp op = BinOp::Add;
    ExprPtr lhs;
    ExprPtr rhs;
};

struct CallExpr : Expr {
    // V1: callee is always a bare identifier. First-class function values
    // and method syntax arrive in later phases.
    std::string callee;
    std::vector<ExprPtr> args;
};

struct StructLitExpr : Expr {
    std::string structName;
    std::vector<std::pair<std::string, ExprPtr>> fields;
};

struct FieldExpr : Expr {
    ExprPtr object;
    std::string fieldName;
};

struct IfExpr : Expr {
    ExprPtr cond;
    ExprPtr thenBranch; // a BlockExpr
    ExprPtr elseBranch; // a BlockExpr
};

struct BlockExpr : Expr {
    std::vector<StmtPtr> stmts;
    ExprPtr tail; // optional; null means the block has no value (unit)
};

// --- Statements ---

struct LetStmt : Stmt {
    std::string name;
    ExprPtr value;
};

struct ReturnStmt : Stmt {
    ExprPtr value; // may be null for bare `return;`
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

// --- Top-level ---

struct TypeRef {
    std::string name;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct Param {
    std::string name;
    TypeRef type;
};

struct FnDecl {
    std::string name;
    std::vector<Param> params;
    TypeRef returnType;
    std::unique_ptr<BlockExpr> body;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct StructDecl {
    std::string name;
    std::vector<Param> fields;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct Program {
    std::vector<FnDecl> functions;
    std::vector<StructDecl> structs;
};

} // namespace kardashev::ast
