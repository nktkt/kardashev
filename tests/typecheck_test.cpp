// Unit tests for kardashev::typecheck.

#include "kardashev/parser.hpp"
#include "kardashev/typecheck.hpp"

#include <cassert>
#include <iostream>
#include <string>

using kardashev::parse;
using kardashev::typecheck;
using kardashev::TypeCheckResult;

namespace {

TypeCheckResult tc(const std::string& src) {
    auto pr = parse(src);
    if (!pr.ok()) {
        std::cerr << "PARSE FAILED:\n";
        for (const auto& e : pr.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": "
                      << e.message << '\n';
        }
        std::abort();
    }
    return typecheck(pr.program);
}

void dump(const TypeCheckResult& r) {
    for (const auto& e : r.errors) {
        std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                  << '\n';
    }
}

void expectOk(const std::string& src, const char* label) {
    auto r = tc(src);
    if (!r.ok()) {
        std::cerr << "[" << label << "] expected ok, got errors:\n";
        dump(r);
        std::abort();
    }
}

void expectErr(const std::string& src, const char* label) {
    auto r = tc(src);
    if (r.ok()) {
        std::cerr << "[" << label << "] expected typecheck error, none "
                     "raised\n";
        std::abort();
    }
}

void test_int_literal_return() {
    expectOk("fn f() -> i64 { 42 }", "int_literal_return");
}

void test_param_ref() {
    expectOk("fn f(x: i64) -> i64 { x }", "param_ref");
}

void test_arithmetic_chain() {
    expectOk("fn f(a: i64, b: i64) -> i64 { a + b * 2 - 1 }",
             "arithmetic_chain");
}

void test_intra_module_call() {
    expectOk(
        "fn add(a: i64, b: i64) -> i64 { a + b }\n"
        "fn use_add(x: i64) -> i64 { add(x, 5) }",
        "intra_module_call");
}

void test_forward_reference() {
    // `caller` uses `callee` which is declared later — both passes must
    // run before checking bodies.
    expectOk(
        "fn caller(x: i64) -> i64 { callee(x) }\n"
        "fn callee(x: i64) -> i64 { x + 1 }",
        "forward_reference");
}

void test_fib_full() {
    expectOk(
        "fn fib(n: i64) -> i64 {\n"
        "    if n < 2 { n } else { fib(n-1) + fib(n-2) }\n"
        "}",
        "fib_full");
}

void test_let_binding() {
    expectOk("fn f(n: i64) -> i64 { let five = 5; n + five }",
             "let_binding");
}

void test_return_stmt() {
    expectOk("fn f() -> i64 { return 42; }", "return_stmt");
}

void test_unknown_identifier() {
    expectErr("fn f() -> i64 { unknown_var }", "unknown_identifier");
}

void test_unknown_function() {
    expectErr("fn f() -> i64 { bar(1, 2) }", "unknown_function");
}

void test_wrong_arity() {
    expectErr(
        "fn add(a: i64, b: i64) -> i64 { a + b }\n"
        "fn f() -> i64 { add(1) }",
        "wrong_arity");
}

void test_if_branch_mismatch() {
    // then-branch is i64, else-branch is bool — should fail.
    expectErr(
        "fn f(x: i64) -> i64 { if x < 2 { x } else { x < 3 } }",
        "if_branch_mismatch");
}

void test_wrong_return_type() {
    // Body's tail is a comparison (bool), declared return is i64.
    expectErr("fn f(n: i64) -> i64 { n < 5 }", "wrong_return_type");
}

void test_if_cond_must_be_bool() {
    // Condition is bare i64, not a comparison.
    expectErr("fn f(n: i64) -> i64 { if n { 1 } else { 0 } }",
              "if_cond_must_be_bool");
}

void test_struct_decl_alone() {
    expectOk("struct Point { x: i64, y: i64 }", "struct_decl_alone");
}

void test_struct_decl_empty() {
    expectOk("struct Empty {}", "struct_decl_empty");
}

void test_struct_param_and_field_access() {
    expectOk(
        "struct Point { x: i64, y: i64 }\n"
        "fn f(p: Point) -> i64 { p.x + p.y }",
        "struct_param_and_field_access");
}

void test_struct_literal_value() {
    expectOk(
        "struct Point { x: i64, y: i64 }\n"
        "fn make() -> Point { Point { x: 1, y: 2 } }",
        "struct_literal_value");
}

void test_struct_decl_exposed_in_result() {
    auto pr = kardashev::parse("struct Point { x: i64, y: i64 }");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[struct_decl_exposed_in_result] expected ok, got "
                     "errors:\n";
        dump(r);
        std::abort();
    }
    assert(r.structs.count("Point") == 1);
    assert(r.structs["Point"].type->kind == kardashev::TypeKind::Struct);
    assert(r.structs["Point"].type->structFields.size() == 2);
}

void test_duplicate_struct_decl() {
    expectErr(
        "struct Point { x: i64 }\n"
        "struct Point { y: i64 }",
        "duplicate_struct_decl");
}

void test_duplicate_field_in_struct_decl() {
    expectErr("struct Point { x: i64, x: i64 }",
              "duplicate_field_in_struct_decl");
}

