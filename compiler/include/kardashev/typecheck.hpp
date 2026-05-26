// Type checker for kardashev V1.
//
// Walks an `ast::Program` (output of `kardashev::parse`) and verifies
// that every expression is well-typed against the declared function
// signatures and annotated parameter types. Reports the first error per
// site (no recovery beyond that) into `errors`, and records the inferred
// type of every Expr in `exprTypes` for downstream consumers (codegen).
//
// Built-in named types in V1: `i64`, `bool`. Other identifiers are
// looked up against the program's struct declarations; anything not
// matching either reports an error.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "kardashev/ast.hpp"
#include "kardashev/types.hpp"

namespace kardashev {

struct TypeError {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct TypeCheckResult {
    std::vector<TypeError> errors;
    // Per-expression resolved type, for codegen.
    std::unordered_map<const ast::Expr*, TypePtr> exprTypes;
    // Resolved struct types keyed by struct name, for codegen layout lookup.
    std::unordered_map<std::string, TypePtr> structs;
    bool ok() const { return errors.empty(); }
};

TypeCheckResult typecheck(const ast::Program& program);

} // namespace kardashev
