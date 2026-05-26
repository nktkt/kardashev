// End-to-end codegen tests for kardashev V1.
//
// Each test parses + type-checks + codegens the source, then JIT-compiles
// the resulting LLVM module via ORC v2 LLJIT, looks up a no-arg `main()`
// returning i64, calls it, and asserts on the value. This is effectively
// a full V1 compilation-pipeline test — the only thing missing for the
// MVP REPL is a stdin loop, which Phase 1.5 adds.
//
// Headline test: `fib(10) == 55` running through native code.

#include "kardashev/codegen.hpp"
#include "kardashev/parser.hpp"
#include "kardashev/typecheck.hpp"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

void initJITTargetsOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
}

std::int64_t compileAndRun(const std::string& src, const char* fnName,
                           const char* label) {
    initJITTargetsOnce();

    auto pr = kardashev::parse(src);
    if (!pr.ok()) {
        std::cerr << "[" << label << "] parse failed:\n";
        for (const auto& e : pr.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": "
                      << e.message << '\n';
        }
        std::abort();
    }

    auto tcr = kardashev::typecheck(pr.program);
    if (!tcr.ok()) {
        std::cerr << "[" << label << "] typecheck failed:\n";
        for (const auto& e : tcr.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": "
                      << e.message << '\n';
        }
        std::abort();
    }

    auto cgr = kardashev::codegen(pr.program, tcr);
    if (!cgr.ok()) {
        std::cerr << "[" << label << "] codegen failed:\n";
        for (const auto& msg : cgr.errors) {
            std::cerr << "  " << msg << '\n';
        }
        std::abort();
    }

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        llvm::errs() << "[" << label
                     << "] LLJIT create failed: "
                     << llvm::toString(jitOrErr.takeError()) << "\n";
        std::abort();
    }
    auto jit = std::move(*jitOrErr);

    auto tsm = llvm::orc::ThreadSafeModule(std::move(cgr.module),
                                           std::move(cgr.context));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::errs() << "[" << label
                     << "] addIRModule failed: "
                     << llvm::toString(std::move(err)) << "\n";
        std::abort();
    }

    auto symOrErr = jit->lookup(fnName);
    if (!symOrErr) {
        llvm::errs() << "[" << label << "] lookup '" << fnName
                     << "' failed: "
                     << llvm::toString(symOrErr.takeError()) << "\n";
        std::abort();
    }

    using EntryFn = std::int64_t (*)();
    auto fn = symOrErr->toPtr<EntryFn>();
    return fn();
}

void expectEquals(std::int64_t got, std::int64_t want, const char* label) {
    if (got != want) {
        std::cerr << "[" << label << "] expected " << want << ", got "
                  << got << '\n';
        std::abort();
    }
}

void test_constant_42() {
    auto v = compileAndRun("fn main() -> i64 { 42 }", "main", "constant_42");
    expectEquals(v, 42, "constant_42");
}

void test_arithmetic_precedence() {
    // 1 + 2 * 3 == 7  (* before +)
    auto v = compileAndRun("fn main() -> i64 { 1 + 2 * 3 }", "main",
                           "arithmetic_precedence");
    expectEquals(v, 7, "arithmetic_precedence");
}

void test_parenthesized() {
    auto v = compileAndRun("fn main() -> i64 { (1 + 2) * 3 }", "main",
                           "parenthesized");
    expectEquals(v, 9, "parenthesized");
}

void test_subtraction_left_assoc() {
    // 10 - 3 - 2 == 5  (left-assoc, not 10 - (3 - 2) = 9)
    auto v = compileAndRun("fn main() -> i64 { 10 - 3 - 2 }", "main",
                           "subtraction_left_assoc");
    expectEquals(v, 5, "subtraction_left_assoc");
}

void test_signed_division() {
    auto v = compileAndRun("fn main() -> i64 { 0 - 7 / 2 }", "main",
                           "signed_division");
    // 0 - (7 / 2) == 0 - 3 == -3  (SDiv truncates toward zero)
    expectEquals(v, -3, "signed_division");
}

void test_let_and_use() {
    auto v = compileAndRun(
        "fn main() -> i64 { let x = 5; let y = 10; x + y }",
        "main", "let_and_use");
    expectEquals(v, 15, "let_and_use");
}