void test_unknown_field_in_struct_literal() {
    expectErr(
        "struct Point { x: i64, y: i64 }\n"
        "fn make() -> Point { Point { x: 1, y: 2, z: 3 } }",
        "unknown_field_in_struct_literal");
}

void test_missing_field_in_struct_literal() {
    expectErr(
        "struct Point { x: i64, y: i64 }\n"
        "fn make() -> Point { Point { x: 1 } }",
        "missing_field_in_struct_literal");
}

void test_duplicate_field_in_struct_literal() {
    expectErr(
        "struct Point { x: i64, y: i64 }\n"
        "fn make() -> Point { Point { x: 1, x: 2, y: 3 } }",
        "duplicate_field_in_struct_literal");
}

void test_struct_literal_field_type_mismatch() {
    expectErr(
        "struct Point { x: i64, y: i64 }\n"
        "fn make() -> Point { Point { x: true, y: 2 } }",
        "struct_literal_field_type_mismatch");
}

void test_field_access_on_non_struct() {
    expectErr(
        "fn f() -> i64 { let x = 1; x.foo }",
        "field_access_on_non_struct");
}

void test_field_access_missing_field() {
    expectErr(
        "struct Point { x: i64, y: i64 }\n"
        "fn f(p: Point) -> i64 { p.z }",
        "field_access_missing_field");
}

void test_typeref_unknown_struct_name() {
    expectErr(
        "fn f(p: Nope) -> i64 { 0 }",
        "typeref_unknown_struct_name");
}

// ---- Phase 2.2: enums + minimal pattern matching ----

void test_enum_decl_empty() {
    expectOk("enum Empty {}", "enum_decl_empty");
}

void test_enum_decl_unit_variants() {
    expectOk("enum Color { Red, Green, Blue }",
             "enum_decl_unit_variants");
}

void test_enum_decl_payload_variant() {
    expectOk("enum Maybe { Some(i64), None }",
             "enum_decl_payload_variant");
}

void test_enum_unwrap_or_program() {
    expectOk(
        "enum Maybe { Some(i64), None }\n"
        "fn unwrap_or(m: Maybe, d: i64) -> i64 {\n"
        "    match m { Some(x) => x, None => d }\n"
        "}",
        "enum_unwrap_or_program");
}

void test_enum_nested_ctor_pattern() {
    // Distinct variant names to avoid the Phase 2.2 global-uniqueness rule.
    expectOk(
        "enum Inner { I(i64), IN }\n"
        "enum Outer { O(Inner), ON }\n"
        "fn f(o: Outer) -> i64 {\n"
        "    match o { O(I(x)) => x, _ => 0 }\n"
        "}",
        "enum_nested_ctor_pattern");
}

void test_enum_typeref_resolution() {
    expectOk(
        "enum Maybe { Some(i64), None }\n"
        "fn id(m: Maybe) -> Maybe { m }",
        "enum_typeref_resolution");
}

void test_enum_unit_ctor_value() {
    // Bare `None` (no parens) is the value of the unit constructor.
    expectOk(
        "enum Maybe { Some(i64), None }\n"
        "fn nothing() -> Maybe { None }",
        "enum_unit_ctor_value");
}

void test_enum_ctor_call_value() {
    expectOk(
        "enum Maybe { Some(i64), None }\n"
        "fn one() -> Maybe { Some(1) }",
        "enum_ctor_call_value");
}

void test_enum_exposed_in_result() {
    auto pr = kardashev::parse(
        "enum Maybe { Some(i64), None }");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[enum_exposed_in_result] expected ok, got errors:\n";
        dump(r);
        std::abort();
    }
    assert(r.enums.count("Maybe") == 1);
    assert(r.enums["Maybe"].type->kind == kardashev::TypeKind::Enum);
    assert(r.enums["Maybe"].type->enumVariants.size() == 2);
    assert(r.variantIndex.count("Some") == 1);
    assert(r.variantIndex.count("None") == 1);
    assert(r.variantIndex["Some"].first == "Maybe");
    assert(r.variantIndex["None"].first == "Maybe");
    // Discriminants reflect source order.
    assert(r.variantIndex["Some"].second == 0);
    assert(r.variantIndex["None"].second == 1);
}

void test_enum_cross_ref_struct_holds_enum() {
    // Struct field of enum type; enum payload references separately
    // resolvable types. Validates the two-phase opaque-then-resolve pass.
    expectOk(
        "struct W { m: Maybe }\n"
        "enum Maybe { Some(i64), None }",
        "enum_cross_ref_struct_holds_enum");
}

void test_enum_cross_ref_enum_holds_struct() {
    expectOk(
        "enum E { V(P) }\n"
        "struct P { x: i64 }",
        "enum_cross_ref_enum_holds_struct");
}

void test_enum_cyclic_struct_decl_typechecks() {
    // Phase 2.2 accepts cyclic value-type decls; size analysis is out of
    // scope (codegen would diverge but typechecker is purely nominal).
    expectOk(
        "struct A { b: B }\n"
        "struct B { a: A }",
        "enum_cyclic_struct_decl_typechecks");
}

void test_enum_duplicate_decl_name() {
    expectErr(
        "enum E { A } enum E { B }",
        "enum_duplicate_decl_name");
}

