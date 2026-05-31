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
    Float, // Phase 39: f64, lowering to LLVM `double`
    Bool,
    // v27 Phase 147: `char` — a Unicode scalar value (0..=0x10FFFF, no
    // surrogates). A DISTINCT type from the integer tower (no arithmetic; only
    // equality/ordering and an explicit `as` cast bridge it to/from integers).
    // Lowers to an LLVM i32 holding the codepoint.
    Char,
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

    // Int (v11 — the numeric tower): a machine integer's BIT WIDTH (8/16/32/64)
    // and SIGNEDNESS. The default `makeInt()` is i64-signed, so every pre-v11
    // site is byte-for-byte the same i64 it always was; other widths/signs are
    // DISTINCT types — there is NO implicit widening, only an explicit `as`
    // cast bridges them (Phase 65). An unsuffixed integer literal is i64 by
    // default and NARROWS to a concrete width at a coercion site
    // (`narrowIntLiteral`), so the type system carries zero literal churn.
    int intWidth = 64;
    bool intSigned = true;
    // Reserved for a future inference-var literal scheme (set only on a Var);
    // unused in Phase 63 — the narrowing approach above is used instead.
    bool intLitVar = false;

    // Float (Phase 67): a float's BIT WIDTH — 64 (f64, LLVM `double`, the
    // default) or 32 (f32, LLVM `float`). Like the int tower, f32 and f64 are
    // DISTINCT non-coercive types — only an explicit `as` cast bridges them —
    // and an unsuffixed float literal is f64 by default, narrowing to f32 in
    // context. makeFloat() is f64, so every pre-Phase-67 site is unchanged.
    int floatWidth = 64;

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
    // Phase 145: a closure-trait bound carried by a `Fn(..)`/`FnMut(..)`/
    // `FnOnce(..)` parameter type — the required maximum closure kind rank
    // (0 = Fn, 1 = FnMut, 2 = FnOnce). -1 = no bound (a bare `fn(..)` value
    // type, which accepts a closure of any kind). Purely a compile-time check
    // (coerceOrUnify); never reaches codegen — the fat-pointer ABI is shared
    // with plain fn values, so a bound type lowers identically.
    int closureBound = -1;

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
    // Phase 57 (v10): a SYMBOLIC array length `[T; N]` where N is a
    // const-generic parameter in scope (empty = concrete `arrayLen`). The
    // length isn't known until the type is instantiated; Phase 58 substitutes
    // this name with the supplied const value to recover a concrete arrayLen.
    std::string arrayLenParam;

    // Phase 58 (v10): a const-generic VALUE used as a type argument — the `3`
    // in `Mat<3>`. Modeled as a `TypeKind::Int` node with `isConstValue` set
    // and the integer in `constValue`. It only ever appears inside a
    // struct/enum/fn instance's `typeArgs`; it is mangled by value (so
    // `Mat<3>` and `Mat<5>` become DISTINCT monomorphized LLVM types) and is
    // the source value that substitutes a symbolic array length `[T; N]`.
    bool isConstValue = false;
    long long constValue = 0;
    // Phase 61 (v10): a SYMBOLIC const-generic argument — the `CAP` in `impl<..,
    // const CAP> Clone for RingBuffer<T, CAP>` or the `C` in `transpose() ->
    // Matrix<C, R>`. When non-empty this const value's `constValue` is unknown;
    // it names a const param in scope and is resolved to a concrete value at
    // the leaf monomorphization (codegen's currentConstParamSubst_).
    std::string constValueName;

    // Tuple (Phase 22): the ordered element types of `(A, B, ...)`.
    std::vector<TypePtr> tupleElems;
};

TypePtr makeInt();
// v11: a sized/signed machine integer. makeInt() == makeIntW(64, true) == i64.
TypePtr makeIntW(int width, bool isSigned);
// v11: a fresh integer-LITERAL inference var (unifies with any concrete int,
// defaults to i64). The type of an unsuffixed integer literal.
TypePtr makeIntLitVar();
// v11: the canonical name (`i32`, `u8`, ...) of an Int type, for diagnostics.
std::string intTypeName(int width, bool isSigned);
TypePtr makeFloat();
// Phase 67: a sized float. makeFloat() == makeFloatW(64) == f64; makeFloatW(32)
// is f32. The canonical name is `f32` / `f64`.
TypePtr makeFloatW(int width);
std::string floatTypeName(int width);
TypePtr makeBool();
TypePtr makeChar(); // v27 Phase 147: the `char` type (Unicode scalar)
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

// Phase 58 (v10): a const-generic VALUE argument (the `3` in `Mat<3>`). A
// fresh `TypeKind::Int` node carrying `isConstValue` + the integer; lives
// only inside a struct/enum/fn instance's `typeArgs`.
TypePtr makeConstValue(long long v);

// Phase 61 (v10): a SYMBOLIC const-generic argument naming a const param in
// scope (value resolved at the leaf monomorphization).
TypePtr makeConstSymbol(std::string name);

// Phase 58 (v10): materialize a generic struct/enum instance from a schema.
// Splits `typeArgs` (in declaration order) into a type-Var substitution and a
// const-param name->length map (using `constParamNames`, one entry per
// genericVars position, empty = type param), substitutes the type Vars in the
// fields, resolves symbolic array lengths `[T; N]` to the bound const values,
// and stamps `typeArgs` onto a guaranteed-fresh node. `isStruct` selects the
// struct vs enum freshness clone. Shared by the typechecker and codegen so
// both agree on the monomorphic identity (`Mat<3>` -> distinct from `Mat<5>`).
TypePtr instantiateGeneric(const TypePtr& schemaType,
                           const std::vector<TypePtr>& genericVars,
                           const std::vector<std::string>& constParamNames,
                           std::vector<TypePtr> typeArgs, bool isStruct);

// Phase 58 (v10): deep-copy `t`, replacing every symbolic array length
// `[T; N]` (an `Array` whose `arrayLenParam` is a key in `lengths`) with its
// concrete length. Used by monomorphization to materialize a generic type's
// fields once a `const N` parameter is bound to a value. Recursive types are
// cycle-guarded; nodes with no symbolic length are returned unchanged.
TypePtr substituteConstLengths(
    const TypePtr& t,
    const std::unordered_map<std::string, std::size_t>& lengths);

// Phase 61 (v10): deep-copy `t`, RENAMING every symbolic const (an array length
// `[T; N]` or a const-value typeArg) whose name is a key in `renames` to the new
// (still symbolic) name. Used when forwarding a const-generic array into another
// const-generic fn: the callee's `[i64; N]` is rebound to the caller's `[i64; M]`.
TypePtr renameConstLengths(
    const TypePtr& t,
    const std::unordered_map<std::string, std::string>& renames);

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