void test_function_call() {
    auto v = compileAndRun(
        "fn add(a: i64, b: i64) -> i64 { a + b }\n"
        "fn main() -> i64 { add(2, 3) }",
        "main", "function_call");
    expectEquals(v, 5, "function_call");
}

void test_if_then() {
    auto v = compileAndRun(
        "fn main() -> i64 { if 1 < 2 { 100 } else { 200 } }",
        "main", "if_then");
    expectEquals(v, 100, "if_then");
}

void test_if_else() {
    auto v = compileAndRun(
        "fn main() -> i64 { if 2 < 1 { 100 } else { 200 } }",
        "main", "if_else");
    expectEquals(v, 200, "if_else");
}

void test_return_stmt() {
    auto v = compileAndRun("fn main() -> i64 { return 99; }", "main",
                           "return_stmt");
    expectEquals(v, 99, "return_stmt");
}

void test_return_inside_if() {
    // Then-branch returns early; else-branch flows through.
    auto v = compileAndRun(
        "fn pick(n: i64) -> i64 {\n"
        "    if n < 0 { return 0; } else { n }\n"
        "}\n"
        "fn main() -> i64 { pick(7) }",
        "main", "return_inside_if");
    expectEquals(v, 7, "return_inside_if");
}

void test_return_inside_if_negative() {
    auto v = compileAndRun(
        "fn pick(n: i64) -> i64 {\n"
        "    if n < 0 { return 0; } else { n }\n"
        "}\n"
        "fn main() -> i64 { pick(0 - 5) }",
        "main", "return_inside_if_negative");
    expectEquals(v, 0, "return_inside_if_negative");
}

// THE MVP TARGET — recursive fib running natively through the entire
// kardashev V1 pipeline (lex -> parse -> typecheck -> codegen -> JIT).
void test_recursive_fib_10() {
    auto v = compileAndRun(
        "fn fib(n: i64) -> i64 {\n"
        "    if n < 2 { n } else { fib(n-1) + fib(n-2) }\n"
        "}\n"
        "fn main() -> i64 { fib(10) }",
        "main", "recursive_fib_10");
    expectEquals(v, 55, "recursive_fib_10");
}

void test_recursive_fib_20() {
    auto v = compileAndRun(
        "fn fib(n: i64) -> i64 {\n"
        "    if n < 2 { n } else { fib(n-1) + fib(n-2) }\n"
        "}\n"
        "fn main() -> i64 { fib(20) }",
        "main", "recursive_fib_20");
    expectEquals(v, 6765, "recursive_fib_20");
}

void test_forward_reference() {
    auto v = compileAndRun(
        "fn main() -> i64 { later(3) }\n"
        "fn later(x: i64) -> i64 { x + x }",
        "main", "forward_reference");
    expectEquals(v, 6, "forward_reference");
}

void test_struct_pass_and_field_access() {
    // Phase 2.1 headline: struct by-value param + field access.
    auto v = compileAndRun(
        "struct Point { x: i64, y: i64 }\n"
        "fn sum(p: Point) -> i64 { p.x + p.y }\n"
        "fn main() -> i64 { sum(Point { x: 3, y: 4 }) }",
        "main", "struct_pass_and_field_access");
    expectEquals(v, 7, "struct_pass_and_field_access");
}

void test_struct_let_bound() {
    auto v = compileAndRun(
        "struct Point { x: i64, y: i64 }\n"
        "fn main() -> i64 {\n"
        "    let p = Point { x: 10, y: 32 };\n"
        "    p.x + p.y\n"
        "}",
        "main", "struct_let_bound");
    expectEquals(v, 42, "struct_let_bound");
}

void test_struct_literal_field_order_swapped() {
    // Source order is y-then-x; codegen must still place each value at its
    // declared index, so x.x == 3 and y.y == 4, giving x - y == -1.
    auto v = compileAndRun(
        "struct Point { x: i64, y: i64 }\n"
        "fn main() -> i64 {\n"
        "    let p = Point { y: 4, x: 3 };\n"
        "    p.x - p.y\n"
        "}",
        "main", "struct_literal_field_order_swapped");
    expectEquals(v, -1, "struct_literal_field_order_swapped");
}