void test_enum_duplicate_name_against_struct() {
    expectErr(
        "struct E { x: i64 }\n"
        "enum E { A }",
        "enum_duplicate_name_against_struct");
}

void test_enum_duplicate_variant_within_enum() {
    expectErr(
        "enum E { A, A }",
        "enum_duplicate_variant_within_enum");
}

void test_enum_duplicate_variant_across_enums() {
    expectErr(
        "enum One { A } enum Two { A }",
        "enum_duplicate_variant_across_enums");
}

void test_enum_ctor_call_wrong_arity_too_many() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f() -> Maybe { Some(1, 2) }",
        "enum_ctor_call_wrong_arity_too_many");
}

void test_enum_ctor_call_wrong_payload_type() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f() -> Maybe { Some(true) }",
        "enum_ctor_call_wrong_payload_type");
}

void test_enum_unit_ctor_called_with_args() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f() -> Maybe { None(1) }",
        "enum_unit_ctor_called_with_args");
}

void test_enum_payload_ctor_bare_ident() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f() -> Maybe { Some }",
        "enum_payload_ctor_bare_ident");
}

void test_match_zero_arms() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 { match m {} }",
        "match_zero_arms");
}

void test_match_arm_body_type_mismatch() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { Some(x) => x, None => true }\n"
        "}",
        "match_arm_body_type_mismatch");
}

void test_match_pattern_arity_mismatch() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { Some(x, y) => x, None => 0 }\n"
        "}",
        "match_pattern_arity_mismatch");
}

void test_match_unknown_ctor_in_pattern() {
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { Nope(x) => 0, _ => 1 }\n"
        "}",
        "match_unknown_ctor_in_pattern");
}

void test_match_duplicate_binding_in_pattern() {
    expectErr(
        "enum Pair { P(i64, i64) }\n"
        "fn f(p: Pair) -> i64 {\n"
        "    match p { P(x, x) => x }\n"
        "}",
        "match_duplicate_binding_in_pattern");
}

void test_match_integer_literal_pattern() {
    expectOk(
        "fn f(n: i64) -> i64 {\n"
        "    match n { 0 => 1, _ => 2 }\n"
        "}",
        "match_integer_literal_pattern");
}

void test_match_var_binding_pattern() {
    // A bare Ident pattern is a variable binding when not a known ctor.
    expectOk(
        "fn f(n: i64) -> i64 {\n"
        "    match n { x => x + 1 }\n"
        "}",
        "match_var_binding_pattern");
}

void test_match_integer_pattern_on_enum_errors() {
    // Integer literal pattern on an enum scrutinee fails to unify.
    expectErr(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { 0 => 1, _ => 2 }\n"
        "}",
        "match_integer_pattern_on_enum_errors");
}

// ---- Phase 2.3a: exhaustiveness ----

void expectErrContains(const std::string& src, const std::string& needle,
                       const char* label) {
    auto r = tc(src);
    if (r.ok()) {
        std::cerr << "[" << label << "] expected typecheck error, none "
                     "raised\n";
        std::abort();
    }
    for (const auto& e : r.errors) {
        if (e.message.find(needle) != std::string::npos) return;
    }
    std::cerr << "[" << label << "] expected error containing \"" << needle
              << "\", got:\n";
    dump(r);
    std::abort();
}

void test_exhaustive_missing_none() {
    expectErrContains(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { Some(x) => x, }\n"
        "}",
        "None",
        "exhaustive_missing_none");
}

void test_exhaustive_missing_some() {
    expectErrContains(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { None => 0, }\n"
        "}",
        "Some(_)",
        "exhaustive_missing_some");
}

void test_exhaustive_missing_wildcard_int() {
    expectErrContains(
        "fn f(n: i64) -> i64 {\n"
        "    match n { 0 => 0, 1 => 1, }\n"
        "}",
        "_",
        "exhaustive_missing_wildcard_int");
}

void test_exhaustive_int_with_wildcard_ok() {
    expectOk(
        "fn f(n: i64) -> i64 {\n"
        "    match n { 0 => 0, 1 => 1, _ => 99 }\n"
        "}",
        "exhaustive_int_with_wildcard_ok");
}

void test_exhaustive_missing_color() {
    expectErrContains(
        "enum Color { Red, Green, Blue }\n"
        "fn f(c: Color) -> i64 {\n"
        "    match c { Red => 1, Green => 2, }\n"
        "}",
        "Blue",
        "exhaustive_missing_color");
}

void test_exhaustive_color_full_ok() {
    expectOk(
        "enum Color { Red, Green, Blue }\n"
        "fn f(c: Color) -> i64 {\n"
        "    match c { Red => 1, Green => 2, Blue => 3 }\n"
        "}",
        "exhaustive_color_full_ok");
}

void test_exhaustive_nested_missing_inner() {
    expectErrContains(
        "enum I { A, B }\n"
        "enum O { O(I), N }\n"
        "fn f(o: O) -> i64 {\n"
        "    match o { O(A) => 1, N => 0, }\n"
        "}",
        "O(B)",
        "exhaustive_nested_missing_inner");
}

