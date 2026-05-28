// Type representation + unification for kardashev V1.
//
// Algebra:
//   Type ::= Int | Bool | Unit | Function([Type], Type) | Var(id)
//
// Var is a Hindley-Milner-style type variable. Unification uses a
// union-find link chain: when `unify(?a, T)` succeeds, ?a's `link` field
// is set to T. `resolve()` follows links (with path compression).
// `occurs(?a, T)` runs the standard occurs check to prevent infinite
// types when unifying `?a := ... ?a ...`.
//
// The full polymorphism story (let-generalization + instantiation) is
// deferred to Phase 3; V1 type-checking against fully-annotated function
// signatures rarely creates type variables in practice, but the
// machinery is in place so the same code keeps working when generics
// arrive.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kardashev {

enum class TypeKind {
    Int,
    Bool,
    Unit,
    Function,
    Var,
    Struct,
    Enum,
    Ref,
    // Phase 11: `dyn Trait` — an unsized trait object, only ever used behind
    // a pointer (`&dyn Trait` = Ref(Dyn), `Box<dyn Trait>` = Box(Dyn)). The
    // trait name is stored in `dynTraitName`. Both pointer forms lower to a
    // fat pointer `{ i8* data, i8* vtable }`.
    Dyn,
    // Phase 11: `Box<T>` — a heap-owned pointer to a T. For a concrete T this
    // lowers to an opaque pointer (heap `T*`); for `Box<dyn Trait>` (inner is
    // a Dyn) it lowers to the same fat pointer as `&dyn Trait`. The pointee
    // type is stored in `refInner` (shared with Ref — they never coexist on
    // one node).
    Box,
    // Phase 22: a fixed-size array `[T; N]` — a stack-allocated value type
    // (copied like a struct). The element type is stored in `arrayElem` and
    // the length in `arrayLen`. Lowers to an LLVM `[N x <T>]`. The MVP
    // restricts T to Copy types (i64, bool, nested arrays/tuples of those).
    Array,
    // Phase 22: an anonymous product `(A, B, ...)` — a value type (copied like
    // a struct). The element types are stored in `tupleElems` (>= 2 elements;
    // the 0-tuple `()` is unit, and `(x)` is just a parenthesized expr).
    // Lowers to an anonymous LLVM struct `{ A, B, ... }`.
    Tuple,
};

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct EnumVariantType {
    std::string name;
    std::vector<TypePtr> payloadTypes; // empty payloadTypes = unit variant
};

struct Type {
    TypeKind kind = TypeKind::Unit;

    // Function:
    std::vector<TypePtr> args;
    TypePtr ret;
    // Phase 10a: effect row carried by the function type. `effectLabels`
    // holds the row's concrete portion — built-in labels (`io`, `alloc`,
    // ...) — mirroring `FnSchema.declaredEffects` so the same string
    // vocabulary flows through first-class fn values. `effectRowVar` is an
    // optional polymorphic tail: a Type Var (Hindley-Milner row variable)
    // standing for "the rest of the effects, unknown until instantiation".
    // `{io}` is {labels=["io"], rowVar=null} (closed); `{io, e}` is
    // {labels=["io"], rowVar=Var}; `{e}` is {labels={}, rowVar=Var}; pure
    // is both empty. The row var participates in union-find exactly like a
    // type Var, so `instantiate` substitutes it by varId and `unify` binds
    // it — that is what makes a higher-order fn pure-or-effectful by its
    // argument. Effects are compile-time only: this field never reaches
    // codegen's LLVM lowering (the calling convention is unchanged).
    std::vector<std::string> effectLabels;
    TypePtr effectRowVar; // null = closed row (no polymorphic tail)

    // Var:
    int varId = -1;
    TypePtr link; // union-find link; null while unbound
    // Phase 10a: when this Var is used as an effect-row tail variable and
    // gets solved to a concrete remainder during unification, the solved
    // labels are stashed here (in this node's `effectLabels`) and this flag
    // is set. We avoid `link` for this so ordinary type-Var resolution and
    // the occurs check are unaffected; `resolveEffectRow` reads it back.
    bool effectRowSolved = false;

    // Struct:
    std::string structName;
    std::vector<std::pair<std::string, TypePtr>> structFields;

    // Enum:
    std::string enumName;
    std::vector<EnumVariantType> enumVariants;

    // Concrete type arguments at this Struct/Enum instantiation, in the
    // declaration order of the corresponding genericParams. Empty for
    // monomorphic structs/enums AND for "schema" types stored in the
    // typechecker's structs_ / enums_ tables (where field / payload
    // TypePtrs still mention the schema's generic Vars). Populated when
    // `resolveTypeRef` materializes a generic type at a use site
    // (e.g. `Box<i64>` -> typeArgs=[i64]).
    std::vector<TypePtr> typeArgs;

