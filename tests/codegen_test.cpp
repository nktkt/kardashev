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
    std::cout << "All codegen tests passed (20 cases) — Phase 2.1 structs\n";
    return 0;
}