void test_exhaustive_var_binding_catchall_ok() {
    expectOk(
        "fn f(n: i64) -> i64 {\n"
        "    match n { x => x }\n"
        "}",
        "exhaustive_var_binding_catchall_ok");
}

void test_match_records_unified_result_type() {
    auto pr = kardashev::parse(
        "enum Maybe { Some(i64), None }\n"
        "fn unwrap_or(m: Maybe, d: i64) -> i64 {\n"
        "    match m { Some(x) => x, None => d }\n"
        "}");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[match_records_unified_result_type] expected ok, got "
                     "errors:\n";
        dump(r);
        std::abort();
    }
    // Spot-check: the match expression appears in exprTypes with type i64.
    bool sawMatch = false;
    for (const auto& kv : r.exprTypes) {
        if (dynamic_cast<const kardashev::ast::MatchExpr*>(kv.first)) {
            sawMatch = true;
            assert(kardashev::resolve(kv.second)->kind ==
                   kardashev::TypeKind::Int);
        }
    }
    assert(sawMatch);
}

// ---- Phase 2.3b: redundancy ----

void test_redundant_two_wildcards() {
    // Arm 1 (the second `_`) is unreachable; the first wildcard catches all.
    // Match expression layout: "match n { _ => 1, _ => 2 }" — arm 1 starts at
    // the second `_`. Source column hand-computed from the literal below.
    // line 2: "    match n { _ => 1, _ => 2 }"
    //                                ^-- this is arm 1's pattern
    // columns: 1=' ' 2=' ' 3=' ' 4=' ' 5='m' ... let's compute precisely.
    auto r = tc(
        "fn f(n: i64) -> i64 {\n"
        "    match n { _ => 1, _ => 2 }\n"
        "}");
    bool sawRedundancy = false;
    for (const auto& e : r.errors) {
        if (e.message.find("unreachable match arm") != std::string::npos) {
            sawRedundancy = true;
            // The redundant arm is the second one (arm index 1), so report
            // on line 2 at the second `_`'s column.
            assert(e.line == 2);
        }
    }
    if (!sawRedundancy) {
        std::cerr << "[redundant_two_wildcards] expected unreachable arm "
                     "error, got:\n";
        dump(r);
        std::abort();
    }
}

void test_redundant_var_then_wildcard() {
    // Arm 1 is unreachable: arm 0's VarPat `x` already catches every i64.
    auto r = tc(
        "fn f(n: i64) -> i64 {\n"
        "    match n { x => 1, _ => 2 }\n"
        "}");
    bool sawRedundancy = false;
    for (const auto& e : r.errors) {
        if (e.message.find("unreachable match arm") != std::string::npos) {
            sawRedundancy = true;
            assert(e.line == 2);
        }
    }
    if (!sawRedundancy) {
        std::cerr << "[redundant_var_then_wildcard] expected unreachable "
                     "arm error, got:\n";
        dump(r);
        std::abort();
    }
}

void test_redundant_duplicate_literal() {
    // Arm 1 (the second `0 => 2`) is unreachable: arm 0 already matched 0.
    auto r = tc(
        "fn f(n: i64) -> i64 {\n"
        "    match n { 0 => 1, 0 => 2, _ => 3 }\n"
        "}");
    int sawCount = 0;
    for (const auto& e : r.errors) {
        if (e.message.find("unreachable match arm") != std::string::npos) {
            ++sawCount;
            assert(e.line == 2);
        }
    }
    if (sawCount == 0) {
        std::cerr << "[redundant_duplicate_literal] expected unreachable "
                     "arm error, got:\n";
        dump(r);
        std::abort();
    }
}

void test_redundant_literal_after_wildcard() {
    // Arm 2 (`1 => 3`) is unreachable: the wildcard at arm 1 absorbs all i64.
    auto r = tc(
        "fn f(n: i64) -> i64 {\n"
        "    match n { 0 => 1, _ => 2, 1 => 3 }\n"
        "}");
    bool saw = false;
    for (const auto& e : r.errors) {
        if (e.message.find("unreachable match arm") != std::string::npos) {
            saw = true;
            assert(e.line == 2);
        }
    }
    if (!saw) {
        std::cerr << "[redundant_literal_after_wildcard] expected error, "
                     "got:\n";
        dump(r);
        std::abort();
    }
}

void test_redundant_some_wild_then_some_zero() {
    // Arm 1 (`Some(0) => 2`) is unreachable: `Some(_)` in arm 0 covers it.
    auto r = tc(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { Some(_) => 1, Some(0) => 2, None => 3 }\n"
        "}");
    bool saw = false;
    for (const auto& e : r.errors) {
        if (e.message.find("unreachable match arm") != std::string::npos) {
            saw = true;
            assert(e.line == 3);
        }
    }
    if (!saw) {
        std::cerr << "[redundant_some_wild_then_some_zero] expected error, "
                     "got:\n";
        dump(r);
        std::abort();
    }
}

void test_redundant_none_some_no_redundancy() {
    // Sanity check: `Some(_) => 1, None => 0` has no redundancy.
    expectOk(
        "enum Maybe { Some(i64), None }\n"
        "fn f(m: Maybe) -> i64 {\n"
        "    match m { Some(_) => 1, None => 0 }\n"
        "}",
        "redundant_none_some_no_redundancy");
}

