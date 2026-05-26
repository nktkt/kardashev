// Unit tests for kardashev::pattern_match (Maranget exhaustiveness).
//
// Tests are constructed by hand-building AST patterns + types — no
// dependency on the parser or typechecker so this slice is verifiable
// in isolation.

#include "kardashev/pattern_match.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using kardashev::EnumVariantType;
using kardashev::TypePtr;
using kardashev::makeEnum;
using kardashev::makeInt;
using kardashev::makeStruct;
using kardashev::pattern_match::checkExhaustiveness;
using kardashev::pattern_match::compileDecisionTree;
using kardashev::pattern_match::DecisionTree;
using kardashev::pattern_match::findRedundantArms;
using kardashev::pattern_match::Witness;

namespace ast = kardashev::ast;

namespace {

// --- AST pattern builders ---

ast::PatternPtr wild() {
    return std::make_unique<ast::WildPat>();
}

ast::PatternPtr lit(std::int64_t v) {
    auto p = std::make_unique<ast::LitIntPat>();
    p->value = v;
    return p;
}

ast::PatternPtr var(std::string name) {
    auto p = std::make_unique<ast::VarPat>();
    p->name = std::move(name);
    return p;
}

ast::PatternPtr ctor(std::string name, std::vector<ast::PatternPtr> subs = {}) {
    auto p = std::make_unique<ast::CtorPat>();
    p->ctorName = std::move(name);
    p->subpatterns = std::move(subs);
    return p;
}

ast::MatchArm arm(ast::PatternPtr p) {
    ast::MatchArm a;
    a.pattern = std::move(p);
    // body is intentionally null — exhaustiveness checker doesn't need it.
    return a;
}

// --- Variant-index helpers ---

using VariantIndex =
    std::unordered_map<std::string, std::pair<std::string, unsigned>>;
using EnumMap = std::unordered_map<std::string, TypePtr>;

// Build Maybe = Some(i64) | None.
TypePtr maybeType() {
    std::vector<EnumVariantType> vs;
    EnumVariantType some;
    some.name = "Some";
    some.payloadTypes = {makeInt()};
    vs.push_back(some);
    EnumVariantType none;
    none.name = "None";
    vs.push_back(none);
    return makeEnum("Maybe", vs);
}

// Build Color = Red | Green | Blue (all unit variants).
TypePtr colorType() {
    std::vector<EnumVariantType> vs;
    EnumVariantType r; r.name = "Red"; vs.push_back(r);
    EnumVariantType g; g.name = "Green"; vs.push_back(g);
    EnumVariantType b; b.name = "Blue"; vs.push_back(b);
    return makeEnum("Color", vs);
}

struct OuterInner {
    TypePtr inner;
    TypePtr outer;
};

// Inner = A | B; Outer = O(Inner) | N.
OuterInner outerInnerTypes() {
    std::vector<EnumVariantType> innerVs;
    EnumVariantType a; a.name = "A"; innerVs.push_back(a);
    EnumVariantType b; b.name = "B"; innerVs.push_back(b);
    TypePtr inner = makeEnum("Inner", innerVs);

    std::vector<EnumVariantType> outerVs;
    EnumVariantType o; o.name = "O"; o.payloadTypes = {inner}; outerVs.push_back(o);
    EnumVariantType n; n.name = "N"; outerVs.push_back(n);
    TypePtr outer = makeEnum("Outer", outerVs);

    return {inner, outer};
}

void expectExhaustive(const TypePtr& ty,
                      std::vector<ast::MatchArm> arms,
                      const EnumMap& enums,
                      const VariantIndex& vidx,
                      const char* label) {
    auto w = checkExhaustiveness(ty, arms, enums, vidx);
    if (w) {
        std::cerr << "[" << label << "] expected exhaustive, got witness: "
                  << w->text << '\n';
        std::abort();
    }
}

void expectNonExhaustive(const TypePtr& ty,
                         std::vector<ast::MatchArm> arms,
                         const EnumMap& enums,
                         const VariantIndex& vidx,
                         const std::string& expectedWitness,
                         const char* label) {
    auto w = checkExhaustiveness(ty, arms, enums, vidx);
    if (!w) {
        std::cerr << "[" << label
                  << "] expected non-exhaustive, but result was exhaustive\n";
        std::abort();
    }
    if (w->text != expectedWitness) {
        std::cerr << "[" << label << "] witness mismatch: expected "
                  << expectedWitness << ", got " << w->text << '\n';
        std::abort();
    }
}

// --- Tests ---

void test_empty_arms_int() {
    std::vector<ast::MatchArm> arms;
    expectNonExhaustive(makeInt(), std::move(arms), {}, {}, "_",
                        "empty_arms_int");
}

void test_empty_arms_enum() {
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    // With zero arms, witness is the first missing variant — Some(_).
    expectNonExhaustive(ty, std::move(arms), em, vidx, "Some(_)",
                        "empty_arms_enum");
}

void test_single_wildcard_int() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(wild()));
    expectExhaustive(makeInt(), std::move(arms), {}, {}, "single_wildcard_int");
}

