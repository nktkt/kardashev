// Untyped AST for the kardashev V1 surface syntax.
//
// Grammar (informal):
//   program       := (mod_decl | fn_decl | struct_decl | enum_decl | trait_decl | impl_decl)*
//   mod_decl      := 'mod' Ident ';'              -- Phase 7: pulls in <Ident>.kd
//   fn_decl       := 'fn' Ident generic_params? '(' params? ')' '->' type_ref effect_row? block_expr
//   generic_params:= '<' generic_param (',' generic_param)* ','? '>'
//   generic_param := Ident (':' Ident)?           -- optional single-trait bound
//   trait_decl    := 'trait' Ident '{' method_sig* '}'
//   method_sig    := 'fn' Ident '(' params? ')' '->' type_ref effect_row? ';'
//   impl_decl     := 'impl' Ident 'for' type_ref '{' fn_decl* '}'
//   type_ref      := Ident type_args?
//   type_args     := '<' type_ref (',' type_ref)* ','? '>'
//   effect_row    := '!' '{' (effect_label (',' effect_label)* ','?)? '}'
//   effect_label  := Ident                        -- built-in: alloc, io, panic, async, unwind
//                 |  row variable bound by generic_params
//   postfix       := primary ( '.' Ident method_call_args? | '?' )*
//   method_call_args := '(' arglist? ')'
//   struct_decl   := 'struct' Ident '{' field_decl (',' field_decl)* ','? '}'
//   field_decl    := Ident ':' type_ref
//   enum_decl     := 'enum' Ident '{' (variant (',' variant)* ','?)? '}'
//   variant       := Ident ('(' type_ref (',' type_ref)* ','? ')')?
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
//   primary       := Integer | Ident | call | struct_lit | '(' expr ')' | if_expr | block_expr | match_expr
//   call          := Ident '(' arglist? ')'
//   arglist       := expr (',' expr)*
//   struct_lit    := Ident '{' (field_init (',' field_init)* ','?)? '}'
//   field_init    := Ident ':' expr
//   if_expr       := 'if' expr block_expr 'else' block_expr
//   match_expr    := 'match' expr '{' (arm (',' arm)* ','?)? '}'
//   arm           := pattern '=>' expr
//   pattern       := Integer | '_' | Ident                       -- LitInt, Wild, or VarBind
//                 |  Ident '(' (pattern (',' pattern)* ','?)? ')' -- Ctor
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

// --- Patterns ---
//
// Patterns form their own polymorphic hierarchy, used as the left-hand side
// of match arms. A bare `Ident` in pattern position is intentionally
// ambiguous between a variable binding (`VarPat`) and a unit constructor
// (`CtorPat` with zero subpatterns). The parser always produces `VarPat`
// at parse time (simpler grammar, no constructor-table lookup mid-parse);
// the typechecker rewrites the semantics to a unit-ctor match when the
// name resolves to a known constructor in scope. Parenthesized
// `Ident(...)` forms are unambiguously `CtorPat` from the start.
struct Pattern {
    std::size_t line = 1;
    std::size_t column = 1;
    virtual ~Pattern() = default;
};
using PatternPtr = std::unique_ptr<Pattern>;

struct LitIntPat : Pattern {
    std::int64_t value = 0;
};

struct WildPat : Pattern {};

struct VarPat : Pattern {
    std::string name;
};

struct CtorPat : Pattern {
    std::string ctorName;
    std::vector<PatternPtr> subpatterns;
};

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
    // Phase 7.3b: true when the source called the fn via a multi-segment
    // path (`foo::bar(args)`). Combined with FnSchema.isPub the
    // typechecker can enforce visibility on cross-module references
    // without breaking bare-name calls that the Phase 7.1 flat-merge
    // semantics still permits.
    bool wasPath = false;
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