void test_redundant_color_full_no_redundancy() {
    expectOk(
        "enum Color { Red, Green, Blue }\n"
        "fn f(c: Color) -> i64 {\n"
        "    match c { Red => 1, Green => 2, Blue => 3 }\n"
        "}",
        "redundant_color_full_no_redundancy");
}

void test_redundant_wildcard_before_color() {
    // Arm 1 (`Red => 2`) is unreachable: the wildcard catches every Color.
    auto r = tc(
        "enum Color { Red, Green, Blue }\n"
        "fn f(c: Color) -> i64 {\n"
        "    match c { _ => 1, Red => 2 }\n"
        "}");
    bool saw = false;
    for (const auto& e : r.errors) {
        if (e.message.find("unreachable match arm") != std::string::npos) {
            saw = true;
            assert(e.line == 3);
        }
    }
    if (!saw) {
        std::cerr << "[redundant_wildcard_before_color] expected error, "
                     "got:\n";
        dump(r);
        std::abort();
    }
}

// ---- Phase 2.3c: decision tree storage ----

void test_decision_tree_stored_for_match() {
    // After typechecking a program containing a match, the result must
    // expose a decision tree keyed by that MatchExpr's address.
    auto pr = kardashev::parse(
        "enum Maybe { Some(i64), None }\n"
        "fn unwrap_or(m: Maybe, d: i64) -> i64 {\n"
        "    match m { Some(x) => x, None => d }\n"
        "}");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[decision_tree_stored_for_match] expected ok, got "
                     "errors:\n";
        dump(r);
        std::abort();
    }
    // At least one decision tree was stored (and it's keyed by a real
    // MatchExpr* present in exprTypes).
    assert(r.matchTrees.size() >= 1);
    bool keyMatchesExprTypes = false;
    for (const auto& kv : r.matchTrees) {
        if (r.exprTypes.count(static_cast<const kardashev::ast::Expr*>(
                kv.first)) == 1) {
            keyMatchesExprTypes = true;
            // The stored DT itself must be non-null.
            assert(kv.second != nullptr);
        }
    }
    assert(keyMatchesExprTypes);
}

void test_decision_tree_count_matches_match_count() {
    // Two match expressions in the same program → two decision trees.
    auto pr = kardashev::parse(
        "enum Maybe { Some(i64), None }\n"
        "fn a(m: Maybe) -> i64 { match m { Some(x) => x, None => 0 } }\n"
        "fn b(n: i64) -> i64 { match n { 0 => 1, _ => 2 } }");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[decision_tree_count_matches_match_count] expected "
                     "ok, got errors:\n";
        dump(r);
        std::abort();
    }
    assert(r.matchTrees.size() == 2);
}

// ---- Phase 3.1: generic functions ----

void test_generic_identity_ok() {
    // Calling id<T> with an i64 arg should infer T = i64.
    expectOk(
        "fn id<T>(x: T) -> T { x }\n"
        "fn main() -> i64 { id(42) }",
        "generic_identity_ok");
}

void test_generic_two_params_ok() {
    // Two independent generic params; return picks A.
    expectOk(
        "fn pair<A, B>(a: A, b: B) -> A { a }\n"
        "fn run() -> i64 { pair(7, 8) }",
        "generic_two_params_ok");
}

void test_generic_calls_generic_ok() {
    // A generic fn calling another generic fn: U flows through to T via id.
    expectOk(
        "fn id<T>(x: T) -> T { x }\n"
        "fn wrap<U>(u: U) -> U { id(u) }\n"
        "fn run() -> i64 { wrap(9) }",
        "generic_calls_generic_ok");
}

void test_generic_with_struct_arg_ok() {
    // Instantiating T with a struct type and then accessing a field on the
    // result of id(p) exercises Var-resolution through the call site.
    expectOk(
        "struct P { x: i64 }\n"
        "fn id<T>(x: T) -> T { x }\n"
        "fn run() -> i64 { let p = P { x: 7 }; id(p).x }",
        "generic_with_struct_arg_ok");
}

void test_generic_dup_param_error() {
    // Duplicate generic parameter name within the same fn is rejected at
    // schema-registration time (Pass 1b).
    expectErr(
        "fn bad<T, T>(x: T) -> T { x }",
        "generic_dup_param_error");
}

void test_generic_schema_recorded() {
    // The result's fnSchemas map should record id with one generic Var.
    auto pr = kardashev::parse("fn id<T>(x: T) -> T { x }");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[generic_schema_recorded] expected ok, got errors:\n";
        dump(r);
        std::abort();
    }
    auto it = r.fnSchemas.find("id");
    assert(it != r.fnSchemas.end());
    assert(it->second.genericVars.size() == 1);
}

// ---- Phase 3.2: generic structs and enums ----

void test_generic_struct_decl_ok() {
    auto pr = kardashev::parse("struct Box<T> { v: T }");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[generic_struct_decl_ok] expected ok, got errors:\n";
        dump(r);
        std::abort();
    }
    auto it = r.structs.find("Box");
    assert(it != r.structs.end());
    assert(it->second.genericVars.size() == 1);
}