void test_single_varpat_int() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(var("x"))); // not in variantIndex → wildcard
    expectExhaustive(makeInt(), std::move(arms), {}, {}, "single_varpat_int");
}

void test_int_literals_no_wildcard() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(lit(1)));
    arms.push_back(arm(lit(2)));
    expectNonExhaustive(makeInt(), std::move(arms), {}, {}, "_",
                        "int_literals_no_wildcard");
}

void test_maybe_some_and_none() {
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(wild());
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    arms.push_back(arm(ctor("None")));
    expectExhaustive(ty, std::move(arms), em, vidx, "maybe_some_and_none");
}

void test_maybe_only_some() {
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(var("x"));
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    expectNonExhaustive(ty, std::move(arms), em, vidx, "None",
                        "maybe_only_some");
}

void test_maybe_only_none() {
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(ctor("None")));
    expectNonExhaustive(ty, std::move(arms), em, vidx, "Some(_)",
                        "maybe_only_none");
}

void test_color_all_variants() {
    auto ty = colorType();
    VariantIndex vidx;
    vidx["Red"] = {"Color", 0};
    vidx["Green"] = {"Color", 1};
    vidx["Blue"] = {"Color", 2};
    EnumMap em; em["Color"] = ty;
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(ctor("Red")));
    arms.push_back(arm(ctor("Green")));
    arms.push_back(arm(ctor("Blue")));
    expectExhaustive(ty, std::move(arms), em, vidx, "color_all_variants");
}

void test_color_missing_blue() {
    auto ty = colorType();
    VariantIndex vidx;
    vidx["Red"] = {"Color", 0};
    vidx["Green"] = {"Color", 1};
    vidx["Blue"] = {"Color", 2};
    EnumMap em; em["Color"] = ty;
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(ctor("Red")));
    arms.push_back(arm(ctor("Green")));
    expectNonExhaustive(ty, std::move(arms), em, vidx, "Blue",
                        "color_missing_blue");
}