void test_struct_return_from_fn() {
    auto v = compileAndRun(
        "struct Point { x: i64, y: i64 }\n"
        "fn make(a: i64, b: i64) -> Point { Point { x: a, y: b } }\n"
        "fn main() -> i64 {\n"
        "    let p = make(11, 31);\n"
        "    p.x + p.y\n"
        "}",
        "main", "struct_return_from_fn");
    expectEquals(v, 42, "struct_return_from_fn");
}

void test_nested_struct() {
    auto v = compileAndRun(
        "struct Inner { v: i64 }\n"
        "struct Outer { inner: Inner, k: i64 }\n"
        "fn main() -> i64 {\n"
        "    let o = Outer { inner: Inner { v: 40 }, k: 2 };\n"
        "    o.inner.v + o.k\n"
        "}",
        "main", "nested_struct");
    expectEquals(v, 42, "nested_struct");
}

void test_enum_unwrap_or_some() {
    auto v = compileAndRun(
        "enum Maybe { Some(i64), None }\n"
        "fn unwrap_or(m: Maybe, d: i64) -> i64 {\n"
        "    match m { Some(x) => x, None => d, }\n"
        "}\n"
        "fn main() -> i64 { unwrap_or(Some(42), 0) }",
        "main", "enum_unwrap_or_some");
    expectEquals(v, 42, "enum_unwrap_or_some");
}

void test_enum_unwrap_or_none() {
    auto v = compileAndRun(
        "enum Maybe { Some(i64), None }\n"
        "fn unwrap_or(m: Maybe, d: i64) -> i64 {\n"
        "    match m { Some(x) => x, None => d, }\n"
        "}\n"
        "fn main() -> i64 { unwrap_or(None, 99) }",
        "main", "enum_unwrap_or_none");
    expectEquals(v, 99, "enum_unwrap_or_none");
}

void test_match_literal_arms() {
    auto v = compileAndRun(
        "fn classify(n: i64) -> i64 {\n"
        "    match n { 0 => 100, 1 => 200, 2 => 300, _ => 999 }\n"
        "}\n"
        "fn main() -> i64 { classify(1) }",
        "main", "match_literal_arms");
    expectEquals(v, 200, "match_literal_arms");
}

void test_match_literal_wildcard_fallthrough() {
    auto v = compileAndRun(
        "fn classify(n: i64) -> i64 {\n"
        "    match n { 0 => 100, 1 => 200, _ => 999 }\n"
        "}\n"
        "fn main() -> i64 { classify(42) }",
        "main", "match_literal_wildcard_fallthrough");
    expectEquals(v, 999, "match_literal_wildcard_fallthrough");
}

void test_match_var_binds_scrutinee() {
    auto v = compileAndRun(
        "fn dbl(n: i64) -> i64 { match n { x => x + x } }\n"
        "fn main() -> i64 { dbl(21) }",
        "main", "match_var_binds_scrutinee");
    expectEquals(v, 42, "match_var_binds_scrutinee");
}

void test_enum_multi_arg_variant() {
    auto v = compileAndRun(
        "enum Pair { Two(i64, i64), Empty }\n"
        "fn sum(p: Pair) -> i64 {\n"
        "    match p { Two(a, b) => a + b, Empty => 0 }\n"
        "}\n"
        "fn main() -> i64 { sum(Two(15, 27)) }",
        "main", "enum_multi_arg_variant");
    expectEquals(v, 42, "enum_multi_arg_variant");
}

void test_enum_all_unit() {
    auto v = compileAndRun(
        "enum Color { Red, Green, Blue }\n"
        "fn rank(c: Color) -> i64 {\n"
        "    match c { Red => 1, Green => 2, Blue => 3 }\n"
        "}\n"
        "fn main() -> i64 { rank(Blue) }",
        "main", "enum_all_unit");
    expectEquals(v, 3, "enum_all_unit");
}

