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

// --- Phase 3.1: generic-function end-to-end codegen tests ---

// Simplest possible monomorphization: a single generic `id<T>` instantiated
// at i64. Verifies the lazy emission path produces a working `id__i64`
// specialization that main() calls.
void test_generic_identity_i64() {
    auto v = compileAndRun(
        "fn id<T>(x: T) -> T { x }\n"
        "fn main() -> i64 { id(42) }",
        "main", "generic_identity_i64");
    expectEquals(v, 42, "generic_identity_i64");
}

// Two type parameters — exercises the mangler producing a multi-type
// suffix (`first__i64_i64`) and the substitution map handling more than
// one binding.
void test_generic_two_params_returns_first() {
    auto v = compileAndRun(
        "fn first<A, B>(a: A, b: B) -> A { a }\n"
        "fn main() -> i64 { first(7, 100) }",
        "main", "generic_two_params_returns_first");
    expectEquals(v, 7, "generic_two_params_returns_first");
}

// A generic function calling another generic function — emitCall inside
// wrap<U>'s body must route to the right `id` specialization based on
// the outer instance's substitution (U == i64 here, so id is instantiated
// at i64).
void test_generic_calling_generic() {
    auto v = compileAndRun(
        "fn id<T>(x: T) -> T { x }\n"
        "fn wrap<U>(u: U) -> U { id(u) }\n"
        "fn main() -> i64 { wrap(123) }",
        "main", "generic_calling_generic");
    expectEquals(v, 123, "generic_calling_generic");
}

// Two distinct instances of `id` — `id__Point` and `id__i64`. Confirms
// pendingInstances_ tracks unique (fnName, [type args]) tuples and emits
// both specializations.
void test_generic_multiple_instances() {
    auto v = compileAndRun(
        "struct Point { x: i64, y: i64 }\n"
        "fn id<T>(x: T) -> T { x }\n"
        "fn main() -> i64 {\n"
        "    let p = Point { x: 3, y: 4 };\n"
        "    let q = id(p);\n"
        "    let n = id(99);\n"
        "    n + q.x\n"
        "}",
        "main", "generic_multiple_instances");
    // 99 + 3 == 102
    expectEquals(v, 102, "generic_multiple_instances");
}

// --- Phase 3.2: generic structs and enums end-to-end codegen ---

// Simplest generic struct: Box<T> literal + field access at T = i64.
void test_generic_struct_box() {
    auto v = compileAndRun(
        "struct Box<T> { value: T }\n"
        "fn main() -> i64 { let b = Box { value: 42 }; b.value }",
        "main", "generic_struct_box");
    expectEquals(v, 42, "generic_struct_box");
}

// Generic enum with a single payload variant: Maybe<T> match at T = i64.
void test_generic_enum_maybe_match() {
    auto v = compileAndRun(
        "enum Maybe<T> { Mk(T) }\n"
        "fn main() -> i64 { let m = Mk(99); match m { Mk(v) => v } }",
        "main", "generic_enum_maybe_match");
    expectEquals(v, 99, "generic_enum_maybe_match");
}

// Generic function returning a generic struct: monomorphizes both
// make_pair<i64,i64> and Pair<i64,i64>.
void test_generic_fn_returns_generic_struct() {
    auto v = compileAndRun(
        "struct Pair<A, B> { first: A, second: B }\n"
        "fn make_pair<X, Y>(x: X, y: Y) -> Pair<X, Y> {\n"
        "    Pair { first: x, second: y }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    let p = make_pair(7, 100);\n"
        "    p.first + p.second\n"
        "}",
        "main", "generic_fn_returns_generic_struct");
    expectEquals(v, 107, "generic_fn_returns_generic_struct");
}

// Option<T> with both Some and None branches exercised; verifies
// instantiation reuse and that the None unit variant carries through.
void test_generic_option_unwrap_or() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "fn unwrap_or(o: Option<i64>, def: i64) -> i64 {\n"
        "    match o { Some(v) => v, None => def }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    let a = unwrap_or(Some(42), 0);\n"
        "    let b = unwrap_or(None, 99);\n"
        "    a + b\n"
        "}",
        "main", "generic_option_unwrap_or");
    expectEquals(v, 141, "generic_option_unwrap_or");
}

// Two-parameter generic enum: Result<T, E> — both Ok and Err carry payloads.
void test_generic_result_with_match() {
    auto v = compileAndRun(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "fn try_thing(n: i64) -> Result<i64, i64> {\n"
        "    if n < 0 { Err(0 - n) } else { Ok(n + 100) }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    match try_thing(7) { Ok(v) => v, Err(e) => 0 - e }\n"
        "}",
        "main", "generic_result_with_match");
    expectEquals(v, 107, "generic_result_with_match");
}

// --- Phase 3.3: traits, impl blocks, method calls, bounded generics ---

// Simplest trait+impl: Point.show() returns x + y.
void test_trait_basic_show() {
    auto v = compileAndRun(
        "trait Show { fn show(self) -> i64; }\n"
        "struct Point { x: i64, y: i64 }\n"
        "impl Show for Point { fn show(self) -> i64 { self.x + self.y } }\n"
        "fn main() -> i64 { let p = Point { x: 3, y: 4 }; p.show() }",
        "main", "trait_basic_show");
    expectEquals(v, 7, "trait_basic_show");
}