void test_nested_outer_missing_inner_b() {
    auto oi = outerInnerTypes();
    VariantIndex vidx;
    vidx["A"] = {"Inner", 0};
    vidx["B"] = {"Inner", 1};
    vidx["O"] = {"Outer", 0};
    vidx["N"] = {"Outer", 1};
    EnumMap em; em["Inner"] = oi.inner; em["Outer"] = oi.outer;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs;
        subs.push_back(ctor("A"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    arms.push_back(arm(ctor("N")));
    expectNonExhaustive(oi.outer, std::move(arms), em, vidx, "O(B)",
                        "nested_outer_missing_inner_b");
}

void test_nested_outer_inner_both_present() {
    auto oi = outerInnerTypes();
    VariantIndex vidx;
    vidx["A"] = {"Inner", 0};
    vidx["B"] = {"Inner", 1};
    vidx["O"] = {"Outer", 0};
    vidx["N"] = {"Outer", 1};
    EnumMap em; em["Inner"] = oi.inner; em["Outer"] = oi.outer;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs;
        subs.push_back(ctor("A"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    {
        std::vector<ast::PatternPtr> subs;
        subs.push_back(ctor("B"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    arms.push_back(arm(ctor("N")));
    expectExhaustive(oi.outer, std::move(arms), em, vidx,
                     "nested_outer_inner_both_present");
}

void test_nested_outer_with_wildcard_inner() {
    auto oi = outerInnerTypes();
    VariantIndex vidx;
    vidx["A"] = {"Inner", 0};
    vidx["B"] = {"Inner", 1};
    vidx["O"] = {"Outer", 0};
    vidx["N"] = {"Outer", 1};
    EnumMap em; em["Inner"] = oi.inner; em["Outer"] = oi.outer;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs;
        subs.push_back(wild());
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    arms.push_back(arm(ctor("N")));
    expectExhaustive(oi.outer, std::move(arms), em, vidx,
                     "nested_outer_with_wildcard_inner");
}

void test_struct_with_varpat_exhaustive() {
    // Struct has no constructor signature in V1 — any wildcard/var arm
    // covers all values.
    auto ty = makeStruct("Point",
                         {{"x", makeInt()}, {"y", makeInt()}});
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(var("p")));
    expectExhaustive(ty, std::move(arms), {}, {},
                     "struct_with_varpat_exhaustive");
}

void test_varpat_unit_ctor_rewrite() {
    // Parser produces VarPat("Red") etc.; the variantIndex rewrites these
    // to unit-Ctor — exhaustiveness must therefore treat all-VarPat as
    // exhaustive when each name corresponds to a variant.
    auto ty = colorType();
    VariantIndex vidx;
    vidx["Red"] = {"Color", 0};
    vidx["Green"] = {"Color", 1};
    vidx["Blue"] = {"Color", 2};
    EnumMap em; em["Color"] = ty;
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(var("Red")));
    arms.push_back(arm(var("Green")));
    arms.push_back(arm(var("Blue")));
    expectExhaustive(ty, std::move(arms), em, vidx,
                     "varpat_unit_ctor_rewrite");
}

void test_int_literals_with_wildcard() {
    // Sanity check: literals plus a wildcard arm is exhaustive.
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(lit(1)));
    arms.push_back(arm(wild()));
    expectExhaustive(makeInt(), std::move(arms), {}, {},
                     "int_literals_with_wildcard");
}

void test_maybe_some_with_inner_int_lit_no_wildcard() {
    // match m { Some(0) => ..., None => ... } — non-exhaustive
    // because Some(1), Some(2), ... aren't covered. Witness Some(_).
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(lit(0));
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    arms.push_back(arm(ctor("None")));
    expectNonExhaustive(ty, std::move(arms), em, vidx, "Some(_)",
                        "maybe_some_with_inner_int_lit_no_wildcard");
}

// --- 2.3b: findRedundantArms helpers / tests ---

void expectRedundant(const TypePtr& ty,
                     std::vector<ast::MatchArm> arms,
                     const EnumMap& enums,
                     const VariantIndex& vidx,
                     const std::vector<unsigned>& expected,
                     const char* label) {
    auto got = findRedundantArms(ty, arms, enums, vidx);
    if (got.size() != expected.size()) {
        std::cerr << "[" << label << "] redundant count mismatch: expected "
                  << expected.size() << ", got " << got.size() << " (indices:";
        for (auto i : got) std::cerr << ' ' << i;
        std::cerr << ")\n";
        std::abort();
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (got[i] != expected[i]) {
            std::cerr << "[" << label << "] redundant index " << i
                      << " mismatch: expected " << expected[i] << ", got "
                      << got[i] << '\n';
            std::abort();
        }
    }
}

void test_redundant_single_arm() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(wild()));
    expectRedundant(makeInt(), std::move(arms), {}, {}, {},
                    "redundant_single_arm");
}

void test_redundant_second_after_wildcard() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(wild()));
    arms.push_back(arm(lit(0)));
    expectRedundant(makeInt(), std::move(arms), {}, {}, {1},
                    "redundant_second_after_wildcard");
}

