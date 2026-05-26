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
};

struct Type;
using TypePtr = std::shared_ptr<Type>;

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
};

TypePtr makeInt();
TypePtr makeBool();
TypePtr makeUnit();
TypePtr makeFunction(std::vector<TypePtr> args, TypePtr ret);
TypePtr makeFreshVar();
TypePtr makeStruct(std::string name, std::vector<std::pair<std::string, TypePtr>> fields);

// Follow the union-find link chain to the representative. Performs
// path compression as a side effect.
TypePtr resolve(const TypePtr& t);

// Unify two types. Returns false on failure (kind mismatch, arity
// mismatch on functions, or occurs-check violation). Successful
// unification mutates the link pointers of involved type variables.
bool unify(const TypePtr& a, const TypePtr& b);

// Pretty-print a type (after `resolve()`).
std::string typeToString(const TypePtr& t);

} // namespace kardashev
