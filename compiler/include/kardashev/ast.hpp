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

#include "kardashev/types.hpp" // Phase 10b: ClosureCapture carries a TypePtr

namespace kardashev::ast {

enum class BinOp {
    Add, Sub, Mul, Div,
    Mod,          // Phase 33: `%` integer modulo
    Lt, Le, Gt, Ge,
    Eq, NotEq,
    And,          // Phase 33: `&&` short-circuit logical-and (bool -> bool)
    Or,           // Phase 124: `||` short-circuit logical-or (bool -> bool)
    // Phase 66: integer bitwise operators (int -> int, any width/signedness).
    // `Shr` is arithmetic (sign-extending) for a signed operand and logical
    // (zero-filling) for an unsigned one — codegen picks ashr/lshr by type.
    BitAnd,       // &  (infix; prefix `&` is still borrow)
    BitOr,        // |  (infix; primary `|...|` is still a closure)
    BitXor,       // ^
    Shl,          // <<
    Shr,          // >>
};

// Phase 15: prefix unary operators. `Neg` is integer negation (`-x`,
// i64 -> i64); `Not` is logical not (`!x`, bool -> bool). Both bind
// tighter than every binary operator.
enum class UnaryOp {
    Neg,    // -x
    Not,    // !x
    Deref,  // *r — Phase 34: read the pointee of a `&T` / `Box<T>` (yields T)
    BitNot, // ~x — Phase 66: integer bitwise complement (int -> same int)
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

// v27 Phase 147: a char-literal pattern `'a' => ...`. Refutable; the scrutinee
// must be `char`. Lowered through the same literal-discriminated decision tree
// as LitIntPat (keyed on the codepoint), comparing at the char's i32 width.
struct LitCharPat : Pattern {
    std::uint32_t codepoint = 0;
};

struct WildPat : Pattern {};

struct VarPat : Pattern {
    std::string name;
};

struct CtorPat : Pattern {
    std::string ctorName;
    std::vector<PatternPtr> subpatterns;
};

// Phase 36: a tuple destructuring pattern `(p0, p1, ...)`. Irrefutable (a
// tuple has exactly one shape); binds each sub-pattern to the corresponding
// element. Modeled in the match compiler as a single-constructor type.
struct TuplePat : Pattern {
    std::vector<PatternPtr> elements;
};

// v26 Phase 141: an or-pattern `p1 | p2 | …` in a match arm. Matches if ANY
// alternative does. A pre-typecheck pass (expandOrPatterns) splits an arm with
// an OrPat into one arm per alternative (deep-cloning the body), so the match
// compiler / typechecker never see this node.
struct OrPat : Pattern {
    std::vector<PatternPtr> alternatives;
};

// v26 Phase 143: a slice pattern `[a, b, _, ..]`. Each element binds a name (or
// `_` to skip); `hasRest` is a trailing `..` (prefix match, length >= N). The
// parser desugars a slice-pattern match into a length-checked if/else chain, so
// this node never reaches the typechecker.
struct SlicePat : Pattern {
    std::vector<std::string> elements; // "_" = wildcard
    bool hasRest = false;
};

// --- Expressions ---

// Forward decl: TypeRef is defined in the "Top-level" section below, but
// LetStmt (Phase 11 type annotation) references it via shared_ptr.
struct TypeRef;

struct IntLitExpr : Expr {
    std::int64_t value = 0;
    // Phase 64 (v11): an explicit suffix `5i32` / `0xFFu8` pins the literal's
    // type (width + signedness). `suffixWidth == 0` means no suffix (an
    // unsuffixed literal is i64 by default and narrows in context).
    int suffixWidth = 0;
    bool suffixSigned = true;
};

// Phase 39: an `f64` floating-point literal (e.g. `1.5`, `-2.0`, `3e8`). The
// lexeme is kept verbatim; codegen parses it to a `double` ConstantFP.
struct FloatLitExpr : Expr {
    std::string lexeme;
    double value = 0.0;
    // Phase 67 (v11): an explicit `f32` / `f64` suffix pins the literal width.
    // `suffixWidth == 0` means no suffix (an unsuffixed float is f64 by default
    // and narrows to f32 in context).
    int suffixWidth = 0;
};

// Phase 15: `true` / `false` boolean literal. Codegen lowers to an i1
// constant (1/0); typechecks to `bool`. Mirrors IntLitExpr.
struct BoolLitExpr : Expr {
    bool value = false;
};

// Phase 5.y: `"..."` string literal. Codegen lowers to a heap-immutable
// global byte buffer wrapped in the built-in `String` struct.
struct StringLitExpr : Expr {
    std::string value;
};

// v27 Phase 147: `'c'` char literal — one Unicode scalar value. Typechecks to
// the `char` type; codegen lowers to an i32 constant holding the codepoint.
struct CharLitExpr : Expr {
    std::uint32_t codepoint = 0;
};

struct IdentExpr : Expr {
    std::string name;
};

struct BinaryExpr : Expr {
    BinOp op = BinOp::Add;
    ExprPtr lhs;
    ExprPtr rhs;
};

// Phase 15: prefix unary expression `-operand` / `!operand`. Parsed in
// prefix position, binding tighter than any binary operator so that
// `-a * b` == `(-a) * b` and `!a == b` == `(!a) == b`. Typecheck:
// `Neg` requires/produces i64; `Not` requires/produces bool. Mirrors
// BinaryExpr.
struct UnaryExpr : Expr {
    UnaryOp op = UnaryOp::Neg;
    ExprPtr operand;
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
    // Phase 48: the segment immediately before the callee when the path is a
    // qualified call (`Type::method(args)`) — empty otherwise. The typechecker
    // uses it ONLY when it names a type with a static (no-self) trait method
    // `method`; for an ordinary module path (`foo::bar()`) it stays unused and
    // the call falls back to the flat-merged bare-name lookup of `callee`.
    std::string pathQualifier;
};

// Phase 17a: call a fn VALUE produced by an arbitrary expression — e.g.
// `(s.f)(args)`, `(getCallback())(args)`. The plain CallExpr above is keyed
// by a callee *name* (direct fn, path fn, indirect call through a let-bound
// name, or a variant constructor); this node handles the cases where the
// callee is itself a sub-expression (a parenthesized expr or a postfix field
// access) of `Function` type. Parsed in postfix position when a `(arglist)`
// follows any non-name-callable primary; typecheck resolves the callee's
// Function type and unifies args; codegen evaluates the callee to a fat
// pointer `{ fn, env }` and dispatches through it (the same path Phase 10b
// uses for fn-typed locals). This unblocks lazy iterator adaptors (a struct
// holding a source + a closure field, stepped via `(self.f)(x)`).
struct CallValueExpr : Expr {
    ExprPtr callee;
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

// Phase 13b: `&v[a..b]` — borrow a contiguous sub-region of a Vec as a
// slice (fat pointer { ptr, len }). `operand` is the Vec being viewed;
// `start`/`end` are the i64 bounds of the half-open range [start, end).
// The `&` and the `[a..b]` are parsed together into this single node (the
// slice value is itself a borrow, so no extra RefExpr wraps it). Element
// type is the Vec's T (MVP exercises i64).
struct SliceExpr : Expr {
    ExprPtr operand; // the Vec expression
    ExprPtr start;   // i64 lower bound (inclusive)
    ExprPtr end;     // i64 upper bound (exclusive)
};

// Phase 22: an array literal `[a, b, c]`. All elements must share one type T;
// the array's type is `[T; N]` where N = elements.size(). An empty `[]`
// literal can't infer T, so it is rejected at typecheck. Lowers to an LLVM
// array value built via insertvalue (value type, copied like a struct).
struct ArrayLitExpr : Expr {
    std::vector<ExprPtr> elements;
    // Phase 62 (v10): array-REPEAT `[value; N]` — `elements[0]` is the repeated
    // value and `repeatCount` is the length expression (a literal, a const
    // item, or a const-generic param `N`). null for an ordinary `[a, b, c]`
    // element list. Lets a const-generic fn build a `[T; N]` result of
    // symbolic size (e.g. a transposed/zeroed matrix).
    ExprPtr repeatCount;
};

// Phase 22: indexing `arr[i]` — reads element `i` (an i64 index) of a
// fixed-size array. Distinct from the slice form `&v[a..b]` (a RangeExpr-like
// SliceExpr): a plain `arr[i]` with no `..` is an index. A compile-time-
// constant out-of-range literal index is a typecheck error; a dynamic index
// is unchecked in the MVP (no runtime bounds check), matching the unchecked
// `vec_get` / `slice_get` builtins.
struct IndexExpr : Expr {
    ExprPtr object; // the array expression
    ExprPtr index;  // an i64 index
};

// Phase 22: a tuple literal `(a, b)`, `(a, b, c)`. Always >= 2 elements (the
// parser only builds this when it sees a comma inside the parens; `(x)` is a
// parenthesized expr, `()` is unit). Type is `(T0, T1, ...)`. Lowers to an
// anonymous LLVM struct value built via insertvalue.
struct TupleLitExpr : Expr {
    std::vector<ExprPtr> elements;
};

// Phase 22: tuple field access by index `t.0`, `t.1`. The parser produces
// this when a numeric (Integer) token follows the `.` in postfix position
// (`recv.method()` / `recv.field` keep using MethodCallExpr / FieldExpr for
// identifier members). `index` is the 0-based element position.
struct TupleFieldExpr : Expr {
    ExprPtr object;
    std::size_t index = 0;
};

// `expr.await` postfix: the suspend point of the enclosing async fn. It
// unwraps a `Future<T>` to its `T`; codegen (Phase 12) lowers it to a poll
// loop over the sub-future's heap frame that genuinely returns Pending and
// resumes (the Phase 18 executor drives it). Typecheck unwraps Future<T> -> T.
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
struct MethodCallExpr; // fwd (defined below)

struct ForExpr : Expr {
    PatternPtr pattern; // the loop variable binding (a VarPat for ranges)
    ExprPtr iter;       // a RangeExpr (fast path) or any `Iterator` impl
    ExprPtr body;       // a BlockExpr
    // Phase 13a: general `for` over an arbitrary `Iterator`. When `iter` is
    // not a literal range / Range value but a type that impls `Iterator`, the
    // typechecker sets `iteratorDesugar` and fills the two synthetic nodes
    // below so the loop lowers to `{ let mut __it = <iter>; loop { match
    // __it.next() { Some(x) => body, None => break } } }`. `iterSlotName` is
    // the fresh binding that holds the iterator; `nextCall` is the
    // `__it.next()` method call (its receiver is an IdentExpr naming
    // `iterSlotName`). Ranges keep the Phase 9 direct lowering (these stay
    // unset). Held by shared_ptr / unique_ptr so the AST node owns them.
    // `mutable` so the typechecker can populate them while walking an
    // otherwise-const AST (mirrors ClosureExpr::captures).
    mutable bool iteratorDesugar = false;
    mutable std::string iterSlotName;
    mutable std::shared_ptr<MethodCallExpr> nextCall; // null unless desugar
};

// Phase 9: `break` / `break <value>`. Exits the innermost enclosing loop.
// `value` is null for a bare `break`. Only legal inside `loop`/`while`/
// `for`; `break <value>` is only meaningful inside `loop`.
struct BreakExpr : Expr {
    ExprPtr value; // may be null
};

// Phase 9: `continue`. Jumps to the innermost enclosing loop's header.
struct ContinueExpr : Expr {};

// Phase 10b: ClosureExpr is declared below, after TypeRef (its param
// annotations hold a TypeRef by value).

// --- Statements ---

struct LetStmt : Stmt {
    std::string name;
    ExprPtr value;
    // Phase 22: tuple destructuring `let (x, y) = t;`. When `tupleNames` is
    // non-empty, this let binds each name to the corresponding tuple element
    // of `value` (whose type must be a tuple of matching arity); `name` is
    // unused in that case. A `_` element drops that position (kept as the
    // literal "_" so codegen/typecheck can skip binding it). Mutually
    // exclusive with the single-`name` form. No type annotation on the
    // destructuring form (the element types come from the RHS tuple).
    std::vector<std::string> tupleNames;
    // Phase 9: `let mut x = ...`. Marks the binding as reassignable; the
    // typechecker rejects assignment to a non-mut binding.
    bool isMut = false;
    // Phase 11: optional type annotation `let x: T = ...`. When present, the
    // typechecker resolves it as the binding's expected type and coerces the
    // RHS into it (e.g. coercing `Box<Sq>` into `Box<dyn Shape>`). Null means
    // the binding's type is inferred from the RHS as before. Held by
    // shared_ptr because TypeRef is declared further down this header.
    std::shared_ptr<TypeRef> annotation; // null = no annotation
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
    // Phase 21b: an associated-type projection `Base::Assoc`, e.g. `Self::Item`
    // inside an impl method or `C::Item` at a bounded call site. When
    // `assocName` is non-empty, `name` holds the base path segment (a type
    // param like `C`, or `Self`) and `assocName` the projected member. The
    // typechecker resolves `Self::Item` (Self concretely bound) directly to the
    // impl's chosen type, and `C::Item` (C a bounded generic param) to a
    // projection Var that codegen materializes per monomorphic instance.
    // Empty for an ordinary (non-projection) type reference.
    std::string assocName;
    // v28 Phase 155 (GATs): type arguments on the projected associated type —
    // the `<i64>` in `Self::Out<i64>`. Only meaningful when `assocName` is set;
    // empty for a non-generic associated type. The resolver substitutes these
    // into the impl's `type Out<T> = ...` binding.
    std::vector<TypeRef> assocTypeArgs;
    // Phase 11: `dyn Trait` — an unsized trait-object type. When `isDyn` is
    // true, `name` holds the trait name and `typeArgs` is empty. Combine with
    // `isRef` for `&dyn Trait`, or nest in `Box<...>` for `Box<dyn Trait>`.
    bool isDyn = false;
    // Phase 13b: `&[T]` — a slice type (fat pointer viewing a contiguous
    // region). When `isSlice` is true, `typeArgs[0]` holds the element type
    // and `isRef` is set (a slice is always spelled behind `&`). `name` is
    // unused. The element MVP is i64.
    bool isSlice = false;
    // Phase 22: `[T; N]` — a fixed-size array type. When `isArray` is true,
    // `typeArgs[0]` holds the element type and `arrayLen` the (literal) length.
    // Distinct from `isSlice` (which has no length and is always behind `&`).
    bool isArray = false;
    // `mutable` (Phase 25): when the length is a const-expr (`arrayLenExpr`),
    // the typechecker evaluates it and writes the result back here so codegen
    // — which re-resolves TypeRefs independently — reads the folded length
    // directly. Mirrors the `mutable` pattern ForExpr uses for typecheck-
    // populated fields on an otherwise-const AST. A literal length is written
    // here at parse time and never mutated.
    mutable std::size_t arrayLen = 0;
    // Phase 25: const-expr array length. Phase 22 only accepted an integer
    // literal for N; this phase lifts that so N may be any compile-time
    // constant expression — a `const` item, a `const fn` call, or an
    // arithmetic expr over them (e.g. `[i64; N]`, `[i64; sq(2)]`, `[i64; A+1]`).
    // When `arrayLenExpr` is non-null the typechecker evaluates it at compile
    // time and fills `arrayLen` with the result; a bare integer literal is
    // parsed straight into `arrayLen` (arrayLenExpr stays null), so the Phase 22
    // literal path is byte-for-byte unchanged. shared_ptr keeps TypeRef
    // copyable (it is copied throughout resolveTypeRef / monomorphization).
    std::shared_ptr<Expr> arrayLenExpr; // null = literal length in arrayLen
    // Phase 22: `(A, B, ...)` — a tuple type. When `isTuple` is true,
    // `tupleElems` holds the ordered element type refs (>= 2). `name` /
    // `typeArgs` are unused. A 1-tuple isn't a type (`(T)` == `T`).
    bool isTuple = false;
    std::vector<TypeRef> tupleElems;
    // Phase 58 (v10): a const-generic VALUE supplied in type-argument
    // position — the `3` in `Mat<3>`. When `isConstArg` is true this TypeRef
    // is NOT a type but an integer literal bound to a `const N` parameter;
    // `name` / `typeArgs` are unused and the value lives in `constArgValue`.
    // The typechecker turns it into a const-value Type (TypeKind::Int with
    // `isConstValue`) that drives a distinct monomorphized instance and
    // substitutes symbolic array lengths `[T; N]`.
    bool isConstArg = false;
    long long constArgValue = 0;
    // v28 Phase 153: the const-arg's apparent type — `i64` (default, an integer
    // literal), `bool` (`true`/`false`), or `char` (a char literal). Lets the
    // typechecker check the arg matches the `const N: <type>` parameter and
    // gives the const-value Type the right kind. The value is still a long long
    // (bool 0/1, char codepoint) so monomorphization keys unchanged.
    std::string constArgTypeName = "i64";
    // Function-type fields (valid only when isFn == true).
    bool isFn = false;
    std::vector<TypeRef> fnParams;
    std::shared_ptr<TypeRef> fnRet; // shared_ptr so TypeRef stays copyable
    std::vector<std::string> fnEffects; // effect-row labels (concrete + vars)
    // Phase 145: a closure-trait bound spelled in type position —
    // `Fn(A) -> R` / `FnMut(A) -> R` / `FnOnce(A) -> R`. Parsed exactly like a
    // bare `fn(A) -> R` (same fat-pointer ABI at codegen) but tagged with the
    // required kind rank: -1 = none (a plain `fn(..)` slot accepts any kind),
    // 0 = Fn, 1 = FnMut, 2 = FnOnce. A passed closure must classify at or below
    // this rank (an `Fn` closure satisfies all bounds; an `FnMut` only
    // `FnMut`/`FnOnce`). Carried onto the Function Type as `closureBound`.
    int closureBound = -1;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 65 (v11): `operand as Type` — an explicit numeric cast, the only way to
// move between the non-coercive integer/float lattice (`i32` <-> `i64`,
// `i64` <-> `f64`, ...). Binds tighter than any binary operator but looser than
// a prefix unary, so `-x as i32` is `(-x) as i32` and `a as i32 * 2` is
// `(a as i32) * 2` (the Rust precedence). Typecheck: both `operand` and
// `targetType` must be numeric (int or f64); the result type is `targetType`.
// Codegen lowers to the width/signedness-correct LLVM cast (trunc / sext / zext
// / sitofp / uitofp / fptosi / fptoui / fpext / fptrunc). Defined here (after
// TypeRef) because it holds a TypeRef by value.
struct CastExpr : Expr {
    ExprPtr operand;
    TypeRef targetType;
};

struct Param {
    std::string name;
    TypeRef type;
};

// Phase 10b: a capturing closure `|x| x + n`, `|x, y| { ... }`, optionally
// with param type annotations `|x: i64| ...`. The closure captures by
// VALUE (copy/move, MVP — like Rust `move`) the free variables it
// references from the enclosing local scope. It is a first-class value of
// `Function` type, callable everywhere a fn value is (let-bound, passed to
// higher-order fns, selected by `if`).
//
// Parsing fills `params` (names + optional annotations) and `body` (any
// expression — typically a BlockExpr, but `|x| x + n` keeps a bare
// BinaryExpr). The typechecker fills `captures` (the resolved free
// variables, in a deterministic order); codegen reads them to lay out the
// env struct and the generated top-level `__closure_<n>` function.
struct ClosureParam {
    std::string name;
    TypeRef type;          // valid only when `hasAnnotation`
    bool hasAnnotation = false;
    std::size_t line = 1;
    std::size_t column = 1;
};

// One captured free variable: its source name plus its resolved type
// (filled by the typechecker). Codegen mirrors this order when building
// the env struct and when loading captures in the closure prologue.
//
// Phase 17a: `byRef` distinguishes a capture-by-reference (FnMut) from the
// Phase 10b capture-by-value. When the closure body ASSIGNS to (or takes
// `&mut` of) a captured `let mut` binding, the typechecker marks that
// capture `byRef`: the env slot stores a POINTER to the variable's enclosing
// alloca instead of a copied value, so reads/writes in the body go through
// the original storage and mutations persist after the call. Variables only
// read stay `byRef = false` (by value). The typechecker rejects a by-ref
// capture of a non-`mut` binding (you cannot mutate it) and rejects a
// closure with any by-ref capture from escaping its defining scope (the env
// would hold a dangling stack pointer — see checkClosure).
struct ClosureCapture {
    std::string name;
    TypePtr type;       // resolved at typecheck time
    bool byRef = false; // Phase 17a: captured by reference (FnMut)
};

// Phase 145: the Fn/FnMut/FnOnce closure-trait hierarchy. Every closure is
// classified by how it uses its captures: `Fn` only reads them (callable
// any number of times, immutably), `FnMut` mutates a by-ref capture (callable
// repeatedly, but exclusively), `FnOnce` consumes/moves a capture out of the
// env (callable once — e.g. a captured `Sender` moved into a spawned worker).
// The ranks are ordered Fn < FnMut < FnOnce: an `Fn` closure satisfies every
// bound, an `FnMut` satisfies `FnMut`/`FnOnce`, an `FnOnce` only `FnOnce`.
enum class ClosureKind { Fn = 0, FnMut = 1, FnOnce = 2 };

struct ClosureExpr : Expr {
    std::vector<ClosureParam> params;
    ExprPtr body;
    // Filled by the typechecker. Stored on the node (rather than a side
    // table) because codegen needs the ordered name+type list to build the
    // env struct and load captures; `mutable` so the checker can populate
    // it while walking an otherwise-const AST.
    mutable std::vector<ClosureCapture> captures;
    // Phase 145: the closure's classified kind (Fn/FnMut/FnOnce), computed by
    // the typechecker from its captures. `mutable` for the same const-walk
    // reason as `captures`. Read at coercion sites to check it satisfies a
    // required `Fn(..)` / `FnMut(..)` / `FnOnce(..)` parameter bound.
    mutable ClosureKind kind = ClosureKind::Fn;
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
// `T: Show` in `fn use_show<T: Show>(t: T) -> i64`. Phase 3.3 added a single
// optional trait bound (`bound`); Phase 28 adds additional bounds via
// `extraBounds`, so `T: A + B + C` (and `where T: A, T: B`) carry A in
// `bound` and B, C in `extraBounds`.
struct TypeParam {
    std::string name;
    std::string bound; // the first/primary bound; empty = unbounded
    // Phase 28: additional trait bounds beyond the primary one (`T: A + B`).
    // Plain trait names (the multi-bound case in practice — Hash + Eq — is
    // non-generic). Method dispatch on a bounded param searches `bound` then
    // each of these in order.
    std::vector<std::string> extraBounds;
    // Phase 21a: type arguments supplied to a *parameterized* trait bound,
    // e.g. the `<T>` in `<I: Iterator<T>>`. Empty when the bound trait takes
    // no type params (or there is no bound). Each TypeRef typically names
    // another of the enclosing decl's generic params (the trait's element
    // type is bound to the fn's own type param), so the typechecker resolves
    // them against the enclosing generic env. Stored on TypeParam (additive,
    // so non-generic bounds are byte-for-byte unchanged). Applies to the
    // primary `bound` only; `extraBounds` are non-parameterized.
    std::vector<TypeRef> boundTypeArgs;
    // Phase 57 (v10): a CONST-generic parameter `const N: i64` (vs a type
    // parameter). When true, `name` is the const param's name; it carries no
    // trait bounds, and its type is always i64 (the only const-generic kind).
    // A `[T; N]` length or a use-site `Foo<3>` arg binds it to a compile-time
    // integer; Phase 58 monomorphizes over that value.
    bool isConst = false;
    // v28 Phase 153: the const param's declared type name — `i64` (default),
    // `bool`, or `char`. The value is still carried as a `long long` (bool ->
    // 0/1, char -> codepoint) so the value-based monomorphization keys
    // unchanged; this drives the value-use TYPE (i64 / bool / char) + the
    // codegen width (i64 / i1 / i32). Only meaningful when `isConst`.
    std::string constTypeName = "i64";
    std::size_t line = 1;
    std::size_t column = 1;
};

struct FnDecl {
    std::string name;
    std::vector<TypeParam> genericParams; // empty = monomorphic fn
    std::vector<Param> params;
    TypeRef returnType;
    EffectRow effects; // Phase 4: declared effect row; empty = pure
    bool isAsync = false; // `async fn` returns a Future and implicitly
                            // carries the `async` effect; codegen (Phase 12)
                            // splits it into a resumable poll fn over a frame.
    bool isPub = false;   // Phase 7.3b: visible across module boundaries
                            // when referenced via path syntax.
    // Phase 25: `const fn` — a function that MAY be evaluated at compile time.
    // When called in a const context (a `const` initializer, an array length,
    // or another const fn body) with constant arguments, the const-evaluator
    // runs its body fully at compile time. It is NOT restricted to const
    // contexts: a `const fn` is also a perfectly ordinary runtime fn (its
    // codegen path is unchanged), so calling it at runtime works as before.
    bool isConst = false;
    std::unique_ptr<BlockExpr> body;
    std::string doc; // v24 Phase 134: the leading `///` doc comment, if any
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 25: a top-level compile-time constant `const NAME: T = <const-expr>;`.
// The initializer is evaluated at compile time to a constant value (i64 / bool
// in the MVP); every use of NAME resolves to that value and codegen emits the
// literal directly (no runtime load). A const may reference an earlier const
// (`const B: i64 = A + 1;`) or call a `const fn`; a cyclic / forward-bad
// reference is a clear compile error.
struct ConstDecl {
    std::string name;
    TypeRef type;
    ExprPtr value; // the const-expr initializer
    bool isPub = false; // `pub const` — parsed + stored (mirrors other decls)
    std::size_t line = 1;
    std::size_t column = 1;
};

struct StructDecl {
    std::string name;
    std::vector<TypeParam> genericParams; // empty = monomorphic struct
    std::vector<Param> fields;
    std::vector<std::string> derives; // Phase 42: `#[derive(Clone, Eq, ...)]`
    bool isPub = false; // Phase 15: `pub struct` — parsed + stored.
    std::string doc; // v24 Phase 134: the leading `///` doc comment, if any
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
    std::vector<std::string> derives; // Phase 42: `#[derive(Clone, Eq, ...)]`
    bool isPub = false; // Phase 15: `pub enum` — parsed + stored.
    std::string doc; // v24 Phase 134: the leading `///` doc comment, if any
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
    // v25 Phase 135: an optional DEFAULT method body (`fn foo(&self) -> T {…}`
    // in a trait). null = an abstract signature an impl must provide. shared_ptr
    // (not unique_ptr) so MethodSig stays copyable; the fill-defaults pass
    // deep-clones it into each impl that doesn't override it.
    std::shared_ptr<BlockExpr> body;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 21b: a trait's associated-type declaration `type Item;`. The name
// becomes a member projectable as `Self::Item` in the trait's method sigs and
// `C::Item` through a `C: Trait` bound. No default / bounds on associated types
// (`type Item: Bound` / `type Item = Default`) this phase.
struct AssocTypeDecl {
    std::string name;
    // v28 Phase 155 (GATs): generic parameters on the associated type itself —
    // the `T` in `type Out<T>;`. Empty for a plain (Phase 21b) associated type.
    std::vector<TypeParam> typeParams;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 21b: an impl's associated-type definition `type Item = i64;`. `name`
// matches a trait `AssocTypeDecl`; `type` is the concrete choice for this impl.
struct AssocTypeDef {
    std::string name;
    TypeRef type;
    // v28 Phase 155 (GATs): the binding's own generic parameters — the `T` in
    // `type Out<T> = Pair<T, T>;`. Those names are in scope in `type` (the RHS).
    // A projection `Self::Out<i64>` substitutes them with the supplied args.
    std::vector<TypeParam> typeParams;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct TraitDecl {
    std::string name;
    // Phase 21a: generic trait type parameters, e.g. the `T` in
    // `trait Iterator<T> { fn next(&mut self) -> Option<T>; }`. Empty for a
    // non-generic trait (the pre-Phase-21a shape). Method signatures may
    // mention these names in type position; the typechecker binds them to an
    // impl's concrete trait-args (or a bound's args) when checking method
    // bodies / calls. These are bound names only — bounds-on-trait-params
    // (`trait T<X: Bound>`) aren't in the grammar.
    std::vector<TypeParam> genericParams;
    // Phase 21b: associated-type declarations `type Item;`. Empty for a trait
    // with no associated types (orthogonal to genericParams — a trait may use
    // either, both, or neither).
    std::vector<AssocTypeDecl> assocTypes;
    std::vector<MethodSig> methods;
    // v25 Phase 136: SUPERTRAITS — `trait Ord: Eq + Hash { … }`. Any type that
    // impls this trait must also impl every supertrait; the typechecker enforces
    // that at the impl site. Plain trait names (no generic args in the grammar).
    std::vector<std::string> supertraits;
    bool isPub = false; // Phase 15: `pub trait` — parsed + stored.
    std::size_t line = 1;
    std::size_t column = 1;
};

// `impl TraitName for TypeRef { fn ... ... }`. The `forType` is a full
// TypeRef so it can be a generic instantiation (e.g. `Box<i64>`); the
// typechecker validates that each method's signature matches the trait's
// after substituting Self -> forType.
//
// Phase 15: inherent impls `impl TypeRef { fn ... }` (no trait) set
// `traitName` to the empty string. Their methods are registered as
// inherent methods on the type (no trait signature to match against);
// method resolution checks them alongside trait impls. Codegen mangles
// them with a fixed sentinel trait token so the existing static-dispatch
// path applies unchanged.
struct ImplDecl {
    // Phase 40: the impl's OWN generic params, e.g. the `T: Clone` in
    // `impl<T: Clone> Display for Pair<T>`. Distinct from traitTypeArgs (the
    // concrete args supplied to a parameterized trait). Each is in scope while
    // resolving forType + every method's signature/body, and behaves like an
    // extra generic param on each method, inferred from the receiver at a call.
    std::vector<TypeParam> genericParams;
    std::string traitName; // empty => inherent impl (Phase 15)
    // Phase 21a: the concrete type arguments this impl supplies to a
    // parameterized trait, e.g. the `i64` in `impl Iterator<i64> for Range`.
    // Empty for a non-generic trait impl or an inherent impl. The typechecker
    // binds the trait's genericParams[k] to traitTypeArgs[k] (Self -> forType
    // as before) while resolving each impl method's signature, so a method
    // returning `Option<T>` lands as `Option<i64>` for this impl.
    std::vector<TypeRef> traitTypeArgs;
    // Phase 21b: associated-type definitions `type Item = i64;`. Each name must
    // match one of the trait's `AssocTypeDecl`s; the typechecker validates
    // coverage. Empty for a trait with no associated types / an inherent impl.
    std::vector<AssocTypeDef> assocTypes;
    TypeRef forType;
    std::vector<FnDecl> methods;
    bool isPub = false; // Phase 15: `pub impl` — parsed + stored.
    bool isInherent() const { return traitName.empty(); }
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

// Phase 11: `Box::new(value)` — heap-allocates and moves `value` into the
// box, producing a `Box<T>` where T is value's type. Parsed as a dedicated
// node (rather than a CallExpr) because `Box` is a built-in heap pointer, not
// a user fn, and `::`-paths otherwise collapse to their last segment.
struct BoxNewExpr : Expr {
    ExprPtr value;
};

// Phase 7: `mod foo;` references — the resolver loads `foo.kd` next to
// the source file and merges its decls into the program. After resolution
// these entries are erased; downstream passes see one flat Program.
struct ModDecl {
    std::string name;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 24: an `extern "C" fn name(params) -> ret;` FFI declaration — a
// *declaration only* (no body) of a C function the program may call. The
// `"C"` ABI string is the only supported ABI; the parser records it in `abi`
// and the typechecker rejects anything else. The signature mirrors a normal
// fn (params + returnType), so calls type-check against it like any fn call.
//
// C-ABI type mapping (resolved in the typechecker + codegen):
//   - `i64`  <-> C `long` / `int64_t` (LLVM i64, no coercion).
//   - `i32`  <-> C `int`   (LLVM i32). `i32` is legal ONLY in an extern
//                signature; the kardashev-visible type is still i64 (so an
//                i64 value flows in/out), and codegen inserts a trunc on the
//                argument and a sign-extend on the result at the call
//                boundary. This is how the C `int` width is handled correctly
//                (not "lucky" upper-bit behavior).
//   - `bool` <-> C `int` / `_Bool` (LLVM i1).
//   - `&String` / `String` / `&[T]` / `&T` / `&mut T` <-> a C pointer
//                (LLVM `ptr`). For a `String` / `&String` arg the *data*
//                pointer is passed (NUL-terminated for string literals), so
//                `strlen(&s)` works.
//
// Effects: an extern call is opaque, so each extern fn carries the `io`
// effect by default (a pure caller cannot call one). An explicit `! { ... }`
// row after the return type overrides the default (e.g. `! { }` for a known-
// pure fn, or `! { io, alloc }`).
struct ExternFn {
    std::string name;
    std::vector<Param> params;
    TypeRef returnType;
    std::string abi;       // the ABI string between `extern` and `fn`; only "C"
    EffectRow effects;     // declared effect row; default {io} applied by the
    bool hasExplicitEffects = false; // typechecker when this stays false
    std::size_t line = 1;
    std::size_t column = 1;
};

// v26 Phase 146: an import / re-export. `use a::b::c;` brings `c` into scope;
// `use a::b as d;` introduces a call alias `d` for the imported item; `pub use`
// re-exports it (isReexport). Under the flat-merge module model every item is
// already globally visible by bare name, so a plain `use` is a scope hint that
// also validates the imported name resolves; the `as` form installs a working
// function-name alias (`d()` calls the imported fn). `path` holds the segments
// in order; `alias` is empty unless an `as` clause was given.
struct UseDecl {
    std::vector<std::string> path;
    std::string alias;
    bool isReexport = false;
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
    // Phase 24: user-declared `extern "C"` function declarations.
    std::vector<ExternFn> externFns;
    // Phase 25: top-level `const NAME: T = ...;` items.
    std::vector<ConstDecl> consts;
    // v26 Phase 144: top-level type aliases `type Name = Target;` (name -> the
    // aliased TypeRef). The typechecker resolves an alias name to its target.
    std::vector<std::pair<std::string, TypeRef>> typeAliases;
    // v26 Phase 146: `use a::b::c;` / `use a::b as d;` / `pub use a::b;`.
    std::vector<UseDecl> uses;
};

} // namespace kardashev::ast