struct MatchArm {
    PatternPtr pattern;
    ExprPtr body;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 3.4: `expr?` postfix. Desugars (in codegen) to `match expr { Ok(v)
// => v, Err(e) => return Err(e) }`. The typechecker enforces that `expr`
// is an enum with `Ok(T)` and `Err(E)` variants and that the enclosing
// function returns an enum with an `Err(E)`-compatible variant. The
// TryExpr's own type is the operand's `Ok` payload type.
struct TryExpr : Expr {
    ExprPtr operand;
};

// Phase 2.4b: `&expr` (and Phase 2.4c `&mut expr`). Phase 2.4b restricts
// `operand` to a bare Ident — borrowing a temporary expression doesn't
// have a useful semantics until we have a stack-spill rule.
struct RefExpr : Expr {
    ExprPtr operand;
    bool isMut = false;
};

// Phase 6 (stub): `expr.await` postfix. Today this is a no-op at both
// typecheck and codegen (the operand's type / value flow through
// unchanged); once a state-machine transform lands this becomes the
// suspend point of the enclosing async fn.
struct AwaitExpr : Expr {
    ExprPtr operand;
};

struct MatchExpr : Expr {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
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

// `Option<i64>` parses as TypeRef{name="Option", typeArgs=[{name="i64"}]}.
// Empty `typeArgs` means a bare type reference; the typechecker rejects
// type-args on names that don't denote a generic type and rejects missing
// type-args on names that do. Type parameter names (e.g. `T` inside
// `fn id<T>(x: T)`) are written as bare `Ident` references and resolved
// against the enclosing fn/struct/enum's generic-param env first.
//
// `&T` and `&mut T` (Phase 2.4b/c) set isRef + refIsMut on the same node.
// Nested references (`&&T`) aren't yet on the grammar.
struct TypeRef {
    std::string name;
    std::vector<TypeRef> typeArgs;
    bool isRef = false;
    bool refIsMut = false;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct Param {
    std::string name;
    TypeRef type;
};

// Phase 4: a function's declared effect row. `labels` carries the
// built-in concrete effects (`alloc`, `io`, `panic`, `async`, `unwind`)
// PLUS any effect-row variable names introduced via the fn's
// genericParams. An empty EffectRow means `pure` — no effects declared.
//
// Surface syntax: `! { io, alloc }` after the return type. Effect-row
// variables (`! {e}`) become first-class generic parameters declared
// alongside type parameters; the parser treats unknown labels in an
// effect_row as row-variable references and validates them against the
// enclosing fn's genericParams at typecheck time.
struct EffectRow {
    std::vector<std::string> labels;
    std::size_t line = 1;
    std::size_t column = 1;
};

// A generic type parameter binding, e.g. `T` in `fn id<T>(x: T) -> T` or
// `T: Show` in `fn use_show<T: Show>(t: T) -> i64`. Phase 3.3 adds a
// single optional trait bound per param; multiple bounds (`T: A + B`)
// can wait for a later phase.
struct TypeParam {
    std::string name;
    std::string bound; // empty = unbounded
    std::size_t line = 1;
    std::size_t column = 1;
};

struct FnDecl {
    std::string name;
    std::vector<TypeParam> genericParams; // empty = monomorphic fn
    std::vector<Param> params;
    TypeRef returnType;
    EffectRow effects; // Phase 4: declared effect row; empty = pure
    bool isAsync = false; // Phase 6 (stub): `async fn` desugars to a fn
                            // that implicitly carries the `async` effect.
    bool isPub = false;   // Phase 7.3b: visible across module boundaries
                            // when referenced via path syntax.
    std::unique_ptr<BlockExpr> body;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct StructDecl {
    std::string name;
    std::vector<TypeParam> genericParams; // empty = monomorphic struct
    std::vector<Param> fields;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct EnumVariant {
    std::string name;
    std::vector<TypeRef> payloadTypes; // empty => unit variant; non-empty => tuple-payload
    std::size_t line = 1;
    std::size_t column = 1;
};

struct EnumDecl {
    std::string name;
    std::vector<TypeParam> genericParams; // empty = monomorphic enum
    std::vector<EnumVariant> variants;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Trait method signature (declaration without a body). The first param
// is conventionally named `self` and has type `Self`; the parser stores
// it as a regular Param with TypeRef name "Self" so downstream code can
// rebind the name to the implementing type at use sites.
struct MethodSig {
    std::string name;
    std::vector<Param> params;
    TypeRef returnType;
    EffectRow effects; // Phase 4: declared effect row on trait methods
    std::size_t line = 1;
    std::size_t column = 1;
};

struct TraitDecl {
    std::string name;
    std::vector<MethodSig> methods;
    std::size_t line = 1;
    std::size_t column = 1;
};

// `impl TraitName for TypeRef { fn ... ... }`. The `forType` is a full
// TypeRef so it can be a generic instantiation (e.g. `Box<i64>`); the
// typechecker validates that each method's signature matches the trait's
// after substituting Self -> forType.
struct ImplDecl {
    std::string traitName;
    TypeRef forType;
    std::vector<FnDecl> methods;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 3.3: postfix method call `receiver.methodName(args)`. Parsed as
// a distinct AST node so the typechecker can route through trait /
// impl resolution rather than the regular `Ident -> CallExpr` path.
struct MethodCallExpr : Expr {
    ExprPtr receiver;
    std::string methodName;
    std::vector<ExprPtr> args;
};

// Phase 7: `mod foo;` references — the resolver loads `foo.kd` next to
// the source file and merges its decls into the program. After resolution
// these entries are erased; downstream passes see one flat Program.
struct ModDecl {
    std::string name;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct Program {
    std::vector<FnDecl> functions;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<TraitDecl> traits;
    std::vector<ImplDecl> impls;
    std::vector<ModDecl> mods;
};

} // namespace kardashev::ast