void test_nested_ctor_pattern() {
    auto v = compileAndRun(
        "enum Maybe { Some(Maybe2), None }\n"
        "enum Maybe2 { Just(i64), Nope }\n"
        "fn deep(m: Maybe) -> i64 {\n"
        "    match m {\n"
        "        Some(Just(x)) => x,\n"
        "        Some(Nope) => 1,\n"
        "        None => 2,\n"
        "    }\n"
        "}\n"
        "fn main() -> i64 { deep(Some(Just(42))) }",
        "main", "nested_ctor_pattern");
    expectEquals(v, 42, "nested_ctor_pattern");
}

void test_enum_with_struct_payload() {
    auto v = compileAndRun(
        "struct Point { x: i64, y: i64 }\n"
        "enum Geo { Pt(Point), Origin }\n"
        "fn get_x(g: Geo) -> i64 {\n"
        "    match g { Pt(p) => p.x, Origin => 0 }\n"
        "}\n"
        "fn main() -> i64 { get_x(Pt(Point { x: 42, y: 99 })) }",
        "main", "enum_with_struct_payload");
    expectEquals(v, 42, "enum_with_struct_payload");
}

// --- Phase 2.3c: decision-tree codegen tests ---

// 10 literal arms — exercises the linear-chain Switch on i64. The
// previous quadratic emit-arm-by-arm path duplicated payload checks per
// arm; the DT version compares each literal once.
void test_dt_10_arm_literal_match() {
    auto v = compileAndRun(
        "fn classify(n: i64) -> i64 {\n"
        "    match n {\n"
        "         0 =>   0,\n"
        "         1 =>  10,\n"
        "         2 =>  20,\n"
        "         3 =>  30,\n"
        "         4 =>  40,\n"
        "         5 =>  50,\n"
        "         6 =>  60,\n"
        "         7 =>  70,\n"
        "         8 =>  80,\n"
        "         9 =>  90,\n"
        "         _ => 999,\n"
        "    }\n"
        "}\n"
        "fn main() -> i64 { classify(7) + classify(42) }",
        "main", "dt_10_arm_literal_match");
    // classify(7) == 70, classify(42) == 999 (default).
    expectEquals(v, 70 + 999, "dt_10_arm_literal_match");
}

// Multiple arms sharing the same outer ctor — the DT should switch
// on the outer tag once, then dispatch on inner sub-patterns. Previously
// the linear lowering re-checked the outer tag per arm.
void test_dt_shared_outer_ctor() {
    auto v = compileAndRun(
        "enum Inner { A(i64), B(i64), C }\n"
        "enum Outer { O(Inner), Z }\n"
        "fn pick(x: Outer) -> i64 {\n"
        "    match x {\n"
        "        O(A(n)) => n + 1,\n"
        "        O(B(n)) => n + 2,\n"
        "        O(C)    => 3,\n"
        "        Z       => 4,\n"
        "    }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    pick(O(A(100))) + pick(O(B(200))) + pick(O(C)) + pick(Z)\n"
        "}",
        "main", "dt_shared_outer_ctor");
    // 101 + 202 + 3 + 4 == 310
    expectEquals(v, 310, "dt_shared_outer_ctor");
}

} // namespace

int main() {
    test_constant_42();
    test_arithmetic_precedence();
    test_parenthesized();
    test_subtraction_left_assoc();
    test_signed_division();
    test_let_and_use();
    test_function_call();
    test_if_then();
    test_if_else();
    test_return_stmt();
    test_return_inside_if();
    test_return_inside_if_negative();
    test_recursive_fib_10();   // MVP: fib(10) == 55
    test_recursive_fib_20();   // 6765 — exercises ~13.5k recursive calls
    test_forward_reference();
    test_struct_pass_and_field_access();
    test_struct_let_bound();
    test_struct_literal_field_order_swapped();
    test_struct_return_from_fn();
    test_nested_struct();
    test_enum_unwrap_or_some();
    test_enum_unwrap_or_none();
    test_match_literal_arms();
    test_match_literal_wildcard_fallthrough();
    test_match_var_binds_scrutinee();
    test_enum_multi_arg_variant();
    test_enum_all_unit();
    test_nested_ctor_pattern();
    test_enum_with_struct_payload();
    test_dt_10_arm_literal_match();
    test_dt_shared_outer_ctor();
    std::cout << "All codegen tests passed (31 cases) — Phase 2.3c decision-tree codegen\n";
    return 0;
}
