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

// ---- Phase 21a: generic trait parameters ----

// A generic trait with impls at two element types; calling `.first()` on each
// yields the correctly-typed value (i64 used as i64, bool used in `if`).
void test_generic_trait_two_impls_ok() {
    expectOk(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }\n"
        "impl Container<bool> for BoolBox { fn first(&self) -> bool { self.b } }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 1 };\n"
        "    let bb = BoolBox { b: true };\n"
        "    let x = ib.first();\n"
        "    if bb.first() { x } else { 0 }\n"
        "}",
        "generic_trait_two_impls_ok");
}

// A generic-trait-bounded fn: `head<T, C: Container<T>>(c: C) -> T` resolves
// the element type through the bound at both element types.
void test_generic_trait_bounded_fn_ok() {
    expectOk(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }\n"
        "impl Container<bool> for BoolBox { fn first(&self) -> bool { self.b } }\n"
        "fn head<T, C: Container<T>>(c: C) -> T { c.first() }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 7 };\n"
        "    let bb = BoolBox { b: false };\n"
        "    let a = head(ib);\n"
        "    if head(bb) { a } else { a }\n"
        "}",
        "generic_trait_bounded_fn_ok");
}

// The bound's element type must flow: returning the wrong-typed value from a
// `Container<bool>` impl method (an i64 where bool is declared) is rejected.
void test_generic_trait_impl_wrong_elem_errors() {
    expectErr(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container<bool> for BoolBox { fn first(&self) -> bool { self.b } }\n"
        "fn use_it(c: BoolBox) -> i64 { c.first() }",
        "generic_trait_impl_wrong_elem_errors");
}

// An impl supplying the wrong NUMBER of trait type-args is rejected.
void test_generic_trait_impl_arity_errors() {
    expectErr(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct B { v: i64 }\n"
        "impl Container<i64, i64> for B { fn first(&self) -> i64 { self.v } }",
        "generic_trait_impl_arity_errors");
}

// A parameterized bound with the wrong arity is rejected.
void test_parameterized_bound_arity_errors() {
    expectErr(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "fn head<T, C: Container<T, T>>(c: C) -> T { c.first() }",
        "parameterized_bound_arity_errors");
}

// Phase 49: a generic trait CAN be a trait object when its type args are
// supplied; a BARE `dyn Iterator` (no `<...>`) for a 1-param trait is an arity
// mismatch and still rejected.
void test_dyn_generic_trait_rejected() {
    expectErr(
        "trait Iterator<T> { fn next(&mut self) -> T; }\n"
        "fn use_it(it: &dyn Iterator) -> i64 { 0 }",
        "dyn_generic_trait_rejected");
}

// Phase 49: a PARAMETERIZED trait object `dyn Producer<i64>` typechecks — the
// trait arg binds the method's return type, and dispatch goes through the
// vtable.
void test_dyn_parameterized_trait_ok() {
    expectOk(
        "trait Producer<T> { fn produce(&self) -> T; }\n"
        "struct G { v: i64 }\n"
        "impl Producer<i64> for G { fn produce(&self) -> i64 { self.v } }\n"
        "fn run(p: &dyn Producer<i64>) -> i64 { p.produce() }\n"
        "fn main() -> i64 { let g = G { v: 7 }; run(&g) }",
        "dyn_parameterized_trait_ok");
}

// Regression: a NON-generic trait used as `dyn` still works.
void test_dyn_nongeneric_trait_still_ok() {
    expectOk(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side * self.side } }\n"
        "fn describe(s: &dyn Shape) -> i64 { s.area() }\n"
        "fn main() -> i64 { let sq = Sq { side: 5 }; describe(&sq) }",
        "dyn_nongeneric_trait_still_ok");
}

// A generic Iterator<T> bound carrying a fn-typed param whose effect row is a
// generic effect-row var still composes (effects keep flowing through the
// generic-trait bound). `fold` is `! {e}` and inherits the closure's effects.
void test_generic_trait_bound_effects_compose() {
    expectOk(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator<T> { fn next(&mut self) -> Option<T>; }\n"
        "fn fold<T, I: Iterator<T>, e>(it: I, init: i64,"
        " f: fn(i64, T) -> i64 ! {e}) -> i64 ! {e} {\n"
        "    let mut iter = it;\n"
        "    let mut acc = init;\n"
        "    loop {\n"
        "        match iter.next() {\n"
        "            Some(x) => { acc = f(acc, x); },\n"
        "            None => { break; },\n"
        "        }\n"
        "    }\n"
        "    acc\n"
        "}",
        "generic_trait_bound_effects_compose");
}

// --- Phase 21b: where clauses ---

// A `where C: Container<T>` fn type-checks exactly like the inline-bounded one
// (the bound was desugared onto the param at parse time).
void test_where_clause_bounded_fn_ok() {
    expectOk(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct IntBox { v: i64 }\n"
        "impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }\n"
        "fn head<T, C>(c: C) -> T where C: Container<T> { c.first() }\n"
        "fn main() -> i64 { let ib = IntBox { v: 7 }; head(ib) }",
        "where_clause_bounded_fn_ok");
}

// A multi-constraint `where` over two distinct bounded params.
void test_where_clause_multi_ok() {
    expectOk(
        "trait Getter { fn get(&self) -> i64; }\n"
        "trait Show { fn show(&self) -> i64; }\n"
        "struct IB { v: i64 }\n"
        "impl Getter for IB { fn get(&self) -> i64 { self.v } }\n"
        "struct Tag { t: i64 }\n"
        "impl Show for Tag { fn show(&self) -> i64 { self.t } }\n"
        "fn combine<G, S>(g: G, s: S) -> i64 where G: Getter, S: Show {\n"
        "    g.get() + s.show()\n"
        "}\n"
        "fn main() -> i64 { combine(IB { v: 40 }, Tag { t: 2 }) }",
        "where_clause_multi_ok");
}

// A `where`-bounded call still enforces the bound: calling a method the bound
// trait doesn't declare is rejected (the bound flowed onto the param).
void test_where_clause_bound_enforced() {
    expectErr(
        "trait Getter { fn get(&self) -> i64; }\n"
        "fn f<C>(c: C) -> i64 where C: Getter { c.nonexistent() }",
        "where_clause_bound_enforced");
}

// --- Phase 21b: associated types ---

// `trait Container { type Item; fn get(&self) -> Self::Item; }` with two impls
// choosing different Item types; both impl methods + a use type-check.
void test_assoc_self_item_two_impls_ok() {
    expectOk(
        "trait Container { type Item; fn get(&self) -> Self::Item; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container for IntBox { type Item = i64; fn get(&self) -> "
        "Self::Item { self.v } }\n"
        "impl Container for BoolBox { type Item = bool; fn get(&self) -> "
        "Self::Item { self.b } }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 1 };\n"
        "    let bb = BoolBox { b: true };\n"
        "    if bb.get() { ib.get() } else { 0 }\n"
        "}",
        "assoc_self_item_two_impls_ok");
}

// `C::Item` at a bounded call site resolves to each impl's associated type.
void test_assoc_c_item_bounded_ok() {
    expectOk(
        "trait Container { type Item; fn get(&self) -> Self::Item; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container for IntBox { type Item = i64; fn get(&self) -> "
        "Self::Item { self.v } }\n"
        "impl Container for BoolBox { type Item = bool; fn get(&self) -> "
        "Self::Item { self.b } }\n"
        "fn first<C: Container>(c: C) -> C::Item { c.get() }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 1 };\n"
        "    let bb = BoolBox { b: true };\n"
        "    if first(bb) { first(ib) } else { 0 }\n"
        "}",
        "assoc_c_item_bounded_ok");
}

// An impl that omits a declared associated type is rejected.
void test_assoc_impl_missing_item_errors() {
    expectErr(
        "trait Container { type Item; fn get(&self) -> Self::Item; }\n"
        "struct IntBox { v: i64 }\n"
        "impl Container for IntBox { fn get(&self) -> Self::Item { self.v } }",
        "assoc_impl_missing_item_errors");
}

// An impl that defines an associated type the trait does not declare is
// rejected.
void test_assoc_impl_extra_item_errors() {
    expectErr(
        "trait Container { fn get(&self) -> i64; }\n"
        "struct IntBox { v: i64 }\n"
        "impl Container for IntBox { type Item = i64; fn get(&self) -> i64 "
        "{ self.v } }",
        "assoc_impl_extra_item_errors");
}