void test_redundant_two_useful() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(wild()));
    expectRedundant(makeInt(), std::move(arms), {}, {}, {},
                    "redundant_two_useful");
}

void test_redundant_third_arm() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(lit(1)));
    arms.push_back(arm(lit(0))); // duplicate of arm 0 → redundant
    expectRedundant(makeInt(), std::move(arms), {}, {}, {2},
                    "redundant_third_arm");
}

void test_redundant_none_no_redundancy_non_exhaustive() {
    // Not exhaustive (no wildcard), but no arm is redundant.
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(lit(1)));
    arms.push_back(arm(lit(2)));
    expectRedundant(makeInt(), std::move(arms), {}, {}, {},
                    "redundant_none_no_redundancy_non_exhaustive");
}

void test_redundant_maybe_some_covers_some_zero() {
    // `Some(x) => ..., None => ..., Some(0) => ...` — third arm is
    // redundant: Some(x) covers everything Some(0) covers.
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(var("x"));
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    arms.push_back(arm(ctor("None")));
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(lit(0));
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    expectRedundant(ty, std::move(arms), em, vidx, {2},
                    "redundant_maybe_some_covers_some_zero");
}

void test_redundant_wildcard_then_some() {
    // `_ => ..., Some(x) => ...` — second arm is redundant (first catches all).
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(wild()));
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(var("x"));
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    expectRedundant(ty, std::move(arms), em, vidx, {1},
                    "redundant_wildcard_then_some");
}

void test_redundant_nested_outer_inner() {
    // Outer = O(Inner) | N; Inner = A | B.
    // Arms: O(A), O(_), N, O(B). Fourth arm is redundant — O(_) covers it.
    auto oi = outerInnerTypes();
    VariantIndex vidx;
    vidx["A"] = {"Inner", 0};
    vidx["B"] = {"Inner", 1};
    vidx["O"] = {"Outer", 0};
    vidx["N"] = {"Outer", 1};
    EnumMap em; em["Inner"] = oi.inner; em["Outer"] = oi.outer;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(ctor("A"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(wild());
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    arms.push_back(arm(ctor("N")));
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(ctor("B"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    expectRedundant(oi.outer, std::move(arms), em, vidx, {3},
                    "redundant_nested_outer_inner");
}

// --- 2.3c: DecisionTree tests ---

void test_dt_single_wildcard() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(wild()));
    auto t = compileDecisionTree(makeInt(), arms, {}, {});
    if (t->kind != DecisionTree::Leaf) {
        std::cerr << "[dt_single_wildcard] expected Leaf\n"; std::abort();
    }
    if (t->armIndex != 0) {
        std::cerr << "[dt_single_wildcard] expected armIndex 0\n"; std::abort();
    }
    if (!t->bindings.empty()) {
        std::cerr << "[dt_single_wildcard] expected no bindings, got "
                  << t->bindings.size() << '\n'; std::abort();
    }
}

void test_dt_single_var_int() {
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(var("x"))); // VarPat -> binding "x"
    auto t = compileDecisionTree(makeInt(), arms, {}, {});
    if (t->kind != DecisionTree::Leaf) {
        std::cerr << "[dt_single_var_int] expected Leaf\n"; std::abort();
    }
    if (t->armIndex != 0) {
        std::cerr << "[dt_single_var_int] expected armIndex 0\n"; std::abort();
    }
    if (t->bindings.size() != 1 || t->bindings[0].first != "x" ||
        !t->bindings[0].second.empty()) {
        std::cerr << "[dt_single_var_int] binding mismatch\n"; std::abort();
    }
}

void test_dt_int_zero_and_wildcard() {
    // 0 => ..., _ => ...
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(wild()));
    auto t = compileDecisionTree(makeInt(), arms, {}, {});
    if (t->kind != DecisionTree::Switch) {
        std::cerr << "[dt_int_zero_and_wildcard] expected Switch\n"; std::abort();
    }
    if (!t->occurrence.empty()) {
        std::cerr << "[dt_int_zero_and_wildcard] expected empty occurrence\n";
        std::abort();
    }
    if (t->cases.size() != 1) {
        std::cerr << "[dt_int_zero_and_wildcard] expected 1 case, got "
                  << t->cases.size() << '\n'; std::abort();
    }
    if (t->cases[0].discKind != DecisionTree::Case::DiscLit ||
        t->cases[0].litValue != 0) {
        std::cerr << "[dt_int_zero_and_wildcard] expected DiscLit 0\n"; std::abort();
    }
    if (!t->cases[0].child || t->cases[0].child->kind != DecisionTree::Leaf ||
        t->cases[0].child->armIndex != 0) {
        std::cerr << "[dt_int_zero_and_wildcard] case child not Leaf{0}\n";
        std::abort();
    }
    if (!t->defaultCase || t->defaultCase->kind != DecisionTree::Leaf ||
        t->defaultCase->armIndex != 1) {
        std::cerr << "[dt_int_zero_and_wildcard] default not Leaf{1}\n";
        std::abort();
    }
}

