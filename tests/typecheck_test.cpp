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
    assert(r.structs["Point"]->kind == kardashev::TypeKind::Struct);
    assert(r.structs["Point"]->structFields.size() == 2);
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
    std::cout << "All typecheck tests passed (28 cases)\n";
    return 0;
}