// A `C::Item` projection on an UNBOUNDED generic param is rejected (no bound to
// resolve the associated type through).
void test_assoc_projection_unbounded_errors() {
    expectErr(
        "trait Container { type Item; fn get(&self) -> Self::Item; }\n"
        "fn first<C>(c: C) -> C::Item { c.get() }",
        "assoc_projection_unbounded_errors");
}

// A duplicate associated-type declaration in a trait is rejected.
void test_assoc_dup_decl_errors() {
    expectErr(
        "trait Container { type Item; type Item; }",
        "assoc_dup_decl_errors");
}

// ---- Phase 11: dyn Trait + trait objects ----

void test_dyn_ref_param_ok() {
    expectOk(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side * self.side } }\n"
        "fn describe(s: &dyn Shape) -> i64 { s.area() }\n"
        "fn main() -> i64 { let sq = Sq { side: 5 }; describe(&sq) }",
        "dyn_ref_param_ok");
}

void test_dyn_box_ok() {
    expectOk(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side * self.side } }\n"
        "fn main() -> i64 {\n"
        "    let b: Box<dyn Shape> = Box::new(Sq { side: 6 });\n"
        "    b.area()\n"
        "}",
        "dyn_box_ok");
}

void test_dyn_coercion_two_impls_ok() {
    expectOk(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "struct Rect { w: i64, h: i64 }\n"
        "impl Shape for Sq   { fn area(&self) -> i64 { self.side * self.side } }\n"
        "impl Shape for Rect { fn area(&self) -> i64 { self.w * self.h } }\n"
        "fn describe(s: &dyn Shape) -> i64 { s.area() }\n"
        "fn main() -> i64 {\n"
        "    let sq = Sq { side: 5 };\n"
        "    let r  = Rect { w: 3, h: 4 };\n"
        "    describe(&sq) + describe(&r)\n"
        "}",
        "dyn_coercion_two_impls_ok");
}

void test_dyn_unknown_trait_errors() {
    expectErr(
        "struct S { x: i64 }\n"
        "fn f(s: &dyn Nope) -> i64 { 0 }",
        "dyn_unknown_trait_errors");
}

void test_dyn_unsafe_static_method_errors() {
    // A trait with a method that has no `self` receiver is not dyn-safe and
    // must be rejected when used as `dyn`.
    expectErr(
        "trait Maker { fn make() -> i64; }\n"
        "fn use_it(m: &dyn Maker) -> i64 { 0 }\n"
        "fn main() -> i64 { 0 }",
        "dyn_unsafe_static_method_errors");
}

