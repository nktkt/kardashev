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

// Phase 5.y: `"..."` string literal. Codegen lowers to a heap-immutable
// global byte buffer wrapped in the built-in `String` struct.
struct StringLitExpr : Expr {
    std::string value;
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

// Phase 9: `while cond { body }`. `cond` must be bool; `body` is a
// BlockExpr that is checked for unit type. The whole expression is unit.
struct WhileExpr : Expr {
    ExprPtr cond;
    ExprPtr body; // a BlockExpr
};

// Phase 9: `loop { body }`. A bare loop with no `break` never terminates
// (type "never", treated as unit for the MVP). If every `break` carries a
// value of a common type T, the loop expression has type T.
struct LoopExpr : Expr {
    ExprPtr body; // a BlockExpr
};

// Phase 9: `a..b` (exclusive) / `a..=b` (inclusive). Both endpoints must
// be i64. Typechecks to the built-in `Range` struct so `for` can iterate
// it through the `Iterator` trait.
struct RangeExpr : Expr {
    ExprPtr start;
    ExprPtr end;
    bool inclusive = false; // false => `..`, true => `..=`
};

// Phase 9: `for <pat> in <range> { body }`. The range is currently always
// a RangeExpr over i64. Conceptually desugars through the `Iterator`
// trait (`let mut it = <range>; loop { match it.next() { Some(x) => body,
// None => break } }`); codegen lowers integer ranges directly (the impl
// method ABI passes `&mut self` by value, so a literal method-driven
// desugar can't advance the iterator). The whole expression is unit.
struct ForExpr : Expr {
    PatternPtr pattern; // the loop variable binding (a VarPat for ranges)
    ExprPtr iter;       // a RangeExpr
    ExprPtr body;       // a BlockExpr
};

// Phase 9: `break` / `break <value>`. Exits the innermost enclosing loop.
// `value` is null for a bare `break`. Only legal inside `loop`/`while`/
// `for`; `break <value>` is only meaningful inside `loop`.
struct BreakExpr : Expr {
    ExprPtr value; // may be null
};

// Phase 9: `continue`. Jumps to the innermost enclosing loop's header.
struct ContinueExpr : Expr {};

// --- Statements ---

struct LetStmt : Stmt {
    std::string name;
    ExprPtr value;
    // Phase 9: `let mut x = ...`. Marks the binding as reassignable; the
    // typechecker rejects assignment to a non-mut binding.
    bool isMut = false;
};

struct ReturnStmt : Stmt {
    ExprPtr value; // may be null for bare `return;`
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

// Phase 9: `lhs = rhs;`. `lhs` is an assignable place — either a bare
// IdentExpr (a `let mut` binding) or a FieldExpr chain rooted at one.
// The typechecker enforces lhs/rhs type agreement and mutability.
struct AssignStmt : Stmt {
    ExprPtr target;
    ExprPtr value;
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
// Phase 10a: a function *type* in type position, e.g. `fn(i64) -> i64 !
// {io}` or `fn(T) -> U ! {e}`. When `isFn` is true, `fnParams` / `fnRet`
// describe the signature and `fnEffects` its effect row (which may name an
// effect-row variable bound by the enclosing fn's generic params). `name`
// is left empty for fn types. Effect-carrying function types are how
// kardashev threads effects through first-class fn values + higher-order
// functions.
struct EffectRow; // fwd (defined below)

struct TypeRef {
    std::string name;
    std::vector<TypeRef> typeArgs;
    bool isRef = false;
    bool refIsMut = false;
    // Function-type fields (valid only when isFn == true).
    bool isFn = false;
    std::vector<TypeRef> fnParams;
    std::shared_ptr<TypeRef> fnRet; // shared_ptr so TypeRef stays copyable
    std::vector<std::string> fnEffects; // effect-row labels (concrete + vars)
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