// Bounded generic <T: Show> dispatched at two different impl types.
void test_trait_bounded_generic() {
    auto v = compileAndRun(
        "trait Show { fn show(self) -> i64; }\n"
        "struct Point { x: i64, y: i64 }\n"
        "struct Line { len: i64 }\n"
        "impl Show for Point { fn show(self) -> i64 { self.x + self.y } }\n"
        "impl Show for Line { fn show(self) -> i64 { self.len } }\n"
        "fn use_show<T: Show>(t: T) -> i64 { t.show() }\n"
        "fn main() -> i64 {\n"
        "    let p = Point { x: 3, y: 4 };\n"
        "    let l = Line { len: 100 };\n"
        "    use_show(p) + use_show(l)\n"
        "}",
        "main", "trait_bounded_generic");
    expectEquals(v, 107, "trait_bounded_generic");
}

// Trait with multiple methods; impl supplies both.
void test_trait_multi_method() {
    auto v = compileAndRun(
        "trait Math { fn double(self) -> i64; fn square(self) -> i64; }\n"
        "struct N { v: i64 }\n"
        "impl Math for N {\n"
        "    fn double(self) -> i64 { self.v + self.v }\n"
        "    fn square(self) -> i64 { self.v * self.v }\n"
        "}\n"
        "fn main() -> i64 { let n = N { v: 5 }; n.double() + n.square() }",
        "main", "trait_multi_method");
    expectEquals(v, 35, "trait_multi_method");
}

// Method takes a non-self argument; receiver carries state.
void test_trait_method_with_args() {
    auto v = compileAndRun(
        "trait Adder { fn add_with(self, x: i64) -> i64; }\n"
        "struct N { v: i64 }\n"
        "impl Adder for N { fn add_with(self, x: i64) -> i64 { self.v + x } }\n"
        "fn main() -> i64 { let n = N { v: 10 }; n.add_with(32) }",
        "main", "trait_method_with_args");
    expectEquals(v, 42, "trait_method_with_args");
}

// --- Phase 3.4: postfix `?` (try) operator end-to-end codegen ---

// Ok-path: `parse(7)?` yields 7; main observes Ok(14) and unwraps via match.
void test_try_ok_path() {
    auto v = compileAndRun(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "fn parse(n: i64) -> Result<i64, i64> {\n"
        "    if n < 0 { Err(0 - n) } else { Ok(n) }\n"
        "}\n"
        "fn double(n: i64) -> Result<i64, i64> {\n"
        "    let x = parse(n)?;\n"
        "    Ok(x + x)\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    match double(7) { Ok(v) => v, Err(e) => 1000 + e }\n"
        "}",
        "main", "try_ok_path");
    expectEquals(v, 14, "try_ok_path");
}

// Err-path: parse(-5) returns Err(5); the `?` early-returns Err(5) from
// double, and main's Err arm yields 1000 + 5 == 1005.
void test_try_err_path() {
    auto v = compileAndRun(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "fn parse(n: i64) -> Result<i64, i64> {\n"
        "    if n < 0 { Err(0 - n) } else { Ok(n) }\n"
        "}\n"
        "fn double(n: i64) -> Result<i64, i64> {\n"
        "    let x = parse(n)?;\n"
        "    Ok(x + x)\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    match double(0 - 5) { Ok(v) => v, Err(e) => 1000 + e }\n"
        "}",
        "main", "try_err_path");
    expectEquals(v, 1005, "try_err_path");
}

// Three chained `?`s in sequence: x=5, each `step` adds 10. The Ok-path
// runs through all three calls, yielding 5 + 30 == 35.
void test_try_chained() {
    auto v = compileAndRun(
        "enum Result<T, E> { Ok(T), Err(E) }\n"
        "fn step(x: i64) -> Result<i64, i64> { Ok(x + 10) }\n"
        "fn run(x: i64) -> Result<i64, i64> {\n"
        "    let a = step(x)?;\n"
        "    let b = step(a)?;\n"
        "    let c = step(b)?;\n"
        "    Ok(c)\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    match run(5) { Ok(v) => v, Err(e) => 0 - e }\n"
        "}",
        "main", "try_chained");
    expectEquals(v, 35, "try_chained");
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
    test_generic_identity_i64();
    test_generic_two_params_returns_first();
    test_generic_calling_generic();
    test_generic_multiple_instances();
    // Phase 3.2 generic structs and enums
    test_generic_struct_box();
    test_generic_enum_maybe_match();
    test_generic_fn_returns_generic_struct();
    test_generic_option_unwrap_or();
    test_generic_result_with_match();
    // Phase 3.3 traits + impl + bounded generics
    test_trait_basic_show();
    test_trait_bounded_generic();
    test_trait_multi_method();
    test_trait_method_with_args();
    // Phase 3.4 try operator
    test_try_ok_path();
    test_try_err_path();
    test_try_chained();
    std::cout << "All codegen tests passed (47 cases) — Phase 3.3 traits + Phase 3.4 try\n";
    return 0;
}