void test_generic_struct_use_ok() {
    expectOk(
        "struct Box<T> { v: T }\n"
        "fn main() -> i64 { let b = Box { v: 42 }; b.v }",
        "generic_struct_use_ok");
}

void test_generic_enum_decl_ok() {
    auto pr = kardashev::parse("enum Option<T> { Some(T), None }");
    assert(pr.ok());
    auto r = kardashev::typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[generic_enum_decl_ok] expected ok, got errors:\n";
        dump(r);
        std::abort();
    }
    auto it = r.enums.find("Option");
    assert(it != r.enums.end());
    assert(it->second.genericVars.size() == 1);
}

void test_generic_enum_match_ok() {
    expectOk(
        "enum Option<T> { Some(T), None }\n"
        "fn unwrap_or(o: Option<i64>, def: i64) -> i64 {\n"
        "    match o { Some(v) => v, None => def }\n"
        "}",
        "generic_enum_match_ok");
}

void test_generic_type_arity_mismatch() {
    expectErr(
        "struct Box<T> { v: T }\n"
        "fn f(b: Box<i64, bool>) -> i64 { 0 }",
        "generic_type_arity_mismatch");
}

void test_generic_type_unknown_param() {
    expectErr(
        "struct Box<T> { v: T }\n"
        "fn f(b: Box) -> i64 { 0 }",
        "generic_type_unknown_param");
}

// ---- Phase 3.3: traits, impls, method calls, bounded generics ----

void test_trait_basic_ok() {
    expectOk(
        "trait Show { fn show(self) -> i64; }\n"
        "struct Point { x: i64, y: i64 }\n"
        "impl Show for Point { fn show(self) -> i64 { self.x + self.y } }\n"
        "fn main() -> i64 { let p = Point { x: 3, y: 4 }; p.show() }",
        "trait_basic_ok");
}

void test_bounded_generic_fn_ok() {
    expectOk(
        "trait Show { fn show(self) -> i64; }\n"
        "struct Point { x: i64, y: i64 }\n"
        "impl Show for Point { fn show(self) -> i64 { self.x + self.y } }\n"
        "fn use_show<T: Show>(t: T) -> i64 { t.show() }\n"
        "fn main() -> i64 {\n"
        "    let p = Point { x: 3, y: 4 };\n"
        "    use_show(p)\n"
        "}",
        "bounded_generic_fn_ok");
}

void test_method_on_unbounded_generic_errors() {
    // No bound on T, so `t.show()` cannot be resolved.
    expectErr(
        "trait Show { fn show(self) -> i64; }\n"
        "fn f<T>(t: T) -> i64 { t.show() }",
        "method_on_unbounded_generic_errors");
}

void test_method_not_in_trait_errors() {
    // T is bounded by Show, but `nope` is not a method of Show.
    expectErr(
        "trait Show { fn show(self) -> i64; }\n"
        "fn f<T: Show>(t: T) -> i64 { t.nope() }",
        "method_not_in_trait_errors");
}

void test_impl_missing_method_errors() {
    expectErr(
        "trait Show { fn show(self) -> i64; }\n"
        "struct P { x: i64 }\n"
        "impl Show for P { }",
        "impl_missing_method_errors");
}

// ---- Phase 3.4: postfix `?` (try) operator ----

void test_try_basic_ok() {
    // Enclosing fn returns a Result-shape enum; `parse(n)?` extracts the Ok
    // payload (i64) and propagates Err.
    expectOk(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "fn parse(n: i64) -> Result<i64, i64> { Ok(n) }\n"
        "fn use_it(n: i64) -> Result<i64, i64> {\n"
        "    let x = parse(n)?;\n"
        "    Ok(x)\n"
        "}",
        "try_basic_ok");
}

void test_try_outside_fn_with_result_errors() {
    // Enclosing fn returns i64, not a Result-shape enum → `?` is illegal here.
    expectErr(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "fn parse(n: i64) -> Result<i64, i64> { Ok(n) }\n"
        "fn caller(n: i64) -> i64 {\n"
        "    let x = parse(n)?;\n"
        "    x\n"
        "}",
        "try_outside_fn_with_result_errors");
}

void test_try_on_non_result_enum_errors() {
    // Operand is an enum without an Err variant.
    expectErr(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "enum Foo { A(i64), B }\n"
        "fn f() -> Result<i64, i64> {\n"
        "    let x = A(7)?;\n"
        "    Ok(x)\n"
        "}",
        "try_on_non_result_enum_errors");
}

void test_try_err_payload_mismatch_errors() {
    // Operand's Err carries i64; enclosing fn's Err carries bool — mismatch.
    expectErr(
        "enum ResI { Ok(i64), Err(i64) }\n"
        "enum ResB { Ok(i64), Err(bool) }\n"
        "fn produce(n: i64) -> ResI { Ok(n) }\n"
        "fn consume(n: i64) -> ResB {\n"
        "    let x = produce(n)?;\n"
        "    Ok(x)\n"
        "}",
        "try_err_payload_mismatch_errors");
}

// --- Phase 4: effect labels ---

void test_pure_fn_no_effects_ok() {
    expectOk("fn add(a: i64, b: i64) -> i64 { a + b }\n"
             "fn main() -> i64 { add(2, 3) }",
             "pure_fn_no_effects_ok");
}

