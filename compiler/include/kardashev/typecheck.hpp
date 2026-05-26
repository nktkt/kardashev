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

struct TypeCheckResult {
    std::vector<TypeError> errors;
    // Per-expression resolved type, for codegen.
    std::unordered_map<const ast::Expr*, TypePtr> exprTypes;
    // Resolved struct types keyed by struct name, for codegen layout lookup.
    std::unordered_map<std::string, TypePtr> structs;
    // Resolved enum types keyed by enum name, for codegen tagged-union layout.
    std::unordered_map<std::string, TypePtr> enums;
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