    // Ref (Phase 2.4b): inner type + mutability flag. `refIsMut` toggles
    // between `&T` (false) and `&mut T` (true, Phase 2.4c). Phase 11: also
    // carries the pointee for a Box (Box and Ref never share one node).
    TypePtr refInner;
    bool refIsMut = false;

    // Dyn (Phase 11): the trait name a `dyn Trait` object dispatches through.
    std::string dynTraitName;

    // Array (Phase 22): element type + compile-time-known length. Lowers to
    // an LLVM `[arrayLen x <arrayElem>]`.
    TypePtr arrayElem;
    std::size_t arrayLen = 0;

    // Tuple (Phase 22): the ordered element types of `(A, B, ...)`.
    std::vector<TypePtr> tupleElems;
};

TypePtr makeInt();
TypePtr makeBool();
TypePtr makeUnit();
// Build a function type. The 2-arg form yields a pure (empty) effect row;
// the 4-arg form attaches an explicit row (concrete labels + optional row
// Var tail). Phase 10a uses the latter to carry declared effects on first-
// class fn values and effect-row polymorphism through fn-typed params.
TypePtr makeFunction(std::vector<TypePtr> args, TypePtr ret);
TypePtr makeFunction(std::vector<TypePtr> args, TypePtr ret,
                     std::vector<std::string> effectLabels,
                     TypePtr effectRowVar);
TypePtr makeFreshVar();
TypePtr makeStruct(std::string name, std::vector<std::pair<std::string, TypePtr>> fields);
TypePtr makeEnum(std::string name, std::vector<EnumVariantType> variants);
TypePtr makeRef(TypePtr inner, bool isMut);
// Phase 11: `dyn Trait` object type and `Box<T>` heap pointer.
TypePtr makeDyn(std::string traitName);
TypePtr makeBox(TypePtr inner);
// Phase 13b: a slice `&[T]` — modeled as the built-in single-layout struct
// `Slice` with `typeArgs[0]` = element type.
TypePtr makeSlice(TypePtr elem);
// Phase 17b: a `Future<T>` — the built-in poll/frame pair carrying its result
// type in `typeArgs[0]`. Codegen lowers every Future to one `{ i8* poll, i8*
// frame }` layout; the result type only affects the per-T `Poll<T>` and the
// per-T `block_on`/`.await` value handling (sized via DataLayout, like Vec).
TypePtr makeFuture(TypePtr result);
// Phase 22: a fixed-size array `[T; N]` value type and an anonymous tuple
// `(A, B, ...)` value type. Both are copied by value like structs.
TypePtr makeArray(TypePtr elem, std::size_t len);
TypePtr makeTuple(std::vector<TypePtr> elems);

// Follow the union-find link chain to the representative. Performs
// path compression as a side effect.
TypePtr resolve(const TypePtr& t);

// Unify two types. Returns false on failure (kind mismatch, arity
// mismatch on functions, or occurs-check violation). Successful
// unification mutates the link pointers of involved type variables.
// For Function types this also unifies their effect rows (Phase 10a):
// concrete labels must agree modulo what a polymorphic row-var tail can
// absorb, and an unbound row var binds to the other row's remainder.
bool unify(const TypePtr& a, const TypePtr& b);

// Phase 10a: collect the fully-resolved concrete effect labels of a
// function type's row. Walks the closed labels plus, if a row-var tail is
// present, the labels it was solved to during unification (an unsolved row
// var contributes nothing — it is still polymorphic / treated as pure).
// `fnType` must be a resolved Function type. Order is unspecified; callers
// treat the result as a set.
std::vector<std::string> resolveEffectRow(const TypePtr& fnType);

// Pretty-print a type (after `resolve()`).
std::string typeToString(const TypePtr& t);

// Deep-copy `t`, substituting any Var whose `varId` is a key in `subst` with
// the substitution target. Vars not in `subst` are returned unchanged (sharing
// the original TypePtr — caller must not mutate it). Function types are
// reconstructed; primitive / struct / enum types are returned as-is when no
// substitution applies to them.
//
// Use cases:
//   - Type-checker: instantiate a generic fn's schema at a call site with
//     a fresh Var per generic parameter, then unify with arg types.
//   - Codegen: produce a fully-concrete signature for a monomorphized
//     instance by mapping schema Vars to concrete TypePtrs (Int/Bool/...).
//
// Substitution is non-recursive in the target: if subst[A] = B (a Var) and
// subst[B] = i64, the result still mentions B (we substitute once per Var).
// Callers requiring fixed-point substitution should compose two passes.
TypePtr instantiate(const TypePtr& t,
                    const std::unordered_map<int, TypePtr>& subst);

} // namespace kardashev
