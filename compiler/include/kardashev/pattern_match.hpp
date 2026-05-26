// Pattern-match exhaustiveness check for kardashev V1.
//
// Implements Maranget's usefulness algorithm (from "Compiling Pattern
// Matching to Good Decision Trees", 2008) to detect non-exhaustive
// `match` expressions and synthesize a missing-pattern witness.
//
// This library is intentionally typecheck-independent: callers pass in
// the scrutinee type plus the program's enum table and variant index so
// the typecheck library can later depend on this one (and not the other
// way around).

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kardashev/ast.hpp"
#include "kardashev/types.hpp"

namespace kardashev::pattern_match {

struct Witness {
    // Pretty-printed example pattern not covered by the arms. e.g. "None",
    // "Some(_)", "O(B)", or "_" if the scrutinee type is infinite.
    std::string text;
};

// Returns nullopt if `arms` exhaustively cover all values of
// `scrutineeType`; otherwise returns a Witness with a missing-pattern
// example.
//
// `enums` and `variantIndex` come from TypeCheckResult (passed in to
// avoid a circular library dep on typecheck).
std::optional<Witness> checkExhaustiveness(
    const TypePtr& scrutineeType,
    const std::vector<ast::MatchArm>& arms,
    const std::unordered_map<std::string, TypePtr>& enums,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex);

// --- 2.3b: redundancy ---
// Returns a sorted vector of arm indices that are unreachable given the
// arms before them. arm i is redundant iff useful(M[0..i-1], M[i]) is false.
std::vector<unsigned> findRedundantArms(
    const TypePtr& scrutineeType,
    const std::vector<ast::MatchArm>& arms,
    const std::unordered_map<std::string, TypePtr>& enums,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex);

// --- 2.3c: decision tree ---
struct DecisionTree {
    enum Kind { Leaf, Fail, Switch };
    Kind kind = Fail;

    // --- Leaf only ---
    unsigned armIndex = 0;
    // Pattern-variable bindings introduced by this arm.
    // Each binding: { var name, occurrence path } where the path is a
    // sequence of field indices to ExtractValue from the scrutinee.
    // Empty path means "the scrutinee value itself".
    std::vector<std::pair<std::string, std::vector<unsigned>>> bindings;

    // --- Switch only ---
    // Path to the subvalue to dispatch on. Empty = root scrutinee.
    std::vector<unsigned> occurrence;

    // Per-case dispatch arm.
    struct Case {
        enum DiscKind { DiscLit, DiscCtor };
        DiscKind discKind = DiscLit;
        std::int64_t litValue = 0;                    // when DiscLit
        std::string ctorName;                          // when DiscCtor
        unsigned ctorTag = 0;                          // when DiscCtor: variant index
        std::unique_ptr<DecisionTree> child;
    };
    std::vector<Case> cases;
    // For incomplete signatures (e.g. Int with no wildcard arm — but then
    // useful would say "fail" — or for explicit non-exhaustive patterns
    // that the typechecker still passed through). Null if signature complete.
    std::unique_ptr<DecisionTree> defaultCase;
};

// Build the Maranget decision tree for `arms` against `scrutineeType`.
// Always returns a tree, even if exhaustiveness already failed — the tree
// then bottoms out at Fail nodes for uncovered values. (The codegen layer
// will emit `unreachable` for Fail.)
std::unique_ptr<DecisionTree> compileDecisionTree(
    const TypePtr& scrutineeType,
    const std::vector<ast::MatchArm>& arms,
    const std::unordered_map<std::string, TypePtr>& enums,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex);

} // namespace kardashev::pattern_match