void test_dt_maybe_some_x_and_none() {
    // Some(x) => x, None => 0
    auto ty = maybeType();
    VariantIndex vidx;
    vidx["Some"] = {"Maybe", 0};
    vidx["None"] = {"Maybe", 1};
    EnumMap em; em["Maybe"] = ty;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(var("x"));
        arms.push_back(arm(ctor("Some", std::move(subs))));
    }
    arms.push_back(arm(ctor("None")));

    auto t = compileDecisionTree(ty, arms, em, vidx);
    if (t->kind != DecisionTree::Switch) {
        std::cerr << "[dt_maybe] expected Switch\n"; std::abort();
    }
    if (!t->occurrence.empty()) {
        std::cerr << "[dt_maybe] expected empty occurrence\n"; std::abort();
    }
    if (t->cases.size() != 2) {
        std::cerr << "[dt_maybe] expected 2 cases, got " << t->cases.size()
                  << '\n'; std::abort();
    }
    // Case 0: Some (variant 0)
    const auto& c0 = t->cases[0];
    if (c0.discKind != DecisionTree::Case::DiscCtor || c0.ctorName != "Some" ||
        c0.ctorTag != 0) {
        std::cerr << "[dt_maybe] case 0 not DiscCtor(Some, 0)\n"; std::abort();
    }
    if (!c0.child || c0.child->kind != DecisionTree::Leaf ||
        c0.child->armIndex != 0) {
        std::cerr << "[dt_maybe] case 0 child not Leaf{0}\n"; std::abort();
    }
    // The Some(x) binding should have name "x" and path {1} — payload at
    // field index 1 of the flat enum struct.
    if (c0.child->bindings.size() != 1 ||
        c0.child->bindings[0].first != "x" ||
        c0.child->bindings[0].second.size() != 1 ||
        c0.child->bindings[0].second[0] != 1) {
        std::cerr << "[dt_maybe] case 0 binding mismatch — got "
                  << c0.child->bindings.size() << " bindings\n";
        if (!c0.child->bindings.empty()) {
            std::cerr << "  first: " << c0.child->bindings[0].first
                      << " path:";
            for (auto p : c0.child->bindings[0].second) std::cerr << ' ' << p;
            std::cerr << '\n';
        }
        std::abort();
    }
    // Case 1: None (variant 1)
    const auto& c1 = t->cases[1];
    if (c1.discKind != DecisionTree::Case::DiscCtor || c1.ctorName != "None" ||
        c1.ctorTag != 1) {
        std::cerr << "[dt_maybe] case 1 not DiscCtor(None, 1)\n"; std::abort();
    }
    if (!c1.child || c1.child->kind != DecisionTree::Leaf ||
        c1.child->armIndex != 1) {
        std::cerr << "[dt_maybe] case 1 child not Leaf{1}\n"; std::abort();
    }
    if (!c1.child->bindings.empty()) {
        std::cerr << "[dt_maybe] case 1 unexpected bindings\n"; std::abort();
    }
    if (t->defaultCase) {
        std::cerr << "[dt_maybe] expected null defaultCase\n"; std::abort();
    }
}

