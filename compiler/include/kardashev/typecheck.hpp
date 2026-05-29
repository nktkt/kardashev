// Type checker for kardashev V1.
//
// Walks an `ast::Program` (output of `kardashev::parse`) and verifies
// that every expression is well-typed against the declared function
// signatures and annotated parameter types. Reports the first error per
// site (no recovery beyond that) into `errors`, and records the inferred
// type of every Expr in `exprTypes` for downstream consumers (codegen).
// Also stores a Maranget decision tree per MatchExpr in `matchTrees`,
// keyed by the MatchExpr's address, for codegen consumption.
//
// Built-in named types in V1: `i64`, `bool`. Other identifiers are
// looked up against the program's struct and enum declarations; anything
// not matching either reports an error.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kardashev/ast.hpp"
#include "kardashev/types.hpp"

namespace kardashev {

// Forward decl: defined in kardashev/pattern_match.hpp. Using a unique_ptr
// to an incomplete type here keeps the full pattern_match header out of
// every typecheck consumer's include graph.
namespace pattern_match {
struct DecisionTree;
} // namespace pattern_match

struct TypeError {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

// Phase 4: a function's declared / inferred effect row. Concrete built-
// in labels are `alloc`, `io`, `panic`, `async`, `unwind`; effect-row
// variables (Phase 4.3) appear here as their source-level name (e.g.
// `e`) and the typechecker validates them against the enclosing fn's
// generic-parameter list. `pure` is the empty set — i.e. omitting
// `! { ... }` on a fn.
struct EffectSet {
    std::vector<std::string> labels;
    bool contains(const std::string& l) const {
        for (const auto& x : labels) if (x == l) return true;
        return false;
    }
    void add(const std::string& l) {
        if (!contains(l)) labels.push_back(l);
    }
    void unionWith(const EffectSet& other) {
        for (const auto& l : other.labels) add(l);
    }
};

// Schema of a function as the typechecker resolved it. For monomorphic
// functions, `genericVars` is empty and `signature` is the concrete
// `fn(args...) -> ret` Type. For generic functions, `genericVars` holds the
// stable Type variables introduced for each generic parameter (one per
// `fn.genericParams`); `signature` mentions those Vars wherever the source
// referenced the parameter by name. Codegen instantiates this schema by
// substituting concrete types for the genericVars to drive monomorphization.
struct FnSchema {
    TypePtr signature;
    std::vector<TypePtr> genericVars;
    // One entry per genericVars[i]: the trait-name bound (empty for an
    // unbounded param). This is the PRIMARY bound; Phase 28 carries any
    // additional bounds (`T: A + B`) in `genericExtraBounds` below.
    std::vector<std::string> genericBounds;
    // Phase 28: one entry per genericVars[i], parallel to genericBounds: the
    // additional trait bounds beyond the primary one (the B, C in `T: A+B+C`).
    // Empty vector for an unbounded or single-bounded param. A method call on
    // a bounded param searches the primary bound then these in order.
    std::vector<std::vector<std::string>> genericExtraBounds;
    // Phase 21a: one entry per genericVars[i], parallel to genericBounds:
    // the resolved type arguments of a *parameterized* trait bound
    // (`<I: Iterator<T>>` -> the bound's `T`, resolved against this fn's own
    // generic env so it shares the schema Var for `T`). Empty vector when the
    // bound trait is non-generic / the param is unbounded. A method call on a
    // bounded param uses these to bind the trait's type params, so the
    // method's element type (e.g. `Option<T>` from `Iterator<T>::next`)
    // resolves to the fn's own `T`.
    std::vector<std::vector<TypePtr>> genericBoundArgs;
    // Phase 4: effects declared in the fn's `! { ... }` row. Empty = pure.
    // Entries are concrete built-in labels (`io`, ...) and/or effect-row
    // variable names (Phase 10a, e.g. `e`).
    EffectSet declaredEffects;
    // Phase 10a: effect-row variables this fn is generic over, mapping the
    // source name (`e`) to the schema Type Var that stands for it. These
    // Vars are a subset of `genericVars` (the ones classified as effect-row
    // vars rather than type vars). At a call site, instantiation maps each
    // to a fresh row var that unification then binds to the actual callee's
    // effects, propagating them to the caller.
    std::vector<std::pair<std::string, TypePtr>> effectRowVars;
    // Phase 7.3b: `pub fn` makes a fn callable via path syntax
    // (`foo::fn_name(args)`). Bare-name calls bypass this check
    // because Phase 7.1 still flat-merges modules.
    bool isPub = false;
    // Phase 6.1: async fn — `signature.ret` is `Future`, the wrapped
    // type that callers see. `asyncInnerType` is the declared T (the
    // type the body actually returns). Codegen emits a pair: the body
    // returning T and a wrapper that constructs Future{state, T}.
    bool isAsync = false;
    TypePtr asyncInnerType;
};

// Schema of a generic struct. For monomorphic structs, `genericVars` is
// empty. For generic structs, `type` is a Struct-kind Type whose
// `structFields` may reference the genericVars in their TypePtrs. Codegen
// monomorphizes each (structName, [concrete typeArgs]) combination
// encountered through resolved expression types.
struct StructSchema {
    TypePtr type;
    std::vector<TypePtr> genericVars;
};

// Schema of a generic enum. Same shape as StructSchema, but `type`'s
// `enumVariants` reference the genericVars in payload TypePtrs.
struct EnumSchema {
    TypePtr type;
    std::vector<TypePtr> genericVars;
};

// Per-call-site result of resolving a `receiver.method(args)` expression
// to a concrete impl. The `mangledName` directly names the LLVM function
// codegen emits for the impl method; for trait-bounded generic receivers
// it gets re-computed at codegen time once the bound's concrete type is
// known via the enclosing instance's substitution.
struct ResolvedMethod {
    // Phase 11 adds `Dyn`: a call on a `&dyn Trait` / `Box<dyn Trait>`
    // receiver, dispatched at runtime through the object's vtable rather than
    // resolved to a single impl at compile time.
    enum Kind { Concrete, BoundedGeneric, Dyn };
    Kind kind = Concrete;
    // Phase 13a: how the resolved method takes its receiver, derived from the
    // method's `self` declaration (`self` / `&self` / `&mut self`). The borrow
    // checker uses this to autoref the receiver place at the call site —
    // ByValue keeps move semantics, ByRef takes a shared borrow, ByMutRef a
    // mutable borrow — so `c.inc(); c.inc()` on a `&mut self` method no longer
    // reports `c` as moved on the second call.
    enum class SelfKind { ByValue, ByRef, ByMutRef };
    SelfKind selfKind = SelfKind::ByValue;
    // Common to all kinds:
    std::string traitName;
    std::string methodName;
    // Concrete: the implementing-type's name (e.g. "Point", "Box");
    // BoundedGeneric: the schema Var ID of the receiver's generic param.
    std::string concreteTypeName; // Concrete only
    int boundedVarId = -1;        // BoundedGeneric only
    // Dyn: the 0-based slot of `methodName` in the trait's declaration order
    // (the vtable index codegen loads + calls through).
    int dynMethodSlot = -1;       // Dyn only
    // typeArgs on the receiver type at the resolution site (so generic
    // impls — Phase 3.3 doesn't have them yet — can route correctly).
    std::vector<TypePtr> receiverTypeArgs;
};

// Phase 11: a point where a `&ConcreteType` / `Box<ConcreteType>` value is
// coerced to a `&dyn Trait` / `Box<dyn Trait>`. The typechecker records one
// per coercion site (keyed by the source Expr*); codegen turns the thin
// pointer into the fat `{ data, vtable }` pair by pairing it with the
// impl's vtable global.
struct DynCoercion {
    std::string traitName;
    std::string concreteTypeName; // the impl's implementing type
    bool isBox = false;           // false => &dyn, true => Box<dyn>
};

// Phase 21b: an unresolved associated-type projection `C::Item` where `C` is a
// trait-bounded generic param. The typechecker can't pin the concrete type
// until monomorphization, so it allocates a placeholder Type Var (the map key)
// and records how to resolve it: `baseVarId` is the schema Var of the generic
// param (`C`); `traitName` is its bound; `assocName` is the projected member.
// Codegen, at each instance, resolves `baseVarId` to a concrete type via the
// instance substitution, then looks up that type's impl's chosen associated
// type. `Self::Item` inside an impl (Self concretely known) never needs this —
// it resolves to a concrete type directly during checking.
struct AssocProjection {
    int baseVarId = -1;
    std::string traitName;
    std::string assocName;
};

// Phase 25: a compile-time-evaluated constant value (i64 or bool). Codegen
// emits these as folded LLVM literals at the use site, so a `const` reaches
// the backend as an immediate rather than a runtime load.
struct ConstFolded {
    bool isBool = false;
    std::int64_t value = 0;
};

struct TypeCheckResult {
    std::vector<TypeError> errors;
    // Per-expression resolved type, for codegen.
    std::unordered_map<const ast::Expr*, TypePtr> exprTypes;
    // Phase 25: per-Expr compile-time constant value. Populated for every use
    // of a `const` name (and for a const initializer / array-length expr).
    // Codegen reads this at an IdentExpr (and any const-folded expr) to emit
    // the literal directly.
    std::unordered_map<const ast::Expr*, ConstFolded> constExprValues;
    // Resolved struct schemas keyed by struct name. For monomorphic
    // structs the schema's `type` is the concrete struct Type and codegen
    // uses it directly; for generic structs codegen instantiates per
    // unique typeArgs combination encountered through `exprTypes`.
    std::unordered_map<std::string, StructSchema> structs;
    // Resolved enum schemas keyed by enum name.
    std::unordered_map<std::string, EnumSchema> enums;
    // Per-fn schema, for codegen monomorphization.
    std::unordered_map<std::string, FnSchema> fnSchemas;
    // Type arguments inferred at each call site to a generic function, in
    // the same order as the callee's `genericParams`. Each TypePtr is the
    // fresh Var created at the call site, possibly linked (via unification)
    // to a concrete type or to a generic Var of the enclosing fn. Codegen
    // walks these to build the monomorphization worklist. Calls to
    // monomorphic functions are NOT recorded here.
    std::unordered_map<const ast::CallExpr*, std::vector<TypePtr>>
        callInstantiations;
    // Phase 48: a qualified static call `Type::method(args)` (an associated /
    // no-self trait method, e.g. `P::default()`) — the resolved LLVM-mangled
    // impl-method name codegen should call. Absent for ordinary calls.
    std::unordered_map<const ast::CallExpr*, std::string> staticCallMangled;
    // Phase 52: a GENERIC static call `T::method()` where T is a bounded type
    // param (e.g. `T::default()` for `<T: Default>`). Codegen resolves
    // `boundedVarId` to the concrete type at the current monomorphization and
    // calls `__impl_<trait>_for_<concrete>__<method>`.
    struct StaticGenericCall {
        std::string traitName;
        std::string methodName;
        int boundedVarId = -1;
    };
    std::unordered_map<const ast::CallExpr*, StaticGenericCall>
        staticCallGeneric;
    // Per-MethodCallExpr resolution: which (trait, type) impl provides
    // the called method, plus the receiver-typeArgs context. Codegen
    // uses this to mangle the LLVM function name.
    std::unordered_map<const ast::MethodCallExpr*, ResolvedMethod>
        methodResolutions;
    // Phase 11: per-Expr coercion of a thin `&T` / `Box<T>` into a fat
    // `&dyn Trait` / `Box<dyn Trait>`. Codegen reads this at the coerced
    // expression to emit the data+vtable fat pointer.
    std::unordered_map<const ast::Expr*, DynCoercion> dynCoercions;
    // Phase 11: every (trait, concreteType) impl pair that must get a vtable
    // global emitted. Populated as coercions are discovered. Codegen walks
    // this to emit one `__vtable_<Trait>_<Type>` constant + its thunks.
    std::vector<std::pair<std::string, std::string>> dynVtablesNeeded;
    // Phase 21b: associated-type projection Vars (`C::Item`) the typechecker
    // couldn't resolve until monomorphization, keyed by the placeholder Var id.
    // Codegen resolves each via the instance substitution + `implAssocTypes`.
    std::unordered_map<int, AssocProjection> assocProjections;
    // Phase 21b: per (implementing-type-name, trait-name) the concrete TypePtr
    // each associated type resolves to. Codegen consults this to materialize a
    // resolved `AssocProjection` for the current instance.
    std::unordered_map<
        std::string,
        std::unordered_map<std::string,
                           std::unordered_map<std::string, TypePtr>>>
        implAssocTypes;
    // Global variant table: variant name -> (enumName, discriminant index).
    // Codegen reads this to map a constructor name to its enum and tag.
    // Phase 2.2 keeps variant names globally unique across all enums to
    // avoid the need for path syntax.
    std::unordered_map<std::string, std::pair<std::string, unsigned>>
        variantIndex;
    // Pre-built Maranget decision tree per MatchExpr, keyed by the
    // MatchExpr's address in the program AST. Empty for programs with no
    // match expressions, or for zero-arm matches (which short-circuit).
    std::unordered_map<const ast::MatchExpr*,
                       std::unique_ptr<pattern_match::DecisionTree>>
        matchTrees;
    // Phase 29: per match-arm, the resolved type of each pattern binding
    // (name -> type), keyed by the arm's address. Codegen uses this to drop a
    // droppable payload binding (e.g. the `s` in `Some(s)` where s: String) at
    // arm-scope exit when the arm doesn't move it out.
    std::unordered_map<const ast::MatchArm*,
                       std::unordered_map<std::string, TypePtr>>
        matchBindingTypes;
    // Phase 30: true iff the program references a file-I/O / CLI-args builtin.
    // Codegen emits that runtime (which calls libc free/fopen) only when set,
    // so I/O-free programs stay free of it.
    bool usesFileIo = false;
    bool ok() const { return errors.empty(); }

    // Special members declared out-of-line so the implicit dtor / move ops
    // are emitted in typecheck.cpp, where pattern_match::DecisionTree is
    // complete (required for `unique_ptr<DecisionTree>`).
    TypeCheckResult();
    ~TypeCheckResult();
    TypeCheckResult(TypeCheckResult&&) noexcept;
    TypeCheckResult& operator=(TypeCheckResult&&) noexcept;
    TypeCheckResult(const TypeCheckResult&) = delete;
    TypeCheckResult& operator=(const TypeCheckResult&) = delete;
};

TypeCheckResult typecheck(const ast::Program& program);

} // namespace kardashev
