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

    // Var:
    int varId = -1;
    TypePtr link; // union-find link; null while unbound

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
};

TypePtr makeInt();
TypePtr makeBool();
TypePtr makeUnit();
TypePtr makeFunction(std::vector<TypePtr> args, TypePtr ret);
TypePtr makeFreshVar();
TypePtr makeStruct(std::string name, std::vector<std::pair<std::string, TypePtr>> fields);
TypePtr makeEnum(std::string name, std::vector<EnumVariantType> variants);

// Follow the union-find link chain to the representative. Performs
// path compression as a side effect.
TypePtr resolve(const TypePtr& t);

// Unify two types. Returns false on failure (kind mismatch, arity
// mismatch on functions, or occurs-check violation). Successful
// unification mutates the link pointers of involved type variables.
bool unify(const TypePtr& a, const TypePtr& b);

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