void test_dt_nested_outer_inner() {
    // Outer = O(Inner) | N; Inner = A | B
    // Arms: O(A) => 1, O(B) => 2, N => 0
    auto oi = outerInnerTypes();
    VariantIndex vidx;
    vidx["A"] = {"Inner", 0};
    vidx["B"] = {"Inner", 1};
    vidx["O"] = {"Outer", 0};
    vidx["N"] = {"Outer", 1};
    EnumMap em; em["Inner"] = oi.inner; em["Outer"] = oi.outer;
    std::vector<ast::MatchArm> arms;
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(ctor("A"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    {
        std::vector<ast::PatternPtr> subs; subs.push_back(ctor("B"));
        arms.push_back(arm(ctor("O", std::move(subs))));
    }
    arms.push_back(arm(ctor("N")));

    auto t = compileDecisionTree(oi.outer, arms, em, vidx);
    if (t->kind != DecisionTree::Switch) {
        std::cerr << "[dt_nested] root expected Switch\n"; std::abort();
    }
    if (!t->occurrence.empty()) {
        std::cerr << "[dt_nested] root occurrence not empty\n"; std::abort();
    }
    if (t->cases.size() != 2) {
        std::cerr << "[dt_nested] expected 2 cases at root, got "
                  << t->cases.size() << '\n'; std::abort();
    }
    // Case 0: O(Inner) — inner switch on payload field at index 1.
    const auto& cO = t->cases[0];
    if (cO.ctorName != "O" || cO.ctorTag != 0) {
        std::cerr << "[dt_nested] case 0 not O,0\n"; std::abort();
    }
    if (!cO.child || cO.child->kind != DecisionTree::Switch) {
        std::cerr << "[dt_nested] O child not Switch\n"; std::abort();
    }
    if (cO.child->occurrence.size() != 1 || cO.child->occurrence[0] != 1) {
        std::cerr << "[dt_nested] inner occurrence not {1}\n"; std::abort();
    }
    if (cO.child->cases.size() != 2) {
        std::cerr << "[dt_nested] inner case count != 2\n"; std::abort();
    }
    if (cO.child->cases[0].ctorName != "A" ||
        cO.child->cases[0].ctorTag != 0 ||
        !cO.child->cases[0].child ||
        cO.child->cases[0].child->kind != DecisionTree::Leaf ||
        cO.child->cases[0].child->armIndex != 0) {
        std::cerr << "[dt_nested] inner case A wrong\n"; std::abort();
    }
    if (cO.child->cases[1].ctorName != "B" ||
        cO.child->cases[1].ctorTag != 1 ||
        !cO.child->cases[1].child ||
        cO.child->cases[1].child->kind != DecisionTree::Leaf ||
        cO.child->cases[1].child->armIndex != 1) {
        std::cerr << "[dt_nested] inner case B wrong\n"; std::abort();
    }
    if (cO.child->defaultCase) {
        std::cerr << "[dt_nested] inner default should be null\n"; std::abort();
    }
    // Case 1: N
    const auto& cN = t->cases[1];
    if (cN.ctorName != "N" || cN.ctorTag != 1 ||
        !cN.child || cN.child->kind != DecisionTree::Leaf ||
        cN.child->armIndex != 2) {
        std::cerr << "[dt_nested] N case wrong\n"; std::abort();
    }
    if (t->defaultCase) {
        std::cerr << "[dt_nested] root default should be null\n"; std::abort();
    }
}

void test_dt_empty_arms_fail() {
    // Zero arms → Fail at the root.
    std::vector<ast::MatchArm> arms;
    auto t = compileDecisionTree(makeInt(), arms, {}, {});
    if (t->kind != DecisionTree::Fail) {
        std::cerr << "[dt_empty_arms_fail] expected Fail\n"; std::abort();
    }
}

void test_dt_int_lits_no_wildcard_default_fail() {
    // `0 => ..., 1 => ...` — no wildcard, default case is Fail.
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(lit(0)));
    arms.push_back(arm(lit(1)));
    auto t = compileDecisionTree(makeInt(), arms, {}, {});
    if (t->kind != DecisionTree::Switch) {
        std::cerr << "[dt_int_lits_no_wildcard] expected Switch\n"; std::abort();
    }
    if (t->cases.size() != 2) {
        std::cerr << "[dt_int_lits_no_wildcard] expected 2 cases\n"; std::abort();
    }
    if (t->cases[0].litValue != 0 || t->cases[1].litValue != 1) {
        std::cerr << "[dt_int_lits_no_wildcard] lit values wrong\n"; std::abort();
    }
    if (!t->defaultCase || t->defaultCase->kind != DecisionTree::Fail) {
        std::cerr << "[dt_int_lits_no_wildcard] default not Fail\n"; std::abort();
    }
}

void test_dt_varpat_unit_ctor_no_binding() {
    // VarPat names that resolve to unit ctors should NOT become bindings.
    // Arms: Red, Green, Blue — all unit ctors via variantIndex rewrite.
    auto ty = colorType();
    VariantIndex vidx;
    vidx["Red"] = {"Color", 0};
    vidx["Green"] = {"Color", 1};
    vidx["Blue"] = {"Color", 2};
    EnumMap em; em["Color"] = ty;
    std::vector<ast::MatchArm> arms;
    arms.push_back(arm(var("Red")));
    arms.push_back(arm(var("Green")));
    arms.push_back(arm(var("Blue")));
    auto t = compileDecisionTree(ty, arms, em, vidx);
    if (t->kind != DecisionTree::Switch) {
        std::cerr << "[dt_varpat_unit_ctor_no_binding] expected Switch\n";
        std::abort();
    }
    if (t->cases.size() != 3) {
        std::cerr << "[dt_varpat_unit_ctor_no_binding] expected 3 cases\n";
        std::abort();
    }
    for (unsigned i = 0; i < 3; ++i) {
        const auto& c = t->cases[i];
        if (!c.child || c.child->kind != DecisionTree::Leaf ||
            c.child->armIndex != i || !c.child->bindings.empty()) {
            std::cerr << "[dt_varpat_unit_ctor_no_binding] case " << i
                      << " wrong (bindings expected empty)\n";
            std::abort();
        }
    }
    if (t->defaultCase) {
        std::cerr << "[dt_varpat_unit_ctor_no_binding] default should be null\n";
        std::abort();
    }
}

} // namespace