void test_callee_io_caller_declares_io_ok() {
    expectOk("fn raw_read() -> i64 ! { io } { 42 }\n"
             "fn main() -> i64 ! { io, alloc } { raw_read() }",
             "callee_io_caller_declares_io_ok");
}

void test_callee_io_caller_pure_errors() {
    expectErr("fn raw_read() -> i64 ! { io } { 42 }\n"
              "fn main() -> i64 { raw_read() }",
              "callee_io_caller_pure_errors");
}

void test_undeclared_effect_label_errors() {
    // `foo` isn't a built-in and isn't declared as a generic param.
    expectErr("fn bad() -> i64 ! { foo } { 0 }\n"
              "fn main() -> i64 { 0 }",
              "undeclared_effect_label_errors");
}

void test_multiple_effects_union_ok() {
    expectOk("fn alloc_one() -> i64 ! { alloc } { 1 }\n"
             "fn io_one() -> i64 ! { io } { 2 }\n"
             "fn both() -> i64 ! { io, alloc } { alloc_one() + io_one() }\n"
             "fn main() -> i64 ! { io, alloc } { both() }",
             "multiple_effects_union_ok");
}

void test_effect_row_recorded_in_schema() {
    auto r = tc("fn f() -> i64 ! { io, alloc } { 0 }");
    if (!r.ok()) {
        std::cerr << "[effect_row_recorded_in_schema] tc failed\n";
        dump(r);
        std::abort();
    }
    auto it = r.fnSchemas.find("f");
    assert(it != r.fnSchemas.end());
    assert(it->second.declaredEffects.contains("io"));
    assert(it->second.declaredEffects.contains("alloc"));
    assert(!it->second.declaredEffects.contains("panic"));
}

// --- Phase 9: loops, ranges, assignment, mutability ---

void test_while_basic_ok() {
    expectOk("fn f() -> i64 { let mut i = 0; while i < 3 { i = i + 1; } i }",
             "while_basic_ok");
}

void test_while_cond_must_be_bool() {
    expectErrContains(
        "fn f() -> i64 { while 1 { } 0 }", "must be bool",
        "while_cond_must_be_bool");
}

void test_while_is_unit() {
    // The `while` expression itself is unit, so it can't be the i64 tail.
    expectErr("fn f() -> i64 { while 1 < 2 { } }", "while_is_unit");
}

void test_loop_break_value_typed() {
    expectOk("fn f() -> i64 { let x = loop { break 7; }; x }",
             "loop_break_value_typed");
}

void test_loop_break_value_mismatch() {
    // Mixing a valued `break` and a valueless `break` in the same loop is
    // inconsistent: the loop can't be both i64-valued and unit.
    expectErrContains(
        "fn f() -> i64 { let x = loop { if 1 < 2 { break 7; } else {} "
        "break; }; x }",
        "both `break` with and without a value",
        "loop_break_value_mismatch");
}

void test_break_value_in_while_errors() {
    expectErrContains(
        "fn f() -> i64 { while 1 < 2 { break 5; } 0 }",
        "only allowed inside `loop`", "break_value_in_while");
}

void test_break_outside_loop_errors() {
    expectErrContains("fn f() -> i64 { break; 0 }", "outside of a loop",
                      "break_outside_loop");
}

void test_continue_outside_loop_errors() {
    expectErrContains("fn f() -> i64 { continue; 0 }", "outside of a loop",
                      "continue_outside_loop");
}

void test_for_range_ok() {
    expectOk(
        "fn f() -> i64 { let mut s = 0; for x in 1..=10 { s = s + x; } s }",
        "for_range_ok");
}

void test_for_exclusive_range_ok() {
    expectOk(
        "fn f() -> i64 { let mut s = 0; for x in 0..5 { s = s + x; } s }",
        "for_exclusive_range_ok");
}

void test_assign_to_immutable_errors() {
    expectErrContains("fn f() -> i64 { let x = 0; x = 1; x }",
                      "not a mutable place", "assign_to_immutable");
}

void test_assign_type_mismatch_errors() {
    // `b` is bool; assigning an i64 should fail to unify.
    expectErr("fn f() -> i64 { let mut b = 1 < 2; b = 5; 0 }",
              "assign_type_mismatch");
}

void test_range_endpoints_must_be_int() {
    expectErrContains("fn f() -> i64 { for x in (1 < 2)..5 { } 0 }",
                      "range start must be i64",
                      "range_endpoints_must_be_int");
}

void test_loop_io_effect_propagates() {
    // A `print` inside a loop body must surface the io effect requirement.
    expectErrContains(
        "fn f() -> i64 { let mut i = 0; while i < 1 { print(i); i = i + 1; } "
        "0 }",
        "io", "loop_io_effect_propagates");
}

void test_nested_loops_ok() {
    expectOk(
        "fn f() -> i64 {\n"
        "  let mut t = 0;\n"
        "  let mut i = 0;\n"
        "  while i < 2 {\n"
        "    let mut j = 0;\n"
        "    while j < 2 { j = j + 1; if j == 1 { continue; } else {} t = t + 1; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  t\n"
        "}",
        "nested_loops_ok");
}

} // namespace