void test_dyn_coerce_unimpl_type_errors() {
    // Passing a `&Concrete` whose type does NOT impl the trait must fail.
    expectErr(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "struct Other { z: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side } }\n"
        "fn describe(s: &dyn Shape) -> i64 { s.area() }\n"
        "fn main() -> i64 { let o = Other { z: 1 }; describe(&o) }",
        "dyn_coerce_unimpl_type_errors");
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

// --- Phase 10a: effect-carrying function types + effect-row polymorphism ---

// The README capstone: a higher-order fn polymorphic over its argument's
// effect row. `apply` is pure when given a pure fn, effectful when given an
// effectful one.
static const char* kApplyPrelude =
    "fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }\n"
    "fn pureInc(x: i64) -> i64 { x + 1 }\n"
    "fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }\n";

void test_effect_poly_apply_pure_ok() {
    expectOk(std::string(kApplyPrelude) +
                 "fn usePure() -> i64 { apply(pureInc) }",
             "effect_poly_apply_pure_ok");
}

void test_effect_poly_apply_io_ok() {
    expectOk(std::string(kApplyPrelude) +
                 "fn useIo() -> i64 ! {io} { apply(ioInc) }",
             "effect_poly_apply_io_ok");
}

void test_effect_poly_apply_io_leak_errors() {
    // Pure signature, but `e := {io}` leaks io through apply: must error.
    expectErr(std::string(kApplyPrelude) +
                  "fn useIoBad() -> i64 { apply(ioInc) }",
              "effect_poly_apply_io_leak_errors");
}

void test_effect_poly_apply_io_caller_declares_more_ok() {
    // Caller may declare a superset of the propagated effects.
    expectOk(std::string(kApplyPrelude) +
                 "fn useIo() -> i64 ! {io, alloc} { apply(ioInc) }",
             "effect_poly_apply_io_caller_declares_more_ok");
}

void test_concrete_fn_type_param_io_ok() {
    // A concrete (non-polymorphic) fn-type param `fn(i64)->i64 ! {io}`: the
    // body's indirect call performs io, so the caller must declare it.
    expectOk("fn callIt(f: fn(i64) -> i64 ! {io}) -> i64 ! {io} { f(3) }\n"
             "fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }\n"
             "fn main() -> i64 ! {io} { callIt(ioInc) }",
             "concrete_fn_type_param_io_ok");
}

void test_concrete_fn_type_param_pure_body_errors() {
    // The fn-type param declares io, so calling it inside a pure-bodied
    // higher-order fn must error.
    expectErr("fn callIt(f: fn(i64) -> i64 ! {io}) -> i64 { f(3) }\n"
              "fn main() -> i64 { 0 }",
              "concrete_fn_type_param_pure_body_errors");
}

void test_fn_value_preserves_effects_via_let() {
    // A first-class fn value bound with `let` keeps its declared effects:
    // calling it requires the caller to declare them.
    expectErr("fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }\n"
              "fn main() -> i64 { let g = ioInc; g(5) }",
              "fn_value_preserves_effects_via_let_errors");
}

void test_fn_value_preserves_effects_via_let_ok() {
    expectOk("fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }\n"
             "fn main() -> i64 ! {io} { let g = ioInc; g(5) }",
             "fn_value_preserves_effects_via_let_ok");
}

void test_pure_fn_value_via_let_ok() {
    // A pure fn value imposes no effect obligation on a pure caller.
    expectOk("fn inc(x: i64) -> i64 { x + 1 }\n"
             "fn main() -> i64 { let g = inc; g(5) }",
             "pure_fn_value_via_let_ok");
}

void test_fn_type_param_arg_must_match_effects() {
    // Passing a pure fn where the param type fixes `io` is fine (pure ⊆ io);
    // passing an io fn where the param is pure must be rejected, because the
    // body would then be allowed to call a pure fn yet the caller passed an
    // effectful one — the effect rows don't unify.
    expectErr("fn takesPure(f: fn(i64) -> i64) -> i64 { f(1) }\n"
              "fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }\n"
              "fn main() -> i64 { takesPure(ioInc) }",
              "fn_type_param_arg_must_match_effects");
}

void test_effect_row_var_classified_in_schema() {
    auto r = tc("fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }");
    if (!r.ok()) {
        std::cerr << "[effect_row_var_classified_in_schema] tc failed\n";
        dump(r);
        std::abort();
    }
    auto it = r.fnSchemas.find("apply");
    assert(it != r.fnSchemas.end());
    // `e` is an effect-row variable (appears in `! { ... }`), so it is
    // recorded in effectRowVars and listed among declaredEffects by name.
    assert(it->second.effectRowVars.size() == 1);
    assert(it->second.effectRowVars[0].first == "e");
    assert(it->second.declaredEffects.contains("e"));
}

void test_generic_param_type_and_effect_conflict_errors() {
    // `e` used in both a type position and an effect position: rejected.
    expectErr("fn bad<e>(x: e, f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(1) }\n"
              "fn main() -> i64 { 0 }",
              "generic_param_type_and_effect_conflict_errors");
}

void test_higher_order_type_var_and_effect_var_together_ok() {
    // Mixed generics: `T` is a type var, `e` an effect-row var. Both roles
    // coexist cleanly in one signature.
    expectOk("fn run<T, e>(x: T, f: fn(T) -> T ! {e}) -> T ! {e} { f(x) }\n"
             "fn dbl(x: i64) -> i64 { x + x }\n"
             "fn main() -> i64 { run(21, dbl) }",
             "higher_order_type_var_and_effect_var_together_ok");
}

void test_implicit_row_var_in_fn_type_ok() {
    // A non-built-in label in a fn-type effect row is an (implicitly
    // introduced) effect-row variable, not an error — even when the
    // enclosing fn doesn't thread it through its own return row. Here the
    // row var stays free/pure at the `takes` definition site, so calling
    // `f(1)` adds no concrete effect and the pure body type-checks.
    expectOk("fn takes(f: fn(i64) -> i64 ! {row}) -> i64 { f(1) }\n"
             "fn main() -> i64 { 0 }",
             "implicit_row_var_in_fn_type_ok");
}

void test_unknown_top_level_effect_label_errors() {
    // A non-built-in label in a fn's OWN top-level effect row, with no
    // backing generic param or fn-type-row use, remains an unknown-label
    // error (it cannot be a row var — nothing introduces it).
    expectErr("fn bad() -> i64 ! { totally_unknown } { 0 }\n"
              "fn main() -> i64 { 0 }",
              "unknown_top_level_effect_label_errors");
}

// --- Phase 10b: capturing closures ---

void test_closure_single_capture_ok() {
    expectOk("fn main() -> i64 { let n = 5; let f = |x| x + n; f(10) }",
             "closure_single_capture_ok");
}

void test_closure_multi_capture_ok() {
    expectOk("fn main() -> i64 { let a = 3; let b = 4; let f = |x| x + a + b; "
             "f(10) }",
             "closure_multi_capture_ok");
}

void test_closure_records_captures_on_node() {
    // The typechecker records the captured free variables (in order) on the
    // ClosureExpr node so codegen can lay out the env struct.
    auto pr = parse("fn main() -> i64 { let a = 1; let b = 2; "
                    "let f = |x| x + a + b; f(0) }");
    assert(pr.ok());
    auto r = typecheck(pr.program);
    if (!r.ok()) {
        std::cerr << "[closure_records_captures_on_node] tc failed\n";
        dump(r);
        std::abort();
    }
    // Find the ClosureExpr in the AST (main's body: let a, let b, let f, tail).
    const auto& mainFn = pr.program.functions.back();
    const kardashev::ast::ClosureExpr* found = nullptr;
    for (const auto& stmt : mainFn.body->stmts) {
        if (auto* let =
                dynamic_cast<const kardashev::ast::LetStmt*>(stmt.get())) {
            if (auto* cl = dynamic_cast<const kardashev::ast::ClosureExpr*>(
                    let->value.get())) {
                found = cl;
            }
        }
    }
    assert(found != nullptr);
    assert(found->captures.size() == 2);
    assert(found->captures[0].name == "a");
    assert(found->captures[1].name == "b");
}

void test_closure_no_capture_ok() {
    expectOk("fn main() -> i64 { let f = |x| x + 1; f(41) }",
             "closure_no_capture_ok");
}

void test_closure_higher_order_effect_poly_ok() {
    // Passing a pure closure to an effect-polymorphic higher-order fn keeps
    // the call pure.
    expectOk("fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }\n"
             "fn main() -> i64 { let k = 7; apply(|x| x + k) }",
             "closure_higher_order_effect_poly_ok");
}

void test_closure_io_effect_propagates_positive_ok() {
    // A closure that calls `print` is `{io}`; calling it from an io-declaring
    // context is accepted.
    expectOk("fn main() -> i64 ! {io} { let n = 1; let p = |x| print(x + n); "
             "p(5); 0 }",
             "closure_io_effect_propagates_positive_ok");
}

void test_closure_io_effect_propagates_negative_errors() {
    // The same io closure called from a PURE context must be rejected: the
    // closure's `{io}` row flows to the indirect call site, which the pure
    // `main` cannot absorb. (Ties Phase 10a effect inference to 10b.)
    expectErr("fn main() -> i64 { let p = |x| print(x); p(5) }",
              "closure_io_effect_propagates_negative_errors");
}

void test_closure_io_effect_via_higher_order_negative_errors() {
    // An io closure passed to an effect-polymorphic `apply` makes that call
    // io; a pure caller must be rejected (row polymorphism + closures).
    expectErr("fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }\n"
              "fn main() -> i64 { apply(|x| print(x)) }",
              "closure_io_effect_via_higher_order_negative_errors");
}

void test_closure_aggregate_capture_rejected() {
    // MVP limitation: capturing a non-Copy aggregate (a struct value) by
    // value is rejected with a clear error rather than miscompiled.
    expectErr("struct P { x: i64 }\n"
              "fn main() -> i64 { let p = P { x: 1 }; let f = |y| y + p.x; "
              "f(2) }",
              "closure_aggregate_capture_rejected");
}

void test_closure_arg_arity_mismatch_errors() {
    // Calling a closure with the wrong number of args is a type error,
    // routed through the same indirect-call arity check as fn values.
    expectErr("fn main() -> i64 { let f = |x| x + 1; f(1, 2) }",
              "closure_arg_arity_mismatch_errors");
}

// --- Phase 17a: richer closures & first-class fn values ---

void test_field_held_fn_value_call_ok() {
    // A fn value held in a struct field is callable via `(s.f)(x)`.
    expectOk("struct Adder { f: fn(i64) -> i64 }\n"
             "fn inc(x: i64) -> i64 { x + 1 }\n"
             "fn main() -> i64 { let a = Adder { f: inc }; (a.f)(10) }",
             "field_held_fn_value_call_ok");
}

void test_field_held_closure_call_ok() {
    // A struct field holding a capturing closure is also callable.
    expectOk("struct Adder { f: fn(i64) -> i64 }\n"
             "fn main() -> i64 { let b = 100; "
             "let a = Adder { f: |x| x + b }; (a.f)(5) }",
             "field_held_closure_call_ok");
}

void test_call_value_through_self_ok() {
    // `(self.f)(self.base)` inside an inherent method type-checks.
    expectOk("struct MapIter { base: i64, f: fn(i64) -> i64 }\n"
             "impl MapIter { fn step(self) -> i64 { (self.f)(self.base) } }\n"
             "fn dbl(x: i64) -> i64 { x + x }\n"
             "fn main() -> i64 { let m = MapIter { base: 4, f: dbl }; m.step() }",
             "call_value_through_self_ok");
}

void test_call_value_non_function_errors() {
    // Calling a non-function value as `(x)(...)` is rejected.
    expectErrContains("fn main() -> i64 { let x = 5; (x)(1) }",
                      "not a function", "call_value_non_function_errors");
}

void test_call_value_arg_type_mismatch_errors() {
    // The args of an indirect call through a field are unified against the
    // fn type's params; a bool arg to an i64 param is rejected.
    expectErr("struct Adder { f: fn(i64) -> i64 }\n"
              "fn inc(x: i64) -> i64 { x + 1 }\n"
              "fn main() -> i64 { let a = Adder { f: inc }; (a.f)(true) }",
              "call_value_arg_type_mismatch_errors");
}

void test_fnmut_by_ref_capture_ok() {
    // A closure mutating a `let mut` capture by reference type-checks, and
    // calling it multiple times is fine (FnMut).
    expectOk("fn main() -> i64 { let mut n = 0; let mut inc = || { n = n + 1; };"
             " inc(); inc(); n }",
             "fnmut_by_ref_capture_ok");
}

void test_fnmut_records_byref_on_node() {
    // The mutated capture is recorded with byRef = true; a read-only capture
    // stays byRef = false.
    auto pr = parse("fn main() -> i64 { let mut n = 0; let r = 7; "
                    "let mut f = || { n = n + r; }; f(); n }");
    assert(pr.ok());
    auto res = typecheck(pr.program);
    if (!res.ok()) {
        std::cerr << "[fnmut_records_byref_on_node] tc failed\n";
        dump(res);
        std::abort();
    }
    const auto& mainFn = pr.program.functions.back();
    const kardashev::ast::ClosureExpr* found = nullptr;
    for (const auto& stmt : mainFn.body->stmts) {
        if (auto* let =
                dynamic_cast<const kardashev::ast::LetStmt*>(stmt.get())) {
            if (auto* cl = dynamic_cast<const kardashev::ast::ClosureExpr*>(
                    let->value.get())) {
                found = cl;
            }
        }
    }
    assert(found != nullptr);
    assert(found->captures.size() == 2);
    // `n` is mutated => byRef; `r` is only read => by value.
    bool sawNByRef = false, sawRByValue = false;
    for (const auto& cap : found->captures) {
        if (cap.name == "n") sawNByRef = cap.byRef;
        if (cap.name == "r") sawRByValue = !cap.byRef;
    }
    assert(sawNByRef);
    assert(sawRByValue);
}

void test_fnmut_non_mut_capture_errors() {
    // Mutating a non-`mut` captured binding is rejected.
    expectErrContains(
        "fn main() -> i64 { let n = 0; let f = || { n = n + 1; }; f(); n }",
        "not declared `let mut`", "fnmut_non_mut_capture_errors");
}

void test_fnmut_return_byref_closure_errors() {
    // Returning a closure that captures by reference would dangle; rejected.
    expectErrContains(
        "fn bad() -> fn() -> i64 { let mut n = 0; || { n = n + 1; n } }\n"
        "fn main() -> i64 { 0 }",
        "by reference", "fnmut_return_byref_closure_errors");
}

void test_return_byvalue_closure_ok() {
    // Returning a by-VALUE capturing closure is still fine (no dangling ptr).
    expectOk("fn make() -> fn(i64) -> i64 { let k = 1; |x| x + k }\n"
             "fn main() -> i64 { (make())(41) }",
             "return_byvalue_closure_ok");
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

// --- Phase 13a: Iterator trait + method-receiver autoref + adaptors ---

// A `&mut self` method can be called repeatedly on a mut binding without a
// move error surfacing as a type/borrow problem (typecheck accepts it; the
// borrow checker's autoref is exercised separately).
void test_mut_self_repeated_calls_typecheck_ok() {
    expectOk(
        "trait Inc { fn inc(&mut self) -> i64; }\n"
        "struct Counter { n: i64 }\n"
        "impl Inc for Counter {\n"
        "    fn inc(&mut self) -> i64 { self.n = self.n + 1; self.n }\n"
        "}\n"
        "fn f() -> i64 { let mut c = Counter { n: 0 };"
        " c.inc(); c.inc(); c.inc() }",
        "mut_self_repeated_calls_typecheck_ok");
}

// `for` over a type implementing Iterator type-checks; the loop var binds the
// Option payload (i64).
void test_for_over_custom_iterator_ok() {
    expectOk(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator { fn next(&mut self) -> Option<i64>; }\n"
        "struct Countdown { n: i64 }\n"
        "impl Iterator for Countdown {\n"
        "    fn next(&mut self) -> Option<i64> {\n"
        "        if self.n <= 0 { None }\n"
        "        else { self.n = self.n - 1; Some(self.n + 1) }\n"
        "    }\n"
        "}\n"
        "fn f() -> i64 { let cd = Countdown { n: 3 }; let mut s = 0;"
        " for x in cd { s = s + x; } s }",
        "for_over_custom_iterator_ok");
}

// `for` over a type that does NOT impl Iterator (and isn't a Range) is
// rejected with a clear diagnostic.
void test_for_over_non_iterator_errors() {
    expectErrContains(
        "struct NotIter { n: i64 }\n"
        "fn f() -> i64 { let x = NotIter { n: 1 };"
        " for y in x { } 0 }",
        "impls Iterator", "for_over_non_iterator_errors");
}

// Effect composition: a `fold` generic over an effect-row var, called with an
// `io` closure inside an `io` fn, type-checks (the closure's io flows to the
// call site through the row var).
void test_fold_io_closure_positive() {
    expectOk(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator { fn next(&mut self) -> Option<i64>; }\n"
        "struct Countdown { n: i64 }\n"
        "impl Iterator for Countdown {\n"
        "    fn next(&mut self) -> Option<i64> {\n"
        "        if self.n <= 0 { None }\n"
        "        else { self.n = self.n - 1; Some(self.n + 1) }\n"
        "    }\n"
        "}\n"
        "fn fold<I: Iterator, e>(it: I, init: i64,"
        " f: fn(i64, i64) -> i64 ! {e}) -> i64 ! {e} {\n"
        "    let mut iter = it; let mut acc = init;\n"
        "    loop { match iter.next() {"
        " Some(x) => { acc = f(acc, x); }, None => { break; }, } }\n"
        "    acc\n"
        "}\n"
        "fn main() -> i64 ! { io } {\n"
        "    fold(Countdown { n: 3 }, 0, |acc, x| { print(x); acc + x })\n"
        "}",
        "fold_io_closure_positive");
}

// Negative: the same fold + io closure in a PURE fn must be rejected — the io
// effect leaks to the call site and the fn doesn't declare it.
void test_fold_io_closure_negative() {
    expectErrContains(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator { fn next(&mut self) -> Option<i64>; }\n"
        "struct Countdown { n: i64 }\n"
        "impl Iterator for Countdown {\n"
        "    fn next(&mut self) -> Option<i64> {\n"
        "        if self.n <= 0 { None }\n"
        "        else { self.n = self.n - 1; Some(self.n + 1) }\n"
        "    }\n"
        "}\n"
        "fn fold<I: Iterator, e>(it: I, init: i64,"
        " f: fn(i64, i64) -> i64 ! {e}) -> i64 ! {e} {\n"
        "    let mut iter = it; let mut acc = init;\n"
        "    loop { match iter.next() {"
        " Some(x) => { acc = f(acc, x); }, None => { break; }, } }\n"
        "    acc\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    fold(Countdown { n: 3 }, 0, |acc, x| { print(x); acc + x })\n"
        "}",
        "io", "fold_io_closure_negative");
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

// --- Phase 12 real async runtime ---

// `.await` is legal inside an async fn; yield_now returns a Future.
void test_async_await_in_async_fn_ok() {
    expectOk(
        "async fn work() -> i64 {\n"
        "    let a = yield_now(1).await;\n"
        "    let b = yield_now(2).await;\n"
        "    a + b\n"
        "}",
        "async_await_in_async_fn_ok");
}

// `.await` outside an async fn is rejected (it suspends the enclosing future).
void test_async_await_outside_async_fn_errors() {
    expectErr(
        "fn main() -> i64 { yield_now(1).await }",
        "async_await_outside_async_fn_errors");
}

// A synchronous `main` may run async work via block_on WITHOUT itself being
// async — constructing/driving a Future is not the `async` effect.
void test_async_block_on_in_sync_main_ok() {
    expectOk(
        "async fn work() -> i64 { let a = yield_now(1).await; a }\n"
        "fn main() -> i64 { block_on(work()) }",
        "async_block_on_in_sync_main_ok");
}

// An async fn implicitly carries `async`; awaiting satisfies that declaration
// (no explicit `! { async }` needed).
void test_async_implicit_effect_ok() {
    expectOk(
        "async fn work() -> i64 { yield_now(5).await }",
        "async_implicit_effect_ok");
}

// Source code cannot forge runtime Future values via struct literals.
void test_async_future_struct_literal_errors() {
    expectErr(
        "fn main() -> i64 { block_on(Future {}) }",
        "async_future_struct_literal_errors");
}

// `.await` requires a Future operand.
void test_async_await_non_future_errors() {
    expectErr(
        "async fn work() -> i64 { let x = 5; x.await }",
        "async_await_non_future_errors");
}

// Calling an async fn yields a Future, not the inner type: it cannot be used
// directly as i64.
void test_async_call_returns_future_not_int() {
    expectErr(
        "async fn work() -> i64 { yield_now(1).await }\n"
        "fn main() -> i64 { work() }",
        "async_call_returns_future_not_int");
}

// A NON-async fn that awaits is rejected even if it otherwise looks async-ish;
// `.await` demands the enclosing fn be async (and a sync fn awaiting an async
// callee leaks `async` it cannot declare). Belt-and-suspenders on the rule.
void test_async_sync_fn_awaiting_errors() {
    expectErr(
        "async fn inner() -> i64 { yield_now(1).await }\n"
        "fn outer() -> i64 { inner().await }",
        "async_sync_fn_awaiting_errors");
}

// --- Phase 17b: generic Future<T> / HashMap<i64, V> ---

// `block_on` on a bool-returning async fn yields `bool`, usable in an if cond.
void test_async_block_on_bool_result() {
    expectOk(
        "async fn ab() -> bool { let x = yield_now(1).await; x == 1 }\n"
        "fn main() -> i64 { if block_on(ab()) { 1 } else { 0 } }",
        "async_block_on_bool_result");
}

// `block_on` of a bool-returning async fn returns bool, NOT i64 — using it
// where an i64 is required is a type error (proves T threads through).
void test_async_block_on_bool_not_int_errors() {
    expectErr(
        "async fn ab() -> bool { let x = yield_now(1).await; x == 1 }\n"
        "fn main() -> i64 { block_on(ab()) + 1 }",
        "async_block_on_bool_not_int_errors");
}

// `.await` of a bool-returning async fn yields bool inside the awaiter.
void test_async_await_bool_result() {
    expectOk(
        "async fn ab() -> bool { let x = yield_now(1).await; x == 1 }\n"
        "async fn outer() -> i64 { if ab().await { 1 } else { 0 } }",
        "async_await_bool_result");
}

// `block_on` of a struct-returning async fn returns that struct; fields read.
void test_async_block_on_struct_result() {
    expectOk(
        "struct P { x: i64, y: i64 }\n"
        "async fn mk() -> P { let a = yield_now(1).await; P { x: a, y: a } }\n"
        "fn main() -> i64 { let p = block_on(mk()); p.x + p.y }",
        "async_block_on_struct_result");
}

// HashMap value type is generic: a bool value inserts and `get` returns
// `Option<bool>` (matched as a bool).
void test_hashmap_bool_value_ok() {
    expectOk(
        "enum Option<T> { Some(T), None }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    hashmap_insert(&mut m, 1, true);\n"
        "    match hashmap_get(&m, 1) { Some(v) => if v { 1 } else { 0 }, None => 0 }\n"
        "}",
        "hashmap_bool_value_ok");
}

// A struct value round-trips through HashMap<i64, P>.
void test_hashmap_struct_value_ok() {
    expectOk(
        "enum Option<T> { Some(T), None }\n"
        "struct P { x: i64, y: i64 }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    hashmap_insert(&mut m, 1, P { x: 2, y: 3 });\n"
        "    match hashmap_get(&m, 1) { Some(p) => p.x + p.y, None => 0 }\n"
        "}",
        "hashmap_struct_value_ok");
}

// Inserting a value of a type inconsistent with an earlier insert is a type
// error (the map's V is pinned by the first insert).
void test_hashmap_value_type_consistency_errors() {
    expectErr(
        "enum Option<T> { Some(T), None }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    hashmap_insert(&mut m, 1, true);\n"
        "    hashmap_insert(&mut m, 2, 5);\n" // 5 is i64, not bool
        "    0\n"
        "}",
        "hashmap_value_type_consistency_errors");
}

// The two-arg `HashMap<i64, V>` surface annotation requires an i64 key.
void test_hashmap_non_i64_key_errors() {
    expectErr(
        "fn f() -> HashMap<bool, i64> ! { alloc } { hashmap_new() }\n"
        "fn main() -> i64 { 0 }",
        "hashmap_non_i64_key_errors");
}

// --- Phase 15: bool literals, unary ops, else-if, inherent impls ---

void test_bool_literal_typechecks_as_bool() {
    expectOk("fn f() -> bool { true }", "bool_literal_true");
    expectOk("fn f() -> bool { false }", "bool_literal_false");
}

void test_bool_literal_in_if_cond() {
    expectOk("fn f() -> i64 { let t = true; if t { 1 } else { 0 } }",
             "bool_literal_in_if_cond");
}

void test_bool_literal_not_i64() {
    expectErr("fn f() -> i64 { true }", "bool_literal_not_i64");
}

void test_unary_neg_ok_and_type() {
    expectOk("fn f() -> i64 { -5 }", "unary_neg_literal");
    expectOk("fn f() -> i64 { let x = 3; -x }", "unary_neg_var");
    expectOk("fn f() -> i64 { -(-3) }", "unary_double_neg");
}

void test_unary_neg_on_bool_errors() {
    expectErrContains("fn f() -> i64 { -true }", "i64", "unary_neg_on_bool");
}

void test_unary_not_ok_and_type() {
    expectOk("fn f() -> bool { !true }", "unary_not_literal");
    expectOk("fn f() -> bool { !(2 < 1) }", "unary_not_comparison");
    expectOk("fn f() -> i64 { let d = false; if !d { 1 } else { 0 } }",
             "unary_not_branch");
}

void test_unary_not_on_int_errors() {
    expectErrContains("fn f() -> bool { !5 }", "bool", "unary_not_on_int");
}

void test_else_if_chain_typechecks() {
    expectOk(
        "fn classify(a: i64) -> i64 {\n"
        "  if a == 0 { 100 } else if a == 1 { 200 } else { 300 }\n"
        "}",
        "else_if_chain");
}

void test_else_if_branch_type_mismatch_errors() {
    // A mismatched branch type in an else-if ladder is still caught.
    expectErr(
        "fn f(a: i64) -> i64 {\n"
        "  if a == 0 { 1 } else if a == 1 { true } else { 3 }\n"
        "}",
        "else_if_branch_mismatch");
}

void test_inherent_impl_method_resolves() {
    expectOk(
        "struct P { x: i64 }\n"
        "impl P { fn get(&self) -> i64 { self.x } }\n"
        "fn main() -> i64 { let p = P { x: 9 }; p.get() }",
        "inherent_impl_method_resolves");
}

void test_inherent_mut_self_method_ok() {
    expectOk(
        "struct C { n: i64 }\n"
        "impl C {\n"
        "  fn get(&self) -> i64 { self.n }\n"
        "  fn bump(&mut self) -> i64 { self.n = self.n + 1; self.n }\n"
        "}\n"
        "fn main() -> i64 { let mut c = C { n: 0 }; c.bump(); c.bump(); c.get() }",
        "inherent_mut_self_method");
}

void test_inherent_and_trait_method_coexist() {
    expectOk(
        "trait Show { fn show(&self) -> i64; }\n"
        "struct W { v: i64 }\n"
        "impl Show for W { fn show(&self) -> i64 { self.v } }\n"
        "impl W { fn dbl(&self) -> i64 { self.v + self.v } }\n"
        "fn main() -> i64 { let w = W { v: 6 }; w.show() + w.dbl() }",
        "inherent_and_trait_coexist");
}

void test_inherent_unknown_method_errors() {
    expectErr(
        "struct P { x: i64 }\n"
        "impl P { fn get(&self) -> i64 { self.x } }\n"
        "fn main() -> i64 { let p = P { x: 9 }; p.nope() }",
        "inherent_unknown_method");
}

void test_duplicate_method_across_impls_errors() {
    // The same method name on both an inherent and a trait impl collides.
    expectErr(
        "trait Show { fn m(&self) -> i64; }\n"
        "struct P { x: i64 }\n"
        "impl Show for P { fn m(&self) -> i64 { self.x } }\n"
        "impl P { fn m(&self) -> i64 { self.x } }\n"
        "fn main() -> i64 { 0 }",
        "duplicate_method_across_impls");
}

// --- Phase 19: OS threads + Mutex + Send (compile-time data-race freedom) ---

void test_thread_spawn_join_ok() {
    // A bare fn value spawned + joined; thread ops carry `io`.
    expectOk(
        "fn w() -> i64 { 7 }\n"
        "fn main() -> i64 ! { io } {\n"
        "  let h = thread_spawn(w);\n"
        "  thread_join(h)\n"
        "}",
        "thread_spawn_join_ok");
}

void test_thread_spawn_requires_io_effect() {
    // `main` performs `io` via thread_spawn/thread_join but doesn't declare it.
    expectErr(
        "fn w() -> i64 { 7 }\n"
        "fn main() -> i64 {\n"
        "  let h = thread_spawn(w);\n"
        "  thread_join(h)\n"
        "}",
        "thread_spawn_requires_io_effect");
}

void test_thread_spawn_byvalue_closure_ok() {
    // A closure capturing an i64 BY VALUE is Send — accepted.
    expectOk(
        "fn main() -> i64 ! { io } {\n"
        "  let base = 41;\n"
        "  let h = thread_spawn(|| base + 1);\n"
        "  thread_join(h)\n"
        "}",
        "thread_spawn_byvalue_closure_ok");
}

void test_thread_spawn_byref_capture_rejected() {
    // A closure capturing `n` BY REFERENCE (it mutates it) is !Send across a
    // thread boundary — must be rejected at compile time.
    expectErr(
        "fn main() -> i64 ! { io } {\n"
        "  let mut n = 0;\n"
        "  let h = thread_spawn(|| { n = n + 1; n });\n"
        "  thread_join(h)\n"
        "}",
        "thread_spawn_byref_capture_rejected");
}

void test_thread_spawn_propagates_closure_io_effect() {
    // The spawned closure performs `io` (mutex_lock); thread_spawn is effect-
    // polymorphic so that flows to the caller, which here declares it: ok.
    expectOk(
        "fn bump(m: i64) -> i64 ! { io } { mutex_lock(m); mutex_unlock(m); 0 }\n"
        "fn main() -> i64 ! { alloc, io } {\n"
        "  let m = mutex_new(0);\n"
        "  let h = thread_spawn(|| bump(m));\n"
        "  thread_join(h)\n"
        "}",
        "thread_spawn_propagates_closure_io_effect");
}

void test_mutex_ops_typecheck_ok() {
    expectOk(
        "fn main() -> i64 ! { alloc, io } {\n"
        "  let m = mutex_new(10);\n"
        "  mutex_lock(m);\n"
        "  let v = mutex_get(m);\n"
        "  mutex_set(m, v + 1);\n"
        "  mutex_unlock(m);\n"
        "  mutex_get(m)\n"
        "}",
        "mutex_ops_typecheck_ok");
}

void test_mutex_new_requires_alloc_effect() {
    // mutex_new allocates -> `alloc`; main here only declares `io`.
    expectErr(
        "fn main() -> i64 ! { io } {\n"
        "  let m = mutex_new(0);\n"
        "  mutex_lock(m);\n"
        "  mutex_unlock(m);\n"
        "  0\n"
        "}",
        "mutex_new_requires_alloc_effect");
}

// --- Phase 23: panic + catch effect checking. ---

void test_panic_carries_panic_effect_ok() {
    // A fn that calls `panic` and DECLARES `! { panic }` is accepted.
    expectOk("fn f() -> i64 ! { panic } { panic(\"x\"); 0 }",
             "panic_carries_panic_effect_ok");
}

void test_panic_undeclared_effect_errors() {
    // Calling `panic` without declaring the `panic` effect is rejected.
    expectErr("fn f() -> i64 { panic(\"x\"); 0 }",
              "panic_undeclared_effect_errors");
}

void test_panic_propagates_through_caller() {
    // `panic` flows through an ordinary call: a caller of a panicking fn must
    // itself declare `panic` (or catch it).
    expectErr(
        "fn boom() -> i64 ! { panic } { panic(\"x\"); 0 }\n"
        "fn caller() -> i64 { boom() }",
        "panic_propagates_through_caller");
}

void test_catch_clears_panic_effect_ok() {
    // `catch` handles the panic, so a fn that only panics INSIDE a catch need
    // not declare `panic` itself.
    expectOk(
        "fn boom() -> i64 ! { panic } { panic(\"x\"); 0 }\n"
        "fn main() -> i64 { catch(boom, 0) }",
        "catch_clears_panic_effect_ok");
}

void test_catch_still_propagates_io_effect() {
    // catch only catches `panic` — other effects of the callback (here `io`)
    // still propagate, so an undeclaring caller is rejected.
    expectErr(
        "fn ioFn() -> i64 ! { io } { print(1); 0 }\n"
        "fn main() -> i64 { catch(ioFn, 0) }",
        "catch_still_propagates_io_effect");
}

void test_catch_propagates_io_effect_declared_ok() {
    // ...and when the caller DOES declare that effect, it's accepted.
    expectOk(
        "fn ioFn() -> i64 ! { io } { print(1); 0 }\n"
        "fn main() -> i64 ! { io } { catch(ioFn, 0) }",
        "catch_propagates_io_effect_declared_ok");
}

// --- Phase 24: extern "C" FFI declarations ---

void test_extern_call_typechecks() {
    // A call to an extern type-checks against its declared signature, and
    // the extern carries `io` by default so an `! { io }` caller is OK.
    expectOk(
        "extern \"C\" fn abs(x: i64) -> i64;\n"
        "fn main() -> i64 ! { io } { abs(0 - 7) }",
        "extern_call_typechecks");
}

void test_extern_i32_arg_is_i64_compatible() {
    // The FFI-only `i32` spelling surfaces as kardashev i64, so an i64 value
    // flows in/out transparently.
    expectOk(
        "extern \"C\" fn abs(x: i32) -> i32;\n"
        "fn main() -> i64 ! { io } { let y = abs(0 - 7); y + 1 }",
        "extern_i32_arg_is_i64_compatible");
}

void test_extern_ref_string_arg() {
    expectOk(
        "extern \"C\" fn strlen(s: &String) -> i64;\n"
        "fn main() -> i64 ! { io } { let s = \"hi\"; strlen(&s) }",
        "extern_ref_string_arg");
}

void test_extern_carries_io_pure_caller_rejected() {
    expectErrContains(
        "extern \"C\" fn getpid() -> i64;\n"
        "fn pureFn() -> i64 { getpid() }\n"
        "fn main() -> i64 ! { io } { pureFn() }",
        "effect `io`", "extern_carries_io_pure_caller_rejected");
}

void test_extern_explicit_pure_row_allows_pure_caller() {
    // `! { }` on the extern overrides the default io -> a pure caller is OK.
    expectOk(
        "extern \"C\" fn abs(x: i32) -> i32 ! { };\n"
        "fn pureFn() -> i64 { abs(0 - 3) }",
        "extern_explicit_pure_row_allows_pure_caller");
}

void test_extern_explicit_effect_row_honored() {
    // An explicit `! { io, alloc }` row replaces the default; a caller must
    // declare both.
    expectErrContains(
        "extern \"C\" fn weird() -> i64 ! { io, alloc };\n"
        "fn caller() -> i64 ! { io } { weird() }",
        "alloc", "extern_explicit_effect_row_honored");
}

void test_extern_bad_abi_rejected() {
    expectErrContains(
        "extern \"Rust\" fn abs(x: i64) -> i64;\n"
        "fn main() -> i64 { 0 }",
        "unsupported ABI", "extern_bad_abi_rejected");
}

void test_extern_collision_with_builtin_rejected() {
    expectErrContains(
        "extern \"C\" fn print(x: i64) -> i64;\n"
        "fn main() -> i64 { 0 }",
        "collides", "extern_collision_with_builtin_rejected");
}

void test_extern_wrong_arg_arity_rejected() {
    expectErrContains(
        "extern \"C\" fn abs(x: i64) -> i64;\n"
        "fn main() -> i64 ! { io } { abs(1, 2) }",
        "expects", "extern_wrong_arg_arity_rejected");
}

void test_extern_unrepresentable_type_rejected() {
    // A by-value user struct has no defined C ABI in an extern signature.
    expectErrContains(
        "struct P { x: i64 }\n"
        "extern \"C\" fn f(p: P) -> i64;\n"
        "fn main() -> i64 { 0 }",
        "not supported in an `extern", "extern_unrepresentable_type_rejected");
}

// --- Phase 25: const items, const fn, const-generic array lengths ----------

void test_const_item_ok() {
    expectOk("const SIZE: i64 = 3 + 2;\n"
             "fn main() -> i64 { SIZE }",
             "const_item_ok");
}

void test_const_bool_item_ok() {
    expectOk("const FLAG: bool = 3 > 2;\n"
             "fn main() -> i64 { if FLAG { 1 } else { 0 } }",
             "const_bool_item_ok");
}

void test_const_references_const_ok() {
    expectOk("const A: i64 = 10;\n"
             "const B: i64 = A * 2;\n"
             "fn main() -> i64 { B }",
             "const_references_const_ok");
}

void test_const_fn_in_const_context_ok() {
    expectOk("const fn sq(x: i64) -> i64 { x * x }\n"
             "const NINE: i64 = sq(3);\n"
             "fn main() -> i64 { NINE }",
             "const_fn_in_const_context_ok");
}

void test_const_fn_at_runtime_ok() {
    // A const fn is also an ordinary fn: a runtime call with a runtime arg.
    expectOk("const fn sq(x: i64) -> i64 { x * x }\n"
             "fn main() -> i64 { let y = 7; sq(y) }",
             "const_fn_at_runtime_ok");
}

void test_const_generic_array_len_ok() {
    expectOk("const N: i64 = 2 + 1;\n"
             "fn main() -> i64 { let a: [i64; N] = [10, 20, 30]; a[0] + a[2] }",
             "const_generic_array_len_ok");
}

void test_const_fn_array_len_ok() {
    expectOk("const fn sq(x: i64) -> i64 { x * x }\n"
             "fn main() -> i64 { let a: [i64; sq(2)] = [1,2,3,4]; a[3] }",
             "const_fn_array_len_ok");
}

void test_const_div_by_zero_errors() {
    expectErrContains("const BAD: i64 = 10 / 0;\n"
                      "fn main() -> i64 { BAD }",
                      "division by zero", "const_div_by_zero_errors");
}

void test_const_overflow_errors() {
    expectErrContains("const BIG: i64 = 9223372036854775807 + 1;\n"
                      "fn main() -> i64 { BIG }",
                      "overflow", "const_overflow_errors");
}

void test_const_cyclic_errors() {
    expectErrContains("const A: i64 = B + 1;\n"
                      "const B: i64 = A + 1;\n"
                      "fn main() -> i64 { A }",
                      "cyclic", "const_cyclic_errors");
}

void test_const_fn_non_evaluable_errors() {
    // A const fn that performs I/O, called in a const context, is rejected.
    expectErrContains("const fn bad(x: i64) -> i64 { print(x); x }\n"
                      "const V: i64 = bad(5);\n"
                      "fn main() -> i64 { V }",
                      "not const-evaluable", "const_fn_non_evaluable_errors");
}

void test_const_type_mismatch_errors() {
    expectErrContains("const X: i64 = true;\n"
                      "fn main() -> i64 { X }",
                      "const initializer has type bool",
                      "const_type_mismatch_errors");
}

void test_const_array_len_bool_errors() {
    expectErrContains("fn main() -> i64 { let a: [i64; 2 > 1] = [1]; a[0] }",
                      "expected an i64 constant",
                      "const_array_len_bool_errors");
}

void test_const_array_len_calls_nonconst_fn_errors() {
    expectErrContains("fn sq(x: i64) -> i64 { x * x }\n"
                      "fn main() -> i64 { let a: [i64; sq(2)] = [1,2,3,4]; a[0] }",
                      "not const-evaluable",
                      "const_array_len_calls_nonconst_fn_errors");
}

// Phase 58/59 (v10): const-generic monomorphization.
void test_const_generic_struct_ok() {
    expectOk("struct Mat<const N: i64> { data: [i64; N] }\n"
             "fn main() -> i64 { let a: Mat<3> = Mat { data: [1, 2, 3] };"
             " a.data[0] + a.data[2] }",
             "const_generic_struct_ok");
}

void test_const_generic_type_in_const_slot_errors() {
    expectErrContains("struct Mat<const N: i64> { data: [i64; N] }\n"
                      "fn main() -> i64 { let a: Mat<i64> = Mat { data: [1] };"
                      " 0 }",
                      "const", "const_generic_type_in_const_slot_errors");
}

void test_const_generic_const_in_type_slot_errors() {
    expectErrContains("struct W<T> { x: T }\n"
                      "fn main() -> i64 { let a: W<3> = W { x: 1 }; 0 }",
                      "expects a type",
                      "const_generic_const_in_type_slot_errors");
}

void test_const_generic_fn_ok() {
    expectOk("fn dot<const N: i64>(a: [i64; N], b: [i64; N]) -> i64 {\n"
             "    let mut acc = 0; let mut i = 0;\n"
             "    while i < N { acc = acc + a[i] * b[i]; i = i + 1; }\n"
             "    acc\n"
             "}\n"
             "fn main() -> i64 {\n"
             "    let x: [i64; 3] = [1, 2, 3];\n"
             "    let y: [i64; 3] = [4, 5, 6];\n"
             "    dot(x, y)\n"
             "}",
             "const_generic_fn_ok");
}

void test_const_generic_dim_mismatch_errors() {
    expectErrContains("fn dot<const N: i64>(a: [i64; N], b: [i64; N]) -> i64 {"
                      " a[0] }\n"
                      "fn main() -> i64 {\n"
                      "    let x: [i64; 3] = [1, 2, 3];\n"
                      "    let y: [i64; 2] = [4, 5];\n"
                      "    dot(x, y)\n"
                      "}",
                      "dimension mismatch",
                      "const_generic_dim_mismatch_errors");
}

void test_const_generic_fn_uninferable_errors() {
    expectErrContains("fn mk<const N: i64>() -> i64 { N }\n"
                      "fn main() -> i64 { mk() }",
                      "cannot infer const generic parameter",
                      "const_generic_fn_uninferable_errors");
}

// Phase 60 (v10): the effect-subset rule. (These run WITHOUT the prelude, so
// they avoid builtins like `print`; the subset check fires on the DECLARED
// effect row alone.)
void test_effect_subset_super_effecting_errors() {
    expectErrContains("trait Greet { fn greet(&self) -> i64; }\n"
                      "struct S {}\n"
                      "impl Greet for S { fn greet(&self) -> i64 ! { io } { 0 } }\n"
                      "fn main() -> i64 { 0 }",
                      "does not permit",
                      "effect_subset_super_effecting_errors");
}

void test_effect_subset_fewer_ok() {
    // An impl with FEWER effects than the trait permits is allowed.
    expectOk("trait Greet { fn greet(&self) -> i64 ! { io }; }\n"
             "struct S {}\n"
             "impl Greet for S { fn greet(&self) -> i64 { 7 } }\n"
             "fn main() -> i64 { let s = S {}; s.greet() }",
             "effect_subset_fewer_ok");
}

void test_effect_subset_drop_io_trait_ok() {
    // Review fix: Drop is NOT exempt — it follows the subset rule like any
    // trait (so a `dyn Drop` dispatch can't launder effects). An io Drop impl
    // under an io-DECLARING Drop trait is fine...
    expectOk("trait Drop { fn drop(&mut self) ! { io }; }\n"
             "struct Noisy { id: i64 }\n"
             "impl Drop for Noisy { fn drop(&mut self) ! { io } { } }\n"
             "fn main() -> i64 { 0 }",
             "effect_subset_drop_io_trait_ok");
}

void test_effect_subset_drop_super_effecting_errors() {
    // ...but an io Drop impl under a PURE Drop trait is rejected (the old
    // name-based exemption laundered io through dyn Drop dispatch).
    expectErrContains("trait Drop { fn drop(&mut self); }\n"
                      "struct Noisy { id: i64 }\n"
                      "impl Drop for Noisy { fn drop(&mut self) ! { io } { } }\n"
                      "fn main() -> i64 { 0 }",
                      "does not permit",
                      "effect_subset_drop_super_effecting_errors");
}

// Phase 61 (v10): non-Copy arrays + RingBuffer<T, const CAP> + closure infer.
void test_noncopy_array_ok() {
    // A non-Copy element type in a fixed-size array is now allowed (used WITHOUT
    // the prelude here, so a tiny user `String`-like struct stands in for it).
    expectOk("struct S { x: i64 }\n"
             "fn main() -> i64 { let a: [S; 2] = [S { x: 1 }, S { x: 2 }];"
             " a[0].x + a[1].x }",
             "noncopy_array_ok");
}

void test_const_generic_mixed_struct_ok() {
    // A struct generic over BOTH a type param and a const param.
    expectOk("struct Buf<T, const N: i64> { data: [T; N] }\n"
             "fn main() -> i64 { let b: Buf<i64, 2> = Buf { data: [3, 4] };"
             " b.data[0] + b.data[1] }",
             "const_generic_mixed_struct_ok");
}

// Phase 63 (v11): sized signed machine integers.
void test_sized_int_ok() {
    expectOk("fn add(a: i32, b: i32) -> i32 { a + b }\n"
             "fn main() -> i64 { let x: i32 = 5; let y: i32 = add(x, 10) + 1;"
             " 0 }",
             "sized_int_ok");
}

void test_int_width_mismatch_errors() {
    // No implicit widening: an i32 binding can't take an i64 value.
    expectErrContains("fn id64(x: i64) -> i64 { x }\n"
                      "fn main() -> i64 { let w: i32 = id64(5); 0 }",
                      "i32",
                      "int_width_mismatch_errors");
}

void test_int_literal_out_of_range_errors() {
    expectErrContains("fn main() -> i64 { let x: i8 = 200; 0 }",
                      "out of range",
                      "int_literal_out_of_range_errors");
}

// Phase 64 (v11): integer-literal width suffixes + radix prefixes.
void test_int_suffix_literal_ok() {
    // A suffixed literal IS that int type with NO annotation; it must match the
    // i32 parameter without any widening.
    expectOk("fn use32(x: i32) -> i32 { x }\n"
             "fn main() -> i64 { let y = use32(5i32); let z: i32 = y + 1i32;"
             " 0 }",
             "int_suffix_literal_ok");
}

void test_radix_literal_ok() {
    // Hex / binary literals are default i64 and add up as values.
    expectOk("fn main() -> i64 { let a = 0xFF; let b = 0b1010; a + b }",
             "radix_literal_ok");
}

void test_radix_suffix_combo_ok() {
    expectOk("fn use32(x: i32) -> i32 { x }\n"
             "fn main() -> i64 { let m = use32(0xFFi32); 0 }",
             "radix_suffix_combo_ok");
}

void test_int_suffix_out_of_range_errors() {
    expectErrContains("fn main() -> i64 { let x = 200i8; 0 }",
                      "out of range",
                      "int_suffix_out_of_range_errors");
}

void test_int_suffix_width_mismatch_errors() {
    // A suffixed i32 can't be added to an i64 var (non-coercive lattice).
    expectErrContains("fn main() -> i64 { let a = 5i32; let b: i64 = 7;"
                      " let c = a + b; 0 }",
                      "same integer",
                      "int_suffix_width_mismatch_errors");
}

void test_unsigned_suffix_deferred_errors() {
    // u8..u64 suffixes are honestly rejected until Phase 66 lands unsigned ints.
    expectErrContains("fn main() -> i64 { let x = 5u8; 0 }",
                      "unsigned",
                      "unsigned_suffix_deferred_errors");
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
    // Phase 21a generic trait parameters
    test_generic_trait_two_impls_ok();
    test_generic_trait_bounded_fn_ok();
    test_generic_trait_impl_wrong_elem_errors();
    test_generic_trait_impl_arity_errors();
    test_parameterized_bound_arity_errors();
    test_dyn_generic_trait_rejected();
    test_dyn_parameterized_trait_ok();
    test_dyn_nongeneric_trait_still_ok();
    test_generic_trait_bound_effects_compose();
    // Phase 21b: where clauses + associated types
    test_where_clause_bounded_fn_ok();
    test_where_clause_multi_ok();
    test_where_clause_bound_enforced();
    test_assoc_self_item_two_impls_ok();
    test_assoc_c_item_bounded_ok();
    test_assoc_impl_missing_item_errors();
    test_assoc_impl_extra_item_errors();
    test_assoc_projection_unbounded_errors();
    test_assoc_dup_decl_errors();
    // Phase 11 dyn Trait + trait objects
    test_dyn_ref_param_ok();
    test_dyn_box_ok();
    test_dyn_coercion_two_impls_ok();
    test_dyn_unknown_trait_errors();
    test_dyn_unsafe_static_method_errors();
    test_dyn_coerce_unimpl_type_errors();
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
    // Phase 10a effect-carrying fn types + effect-row polymorphism
    test_effect_poly_apply_pure_ok();
    test_effect_poly_apply_io_ok();
    test_effect_poly_apply_io_leak_errors();
    test_effect_poly_apply_io_caller_declares_more_ok();
    test_concrete_fn_type_param_io_ok();
    test_concrete_fn_type_param_pure_body_errors();
    test_fn_value_preserves_effects_via_let();
    test_fn_value_preserves_effects_via_let_ok();
    test_pure_fn_value_via_let_ok();
    test_fn_type_param_arg_must_match_effects();
    test_effect_row_var_classified_in_schema();
    test_generic_param_type_and_effect_conflict_errors();
    test_higher_order_type_var_and_effect_var_together_ok();
    test_implicit_row_var_in_fn_type_ok();
    test_unknown_top_level_effect_label_errors();
    // Phase 10b capturing closures
    test_closure_single_capture_ok();
    test_closure_multi_capture_ok();
    test_closure_records_captures_on_node();
    test_closure_no_capture_ok();
    test_closure_higher_order_effect_poly_ok();
    test_closure_io_effect_propagates_positive_ok();
    test_closure_io_effect_propagates_negative_errors();
    test_closure_io_effect_via_higher_order_negative_errors();
    test_closure_aggregate_capture_rejected();
    test_closure_arg_arity_mismatch_errors();
    // Phase 17a richer closures & first-class fn values
    test_field_held_fn_value_call_ok();
    test_field_held_closure_call_ok();
    test_call_value_through_self_ok();
    test_call_value_non_function_errors();
    test_call_value_arg_type_mismatch_errors();
    test_fnmut_by_ref_capture_ok();
    test_fnmut_records_byref_on_node();
    test_fnmut_non_mut_capture_errors();
    test_fnmut_return_byref_closure_errors();
    test_return_byvalue_closure_ok();
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
    // Phase 12 real async runtime
    test_async_await_in_async_fn_ok();
    test_async_await_outside_async_fn_errors();
    test_async_block_on_in_sync_main_ok();
    test_async_implicit_effect_ok();
    test_async_future_struct_literal_errors();
    test_async_await_non_future_errors();
    test_async_call_returns_future_not_int();
    test_async_sync_fn_awaiting_errors();
    // Phase 17b generic Future<T> / HashMap<i64,V>
    test_async_block_on_bool_result();
    test_async_block_on_bool_not_int_errors();
    test_async_await_bool_result();
    test_async_block_on_struct_result();
    test_hashmap_bool_value_ok();
    test_hashmap_struct_value_ok();
    test_hashmap_value_type_consistency_errors();
    test_hashmap_non_i64_key_errors();
    // Phase 13a Iterator trait + method-receiver autoref + adaptors
    test_mut_self_repeated_calls_typecheck_ok();
    test_for_over_custom_iterator_ok();
    test_for_over_non_iterator_errors();
    test_fold_io_closure_positive();
    test_fold_io_closure_negative();
    // Phase 15: bool literals, unary ops, else-if, inherent impls
    test_bool_literal_typechecks_as_bool();
    test_bool_literal_in_if_cond();
    test_bool_literal_not_i64();
    test_unary_neg_ok_and_type();
    test_unary_neg_on_bool_errors();
    test_unary_not_ok_and_type();
    test_unary_not_on_int_errors();
    test_else_if_chain_typechecks();
    test_else_if_branch_type_mismatch_errors();
    test_inherent_impl_method_resolves();
    test_inherent_mut_self_method_ok();
    test_inherent_and_trait_method_coexist();
    test_inherent_unknown_method_errors();
    test_duplicate_method_across_impls_errors();
    // Phase 19: OS threads + Mutex + Send
    test_thread_spawn_join_ok();
    test_thread_spawn_requires_io_effect();
    test_thread_spawn_byvalue_closure_ok();
    test_thread_spawn_byref_capture_rejected();
    test_thread_spawn_propagates_closure_io_effect();
    test_mutex_ops_typecheck_ok();
    test_mutex_new_requires_alloc_effect();
    test_panic_carries_panic_effect_ok();
    test_panic_undeclared_effect_errors();
    test_panic_propagates_through_caller();
    test_catch_clears_panic_effect_ok();
    test_catch_still_propagates_io_effect();
    test_catch_propagates_io_effect_declared_ok();
    // Phase 24: extern "C" FFI declarations.
    test_extern_call_typechecks();
    test_extern_i32_arg_is_i64_compatible();
    test_extern_ref_string_arg();
    test_extern_carries_io_pure_caller_rejected();
    test_extern_explicit_pure_row_allows_pure_caller();
    test_extern_explicit_effect_row_honored();
    test_extern_bad_abi_rejected();
    test_extern_collision_with_builtin_rejected();
    test_extern_wrong_arg_arity_rejected();
    test_extern_unrepresentable_type_rejected();
    // Phase 25: const items, const fn, const-generic array lengths.
    test_const_item_ok();
    test_const_bool_item_ok();
    test_const_references_const_ok();
    test_const_fn_in_const_context_ok();
    test_const_fn_at_runtime_ok();
    test_const_generic_array_len_ok();
    // Phase 58/59 (v10): const-generic monomorphization + dimension unification.
    test_const_generic_struct_ok();
    test_const_generic_type_in_const_slot_errors();
    test_const_generic_const_in_type_slot_errors();
    test_const_generic_fn_ok();
    test_const_generic_dim_mismatch_errors();
    test_const_generic_fn_uninferable_errors();
    // Phase 60 (v10): effect-subset rule.
    test_effect_subset_super_effecting_errors();
    test_effect_subset_fewer_ok();
    test_effect_subset_drop_io_trait_ok();
    test_effect_subset_drop_super_effecting_errors();
    // Phase 61 (v10): non-Copy arrays + mixed type+const generics.
    test_noncopy_array_ok();
    test_const_generic_mixed_struct_ok();
    test_sized_int_ok();
    test_int_width_mismatch_errors();
    test_int_literal_out_of_range_errors();
    test_int_suffix_literal_ok();
    test_radix_literal_ok();
    test_radix_suffix_combo_ok();
    test_int_suffix_out_of_range_errors();
    test_int_suffix_width_mismatch_errors();
    test_unsigned_suffix_deferred_errors();
    test_const_fn_array_len_ok();
    test_const_div_by_zero_errors();
    test_const_overflow_errors();
    test_const_cyclic_errors();
    test_const_fn_non_evaluable_errors();
    test_const_type_mismatch_errors();
    test_const_array_len_bool_errors();
    test_const_array_len_calls_nonconst_fn_errors();
    std::cout << "All typecheck tests passed (269 cases)\n";
    return 0;
}