int main() {
    test_empty_arms_int();
    test_empty_arms_enum();
    test_single_wildcard_int();
    test_single_varpat_int();
    test_int_literals_no_wildcard();
    test_maybe_some_and_none();
    test_maybe_only_some();
    test_maybe_only_none();
    test_color_all_variants();
    test_color_missing_blue();
    test_nested_outer_missing_inner_b();
    test_nested_outer_inner_both_present();
    test_nested_outer_with_wildcard_inner();
    test_struct_with_varpat_exhaustive();
    test_varpat_unit_ctor_rewrite();
    test_int_literals_with_wildcard();
    test_maybe_some_with_inner_int_lit_no_wildcard();
    // 2.3b: redundancy
    test_redundant_single_arm();
    test_redundant_second_after_wildcard();
    test_redundant_two_useful();
    test_redundant_third_arm();
    test_redundant_none_no_redundancy_non_exhaustive();
    test_redundant_maybe_some_covers_some_zero();
    test_redundant_wildcard_then_some();
    test_redundant_nested_outer_inner();
    // 2.3c: decision tree
    test_dt_single_wildcard();
    test_dt_single_var_int();
    test_dt_int_zero_and_wildcard();
    test_dt_maybe_some_x_and_none();
    test_dt_nested_outer_inner();
    test_dt_empty_arms_fail();
    test_dt_int_lits_no_wildcard_default_fail();
    test_dt_varpat_unit_ctor_no_binding();
    std::cout << "All pattern_match tests passed (33 cases)\n";
    return 0;
}