int main() {
    test_int_literal_return();
    test_param_ref();
    test_arithmetic_chain();
    test_intra_module_call();
    test_forward_reference();
    test_fib_full();
    test_let_binding();
    test_return_stmt();
    test_unknown_identifier();
    test_unknown_function();
    test_wrong_arity();
    test_if_branch_mismatch();
    test_wrong_return_type();
    test_if_cond_must_be_bool();
    test_struct_decl_alone();
    test_struct_decl_empty();
    test_struct_param_and_field_access();
    test_struct_literal_value();
    test_struct_decl_exposed_in_result();
    test_duplicate_struct_decl();
    test_duplicate_field_in_struct_decl();
    test_unknown_field_in_struct_literal();
    test_missing_field_in_struct_literal();
    test_duplicate_field_in_struct_literal();
    test_struct_literal_field_type_mismatch();
    test_field_access_on_non_struct();
    test_field_access_missing_field();
    test_typeref_unknown_struct_name();
    // Phase 2.2
    test_enum_decl_empty();
    test_enum_decl_unit_variants();
    test_enum_decl_payload_variant();
    test_enum_unwrap_or_program();
    test_enum_nested_ctor_pattern();
    test_enum_typeref_resolution();
    test_enum_unit_ctor_value();
    test_enum_ctor_call_value();
    test_enum_exposed_in_result();
    test_enum_cross_ref_struct_holds_enum();
    test_enum_cross_ref_enum_holds_struct();
    test_enum_cyclic_struct_decl_typechecks();
    test_enum_duplicate_decl_name();
    test_enum_duplicate_name_against_struct();
    test_enum_duplicate_variant_within_enum();
    test_enum_duplicate_variant_across_enums();
    test_enum_ctor_call_wrong_arity_too_many();
    test_enum_ctor_call_wrong_payload_type();
    test_enum_unit_ctor_called_with_args();
    test_enum_payload_ctor_bare_ident();
    test_match_zero_arms();
    test_match_arm_body_type_mismatch();
    test_match_pattern_arity_mismatch();
    test_match_unknown_ctor_in_pattern();
    test_match_duplicate_binding_in_pattern();
    test_match_integer_literal_pattern();
    test_match_var_binding_pattern();
    test_match_integer_pattern_on_enum_errors();
    test_match_records_unified_result_type();
    // Phase 2.3a exhaustiveness
    test_exhaustive_missing_none();
    test_exhaustive_missing_some();
    test_exhaustive_missing_wildcard_int();
    test_exhaustive_int_with_wildcard_ok();
    test_exhaustive_missing_color();
    test_exhaustive_color_full_ok();
    test_exhaustive_nested_missing_inner();
    test_exhaustive_var_binding_catchall_ok();
    // Phase 2.3b redundancy + 2.3c decision-tree storage
    test_redundant_two_wildcards();
    test_redundant_var_then_wildcard();
    test_redundant_duplicate_literal();
    test_redundant_literal_after_wildcard();
    test_redundant_some_wild_then_some_zero();
    test_redundant_none_some_no_redundancy();
    test_redundant_color_full_no_redundancy();
    test_redundant_wildcard_before_color();
    test_decision_tree_stored_for_match();
    test_decision_tree_count_matches_match_count();
    // Phase 3.1 generic functions
    test_generic_identity_ok();
    test_generic_two_params_ok();
    test_generic_calls_generic_ok();
    test_generic_with_struct_arg_ok();
    test_generic_dup_param_error();
    test_generic_schema_recorded();
    // Phase 3.2 generic structs and enums
    test_generic_struct_decl_ok();
    test_generic_struct_use_ok();
    test_generic_enum_decl_ok();
    test_generic_enum_match_ok();
    test_generic_type_arity_mismatch();
    test_generic_type_unknown_param();
    // Phase 3.3 traits + impl + bounded generics
    test_trait_basic_ok();
    test_bounded_generic_fn_ok();
    test_method_on_unbounded_generic_errors();
    test_method_not_in_trait_errors();
    test_impl_missing_method_errors();
    // Phase 3.4 try operator
    test_try_basic_ok();
    test_try_outside_fn_with_result_errors();
    test_try_on_non_result_enum_errors();
    test_try_err_payload_mismatch_errors();
    // Phase 4 effect labels
    test_pure_fn_no_effects_ok();
    test_callee_io_caller_declares_io_ok();
    test_callee_io_caller_pure_errors();
    test_undeclared_effect_label_errors();
    test_multiple_effects_union_ok();
    test_effect_row_recorded_in_schema();
    // Phase 9 loops + ranges + assignment + mutability
    test_while_basic_ok();
    test_while_cond_must_be_bool();
    test_while_is_unit();
    test_loop_break_value_typed();
    test_loop_break_value_mismatch();
    test_break_value_in_while_errors();
    test_break_outside_loop_errors();
    test_continue_outside_loop_errors();
    test_for_range_ok();
    test_for_exclusive_range_ok();
    test_assign_to_immutable_errors();
    test_assign_type_mismatch_errors();
    test_range_endpoints_must_be_int();
    test_loop_io_effect_propagates();
    test_nested_loops_ok();
    std::cout << "All typecheck tests passed (116 cases)\n";
    return 0;
}
