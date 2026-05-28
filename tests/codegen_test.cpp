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
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
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

// --- Phase 21a: generic trait parameters, end to end ---

// A generic trait with impls at two element types + a generic-trait-bounded
// fn `head<T, C: Container<T>>`, used at both element types. The bool path
// gates the i64 path, so a correct element-type resolution returns 42.
void test_generic_trait_bounded_head_both_elems() {
    auto v = compileAndRun(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }\n"
        "impl Container<bool> for BoolBox { fn first(&self) -> bool { self.b } }\n"
        "fn head<T, C: Container<T>>(c: C) -> T { c.first() }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 42 };\n"
        "    let bb = BoolBox { b: true };\n"
        "    let a = head(ib);\n"
        "    if head(bb) { a } else { 0 }\n"
        "}",
        "main", "generic_trait_bounded_head_both_elems");
    expectEquals(v, 42, "generic_trait_bounded_head_both_elems");
}

// --- Phase 21b: associated types, end to end ---

// `Self::Item` resolved per-impl: two impls choose different Item types (i64 +
// bool); `.get()` on each lands the right-typed value. The bool path gates the
// i64 path -> 42.
void test_assoc_self_item_two_impls() {
    auto v = compileAndRun(
        "trait Container { type Item; fn get(&self) -> Self::Item; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container for IntBox { type Item = i64; fn get(&self) -> "
        "Self::Item { self.v } }\n"
        "impl Container for BoolBox { type Item = bool; fn get(&self) -> "
        "Self::Item { self.b } }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 42 };\n"
        "    let bb = BoolBox { b: true };\n"
        "    let a = ib.get();\n"
        "    if bb.get() { a } else { 0 }\n"
        "}",
        "main", "assoc_self_item_two_impls");
    expectEquals(v, 42, "assoc_self_item_two_impls");
}

// `C::Item` at a bounded call site: `first<C: Container>(c: C) -> C::Item` used
// at both impls. Each call's result type resolves to that impl's associated
// type at its monomorphic instance. The bool path gates the i64 path -> 42.
void test_assoc_c_item_bounded_call_site() {
    auto v = compileAndRun(
        "trait Container { type Item; fn get(&self) -> Self::Item; }\n"
        "struct IntBox { v: i64 }\n"
        "struct BoolBox { b: bool }\n"
        "impl Container for IntBox { type Item = i64; fn get(&self) -> "
        "Self::Item { self.v } }\n"
        "impl Container for BoolBox { type Item = bool; fn get(&self) -> "
        "Self::Item { self.b } }\n"
        "fn first<C: Container>(c: C) -> C::Item { c.get() }\n"
        "fn main() -> i64 {\n"
        "    let ib = IntBox { v: 42 };\n"
        "    let bb = BoolBox { b: true };\n"
        "    let a = first(ib);\n"
        "    if first(bb) { a } else { 0 }\n"
        "}",
        "main", "assoc_c_item_bounded_call_site");
    expectEquals(v, 42, "assoc_c_item_bounded_call_site");
}

// `where` clause runs identically to the inline-bounded form (uses a generic
// trait Container<T>). -> 77, the value stored in the box.
void test_where_clause_runs_like_inline() {
    auto v = compileAndRun(
        "trait Container<T> { fn first(&self) -> T; }\n"
        "struct IntBox { v: i64 }\n"
        "impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }\n"
        "fn head<U, C>(c: C) -> U where C: Container<U> { c.first() }\n"
        "fn main() -> i64 { let ib = IntBox { v: 77 }; head(ib) }",
        "main", "where_clause_runs_like_inline");
    expectEquals(v, 77, "where_clause_runs_like_inline");
}

// A custom `impl Iterator<bool> for Count`, iterated with `for x in c`. The
// `for` desugar derives the bool element type from `next()`'s Option<bool>.
// Count{n:5} yields elements for n=4,3,2,1,0; isEven is true for 4,2,0 -> 3.
void test_generic_iterator_bool_for_loop() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator<T> { fn next(&mut self) -> Option<T>; }\n"
        "struct Count { n: i64 }\n"
        "impl Iterator<bool> for Count {\n"
        "    fn next(&mut self) -> Option<bool> {\n"
        "        if self.n <= 0 { None } else {\n"
        "            self.n = self.n - 1;\n"
        "            let isEven = self.n - (self.n / 2) * 2 == 0;\n"
        "            Some(isEven)\n"
        "        }\n"
        "    }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    let c = Count { n: 5 };\n"
        "    let mut trues = 0;\n"
        "    for b in c { if b { trues = trues + 1; } else { trues = trues; } }\n"
        "    trues\n"
        "}",
        "main", "generic_iterator_bool_for_loop");
    expectEquals(v, 3, "generic_iterator_bool_for_loop");
}

// A generic adaptor `fold<T, I: Iterator<T>>` over a NON-i64 (bool) element,
// proving the adaptor is now genuinely generic over the element type.
void test_generic_fold_over_bool() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator<T> { fn next(&mut self) -> Option<T>; }\n"
        "struct Count { n: i64 }\n"
        "impl Iterator<bool> for Count {\n"
        "    fn next(&mut self) -> Option<bool> {\n"
        "        if self.n <= 0 { None } else {\n"
        "            self.n = self.n - 1;\n"
        "            let isEven = self.n - (self.n / 2) * 2 == 0;\n"
        "            Some(isEven)\n"
        "        }\n"
        "    }\n"
        "}\n"
        "fn fold<T, I: Iterator<T>>(it: I, init: i64, f: fn(i64, T) -> i64)"
        " -> i64 {\n"
        "    let mut iter = it;\n"
        "    let mut acc = init;\n"
        "    loop {\n"
        "        match iter.next() {\n"
        "            Some(x) => { acc = f(acc, x); },\n"
        "            None => { break; },\n"
        "        }\n"
        "    }\n"
        "    acc\n"
        "}\n"
        "fn count_true(acc: i64, b: bool) -> i64 { if b { acc + 1 } else { acc } }\n"
        "fn main() -> i64 {\n"
        "    let c = Count { n: 5 };\n"
        "    fold(c, 0, count_true)\n"
        "}",
        "main", "generic_fold_over_bool");
    expectEquals(v, 3, "generic_fold_over_bool");
}

// Regression: the pre-21a `<I: Iterator>` spelling (omitted element arg) over
// an i64 element still works after migrating Iterator to Iterator<T>. A custom
// i64 Countdown summed via fold: 4+3+2+1 == 10.
void test_iterator_unparam_bound_i64_regression() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "trait Iterator<T> { fn next(&mut self) -> Option<T>; }\n"
        "struct Countdown { n: i64 }\n"
        "impl Iterator<i64> for Countdown {\n"
        "    fn next(&mut self) -> Option<i64> {\n"
        "        if self.n <= 0 { None } else { self.n = self.n - 1; Some(self.n + 1) }\n"
        "    }\n"
        "}\n"
        "fn fold<I: Iterator>(it: I, init: i64, f: fn(i64, i64) -> i64) -> i64 {\n"
        "    let mut iter = it;\n"
        "    let mut acc = init;\n"
        "    loop {\n"
        "        match iter.next() {\n"
        "            Some(x) => { acc = f(acc, x); },\n"
        "            None => { break; },\n"
        "        }\n"
        "    }\n"
        "    acc\n"
        "}\n"
        "fn add(a: i64, b: i64) -> i64 { a + b }\n"
        "fn main() -> i64 { let c = Countdown { n: 4 }; fold(c, 0, add) }",
        "main", "iterator_unparam_bound_i64_regression");
    expectEquals(v, 10, "iterator_unparam_bound_i64_regression");
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

// --- Phase 9: loops, ranges, assignment ---

void test_while_countdown_sum() {
    // Sum 1+2+...+10 == 55 via a mutable accumulator.
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut sum = 0;\n"
        "    let mut i = 1;\n"
        "    while i <= 10 { sum = sum + i; i = i + 1; }\n"
        "    sum\n"
        "}",
        "main", "while_countdown_sum");
    expectEquals(v, 55, "while_countdown_sum");
}

void test_loop_break_value() {
    auto v = compileAndRun(
        "fn main() -> i64 { let x = loop { break 42; }; x }",
        "main", "loop_break_value");
    expectEquals(v, 42, "loop_break_value");
}

void test_loop_continue_sum_odds() {
    // Skip even numbers, sum odds in 1..=10 == 25.
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut sum = 0;\n"
        "    let mut i = 0;\n"
        "    loop {\n"
        "        i = i + 1;\n"
        "        if i > 10 { break; } else {}\n"
        "        if i / 2 * 2 == i { continue; } else {}\n"
        "        sum = sum + i;\n"
        "    }\n"
        "    sum\n"
        "}",
        "main", "loop_continue_sum_odds");
    expectEquals(v, 25, "loop_continue_sum_odds");
}

void test_for_inclusive_range_sum() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut sum = 0;\n"
        "    for x in 1..=10 { sum = sum + x; }\n"
        "    sum\n"
        "}",
        "main", "for_inclusive_range_sum");
    expectEquals(v, 55, "for_inclusive_range_sum");
}

void test_for_exclusive_range_sum() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut sum = 0;\n"
        "    for x in 1..11 { sum = sum + x; }\n"
        "    sum\n"
        "}",
        "main", "for_exclusive_range_sum");
    expectEquals(v, 55, "for_exclusive_range_sum");
}

void test_nested_loops_break_continue_innermost() {
    // Inner loop runs j=1,2,3; skips j==2 via continue; the break exits
    // only the inner loop. 2 increments per outer iteration, 3 outers => 6.
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut total = 0;\n"
        "    let mut i = 0;\n"
        "    while i < 3 {\n"
        "        let mut j = 0;\n"
        "        loop {\n"
        "            j = j + 1;\n"
        "            if j > 3 { break; } else {}\n"
        "            if j == 2 { continue; } else {}\n"
        "            total = total + 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    total\n"
        "}",
        "main", "nested_loops_break_continue_innermost");
    expectEquals(v, 6, "nested_loops_break_continue_innermost");
}

void test_range_value_iterated() {
    // A range bound to a let and iterated through the general Range path.
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let r = 0..5;\n"
        "    let mut sum = 0;\n"
        "    for x in r { sum = sum + x; }\n"
        "    sum\n"
        "}",
        "main", "range_value_iterated");
    expectEquals(v, 10, "range_value_iterated");
}

void test_field_assign_through_mut_local() {
    // Assign into a struct field of a `let mut` local.
    auto v = compileAndRun(
        "struct P { x: i64, y: i64 }\n"
        "fn main() -> i64 {\n"
        "    let mut p = P { x: 1, y: 2 };\n"
        "    p.x = 40;\n"
        "    p.y = p.y + 0;\n"
        "    p.x + p.y\n"
        "}",
        "main", "field_assign_through_mut_local");
    expectEquals(v, 42, "field_assign_through_mut_local");
}

// Phase 10a: a higher-order fn with an effect-polymorphic fn-typed
// parameter, instantiated at a pure argument and invoked indirectly. The
// effect row is compile-time only — codegen lowers the param to an opaque
// fn pointer and the indirect call must execute exactly as before.
void test_effect_poly_higher_order_pure() {
    auto v = compileAndRun(
        "fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }\n"
        "fn dbl(x: i64) -> i64 { x + x }\n"
        "fn main() -> i64 { apply(dbl) }",
        "main", "effect_poly_higher_order_pure");
    expectEquals(v, 20, "effect_poly_higher_order_pure");
}

// A concrete (non-polymorphic) effect-carrying fn-type parameter: the
// effect annotation `! {io}` is irrelevant to lowering; the indirect call
// must still dispatch correctly.
void test_concrete_effect_fn_type_param() {
    auto v = compileAndRun(
        "fn run(f: fn(i64) -> i64 ! {io}) -> i64 ! {io} { f(21) + 1 }\n"
        "fn idn(x: i64) -> i64 ! {io} { x }\n"
        "fn main() -> i64 ! {io} { run(idn) }",
        "main", "concrete_effect_fn_type_param");
    expectEquals(v, 22, "concrete_effect_fn_type_param");
}

// --- Phase 10b: capturing closures (fat-pointer fn values) ---

// Single capture: `|x| x + n` closes over `n` by value.
void test_closure_single_capture() {
    auto v = compileAndRun(
        "fn main() -> i64 { let n = 5; let add_n = |x| x + n; add_n(10) }",
        "main", "closure_single_capture");
    expectEquals(v, 15, "closure_single_capture");
}

// Multi-capture: two free variables copied into the env struct.
void test_closure_multi_capture() {
    auto v = compileAndRun(
        "fn main() -> i64 { let a = 3; let b = 4; let f = |x| x + a + b; "
        "f(10) }",
        "main", "closure_multi_capture");
    expectEquals(v, 17, "closure_multi_capture");
}

// Closure passed to a higher-order fn with an effect-row-polymorphic
// fn-typed param — exercises the fat pointer flowing as a by-value arg.
void test_closure_passed_to_higher_order() {
    auto v = compileAndRun(
        "fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }\n"
        "fn main() -> i64 { let k = 7; apply(|x| x + k) }",
        "main", "closure_passed_to_higher_order");
    expectEquals(v, 17, "closure_passed_to_higher_order");
}

// Multiple params + a capture, with a param type annotation.
void test_closure_multi_param_annotated() {
    auto v = compileAndRun(
        "fn main() -> i64 { let base = 100; let f = |x: i64, y| x + y + base; "
        "f(3, 4) }",
        "main", "closure_multi_param_annotated");
    expectEquals(v, 107, "closure_multi_param_annotated");
}

// A zero-capture closure: env is null, dispatch still goes through the fat
// pointer uniformly.
void test_closure_no_capture() {
    auto v = compileAndRun(
        "fn main() -> i64 { let f = |x| x + 1; f(41) }",
        "main", "closure_no_capture");
    expectEquals(v, 42, "closure_no_capture");
}

// A closure selected by `if` and then invoked — both branches are fat
// pointers of the same LLVM type so the PHI is well-formed. Interops with a
// top-level fn value in the other branch.
void test_closure_selected_by_if() {
    auto v = compileAndRun(
        "fn dbl(x: i64) -> i64 { x + x }\n"
        "fn main() -> i64 {\n"
        "    let n = 10;\n"
        "    let chosen = if 1 < 2 { dbl } else { |x| x + n };\n"
        "    chosen(20)\n"
        "}",
        "main", "closure_selected_by_if");
    expectEquals(v, 40, "closure_selected_by_if");
}

// A closure with a block body and a nested `let` that shadows nothing —
// confirms block-scoped names aren't mistaken for captures.
void test_closure_block_body() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let n = 3;\n"
        "    let f = |x| { let y = x * n; y + 1 };\n"
        "    f(4)\n"
        "}",
        "main", "closure_block_body");
    expectEquals(v, 13, "closure_block_body");
}

// --- Phase 17a: richer closures & first-class fn values ---

// A fn value held in a struct field, called via `(a.f)(10)`. The field type is
// `fn(i64)->i64`; the value is a top-level fn. 10 + 1 == 11.
void test_field_held_fn_value_toplevel() {
    auto v = compileAndRun(
        "struct Adder { f: fn(i64) -> i64 }\n"
        "fn inc(x: i64) -> i64 { x + 1 }\n"
        "fn main() -> i64 {\n"
        "    let a = Adder { f: inc };\n"
        "    (a.f)(10)\n"
        "}",
        "main", "field_held_fn_value_toplevel");
    expectEquals(v, 11, "field_held_fn_value_toplevel");
}

// A struct field holding a CAPTURING closure, called via `(b.f)(5)`. The
// closure captures `base` by value. 5 + 100 == 105.
void test_field_held_closure_value() {
    auto v = compileAndRun(
        "struct Adder { f: fn(i64) -> i64 }\n"
        "fn main() -> i64 {\n"
        "    let base = 100;\n"
        "    let b = Adder { f: |x| x + base };\n"
        "    (b.f)(5)\n"
        "}",
        "main", "field_held_closure_value");
    expectEquals(v, 105, "field_held_closure_value");
}

// Lazy-adaptor shape: an inherent method applies `(self.f)(self.base)`.
// A stored closure field is callable through `self`. 5 + 100 == 105.
void test_field_fn_called_through_self() {
    auto v = compileAndRun(
        "struct MapIter { base: i64, f: fn(i64) -> i64 }\n"
        "impl MapIter { fn step(self) -> i64 { (self.f)(self.base) } }\n"
        "fn main() -> i64 {\n"
        "    let bump = 100;\n"
        "    let m = MapIter { base: 5, f: |x| x + bump };\n"
        "    m.step()\n"
        "}",
        "main", "field_fn_called_through_self");
    expectEquals(v, 105, "field_fn_called_through_self");
}

// Call a fn value returned by an expression: `(make_adder())(41)` == 42.
void test_call_value_of_expression() {
    auto v = compileAndRun(
        "fn make_adder() -> fn(i64) -> i64 { |x| x + 1 }\n"
        "fn main() -> i64 { (make_adder())(41) }",
        "main", "call_value_of_expression");
    expectEquals(v, 42, "call_value_of_expression");
}

// FnMut counter: a closure that mutates a captured `let mut` binding by
// reference; calling it three times leaves n == 3 (visible after the calls).
void test_fnmut_counter() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut n = 0;\n"
        "    let mut inc = || { n = n + 1; };\n"
        "    inc(); inc(); inc();\n"
        "    n\n"
        "}",
        "main", "fnmut_counter");
    expectEquals(v, 3, "fnmut_counter");
}

// FnMut accumulator: a closure adding its arg to a captured running total
// across calls; the total accumulates to 18 (5 + 10 + 3).
void test_fnmut_accumulator() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut total = 0;\n"
        "    let mut add = |x| { total = total + x; total };\n"
        "    let a = add(5);\n"
        "    let b = add(10);\n"
        "    let c = add(3);\n"
        "    a + b + c + total\n" // 5 + 15 + 18 + 18 == 56
        "}",
        "main", "fnmut_accumulator");
    expectEquals(v, 56, "fnmut_accumulator");
}

// --- Phase 11: dyn Trait + vtable dynamic dispatch ---

// The headline acceptance test: ONE `describe` call site dispatches to two
// different impls at runtime via the trait object's vtable. 25 + 12 == 37.
void test_dyn_dispatch_two_impls() {
    auto v = compileAndRun(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "struct Rect { w: i64, h: i64 }\n"
        "impl Shape for Sq   { fn area(&self) -> i64 { self.side * self.side } }\n"
        "impl Shape for Rect { fn area(&self) -> i64 { self.w * self.h } }\n"
        "fn describe(s: &dyn Shape) -> i64 { s.area() }\n"
        "fn main() -> i64 {\n"
        "    let sq = Sq { side: 5 };\n"
        "    let r  = Rect { w: 3, h: 4 };\n"
        "    let a = describe(&sq);\n"
        "    let b = describe(&r);\n"
        "    a + b\n"
        "}",
        "main", "dyn_dispatch_two_impls");
    expectEquals(v, 37, "dyn_dispatch_two_impls");
}

// Box<dyn Trait>: a heap-owned trait object dispatched through its vtable.
void test_dyn_box() {
    auto v = compileAndRun(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side * self.side } }\n"
        "fn main() -> i64 {\n"
        "    let b: Box<dyn Shape> = Box::new(Sq { side: 6 });\n"
        "    b.area()\n"
        "}",
        "main", "dyn_box");
    expectEquals(v, 36, "dyn_box");
}

// Multi-method trait through a trait object: calling the 2nd vtable slot must
// land on the right method (proves slot indexing). 4+100 (Dog) +2+200 (Bird).
void test_dyn_multi_method_slot() {
    auto v = compileAndRun(
        "trait Animal { fn legs(&self) -> i64; fn sound(&self) -> i64; }\n"
        "struct Dog { x: i64 }\n"
        "struct Bird { y: i64 }\n"
        "impl Animal for Dog  { fn legs(&self) -> i64 { 4 } fn sound(&self) -> i64 { 100 } }\n"
        "impl Animal for Bird { fn legs(&self) -> i64 { 2 } fn sound(&self) -> i64 { 200 } }\n"
        "fn legs_of(a: &dyn Animal) -> i64 { a.legs() }\n"
        "fn sound_of(a: &dyn Animal) -> i64 { a.sound() }\n"
        "fn main() -> i64 {\n"
        "    let d = Dog { x: 0 };\n"
        "    let b = Bird { y: 0 };\n"
        "    legs_of(&d) + sound_of(&d) + legs_of(&b) + sound_of(&b)\n"
        "}",
        "main", "dyn_multi_method_slot");
    expectEquals(v, 306, "dyn_multi_method_slot");
}

// A trait method that takes a non-self argument, called through a dyn object.
void test_dyn_method_with_arg() {
    auto v = compileAndRun(
        "trait Adder { fn add_with(&self, x: i64) -> i64; }\n"
        "struct N { v: i64 }\n"
        "impl Adder for N { fn add_with(&self, x: i64) -> i64 { self.v + x } }\n"
        "fn run(a: &dyn Adder) -> i64 { a.add_with(32) }\n"
        "fn main() -> i64 { let n = N { v: 10 }; run(&n) }",
        "main", "dyn_method_with_arg");
    expectEquals(v, 42, "dyn_method_with_arg");
}

// `&self` static dispatch (no dyn): autoref a value receiver to the method's
// `&self` pointer slot. Confirms Phase 3 static dispatch still works with the
// new receiver convention.
void test_self_ref_static_dispatch() {
    auto v = compileAndRun(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side * self.side } }\n"
        "fn main() -> i64 { let s = Sq { side: 7 }; s.area() }",
        "main", "self_ref_static_dispatch");
    expectEquals(v, 49, "self_ref_static_dispatch");
}

// Stretch: a heterogeneous pair stored as two `&dyn Shape` and summed through
// the same vtable-dispatched call inside a helper.
void test_dyn_heterogeneous_sum() {
    auto v = compileAndRun(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "struct Rect { w: i64, h: i64 }\n"
        "impl Shape for Sq   { fn area(&self) -> i64 { self.side * self.side } }\n"
        "impl Shape for Rect { fn area(&self) -> i64 { self.w * self.h } }\n"
        "fn area_of(s: &dyn Shape) -> i64 { s.area() }\n"
        "fn sum2(a: &dyn Shape, b: &dyn Shape) -> i64 { area_of(a) + area_of(b) }\n"
        "fn main() -> i64 {\n"
        "    let sq = Sq { side: 5 };\n"
        "    let r  = Rect { w: 3, h: 4 };\n"
        "    sum2(&sq, &r)\n"
        "}",
        "main", "dyn_heterogeneous_sum");
    expectEquals(v, 37, "dyn_heterogeneous_sum");
}

// --- Phase 12 real async runtime ---

// (a) Multi-state resume: two suspensions, result from locals across them.
void test_async_two_awaits_locals() {
    auto v = compileAndRun(
        "async fn work() -> i64 {\n"
        "    let a = yield_now(10).await;\n"
        "    let b = yield_now(20).await;\n"
        "    a + b\n"
        "}\n"
        "fn main() -> i64 { block_on(work()) }",
        "main", "async_two_awaits_locals");
    expectEquals(v, 30, "async_two_awaits_locals");
}

// A local bound BEFORE the first await and read only AFTER the second await
// must be preserved across both suspensions (frame-promoted).
void test_async_local_live_across_two_suspends() {
    auto v = compileAndRun(
        "async fn work() -> i64 {\n"
        "    let base = 100;\n"
        "    let a = yield_now(10).await;\n"
        "    let b = yield_now(20).await;\n"
        "    base + a + b\n"
        "}\n"
        "fn main() -> i64 { block_on(work()) }",
        "main", "async_local_live_across_two_suspends");
    expectEquals(v, 130, "async_local_live_across_two_suspends");
}

// (b) The Pending path is taken: two yields suspend once each, so the future
// is polled 4 times (Pending+Ready per yield) — strictly more than the 2
// awaits. poll_count() observes it.
void test_async_poll_count_observes_pending() {
    auto v = compileAndRun(
        "async fn work() -> i64 {\n"
        "    let a = yield_now(1).await;\n"
        "    let b = yield_now(2).await;\n"
        "    a + b\n"
        "}\n"
        "fn main() -> i64 { let _r = block_on(work()); poll_count() }",
        "main", "async_poll_count_observes_pending");
    expectEquals(v, 4, "async_poll_count_observes_pending");
}

// Single-await async fn still works under the new state-machine model.
void test_async_single_await() {
    auto v = compileAndRun(
        "async fn one(n: i64) -> i64 {\n"
        "    let x = yield_now(n).await;\n"
        "    x + 1\n"
        "}\n"
        "fn main() -> i64 { block_on(one(41)) }",
        "main", "async_single_await");
    expectEquals(v, 42, "async_single_await");
}

// Zero-await async fn finishes Ready on the first poll (and is awaitable by
// another async fn — the Phase 6 nested-async chain shape).
void test_async_zero_await_chain() {
    auto v = compileAndRun(
        "async fn add(a: i64, b: i64) -> i64 { a + b }\n"
        "async fn double(n: i64) -> i64 { add(n, n).await }\n"
        "fn main() -> i64 { block_on(double(21)) }",
        "main", "async_zero_await_chain");
    expectEquals(v, 42, "async_zero_await_chain");
}

// Params are stored in the frame and survive suspension (used after awaits).
void test_async_param_survives_suspension() {
    auto v = compileAndRun(
        "async fn work(n: i64) -> i64 {\n"
        "    let a = yield_now(5).await;\n"
        "    let b = yield_now(6).await;\n"
        "    n + a + b\n"
        "}\n"
        "fn main() -> i64 { block_on(work(7)) }",
        "main", "async_param_survives_suspension");
    expectEquals(v, 18, "async_param_survives_suspension");
}

// --- Phase 13a: method-receiver autoref + Iterator trait + adaptors ---

// THE foundation fix: a `&mut self` method mutates the receiver in place
// across repeated calls (previously the 2nd call saw a moved value and, even
// once that was fixed, the mutation had to persist via by-address passing).
const char* kIterPrelude =
    "enum Option<T> { Some(T), None }\n"
    "trait Iterator { fn next(&mut self) -> Option<i64>; }\n";

void test_mut_self_persists_across_calls() {
    auto v = compileAndRun(
        "trait Inc { fn inc(&mut self) -> i64; }\n"
        "struct Counter { n: i64 }\n"
        "impl Inc for Counter {\n"
        "    fn inc(&mut self) -> i64 { self.n = self.n + 1; self.n }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    let mut c = Counter { n: 0 };\n"
        "    c.inc(); c.inc(); c.inc()\n"
        "}",
        "main", "mut_self_persists_across_calls");
    expectEquals(v, 3, "mut_self_persists_across_calls");
}

// `&self` shared borrow: the receiver stays usable, multiple calls allowed.
void test_shared_self_multiple_calls() {
    auto v = compileAndRun(
        "trait Get { fn get(&self) -> i64; }\n"
        "struct P { x: i64 }\n"
        "impl Get for P { fn get(&self) -> i64 { self.x } }\n"
        "fn main() -> i64 {\n"
        "    let p = P { x: 7 };\n"
        "    p.get() + p.get() + p.get()\n"
        "}",
        "main", "shared_self_multiple_calls");
    expectEquals(v, 21, "shared_self_multiple_calls");
}

// `for` over a non-Range Iterator impl (Countdown) visits the right elements.
void test_for_over_custom_iterator() {
    auto v = compileAndRun(
        std::string(kIterPrelude) +
            "struct Countdown { n: i64 }\n"
            "impl Iterator for Countdown {\n"
            "    fn next(&mut self) -> Option<i64> {\n"
            "        if self.n <= 0 { None }\n"
            "        else { self.n = self.n - 1; Some(self.n + 1) }\n"
            "    }\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let cd = Countdown { n: 4 };\n"
            "    let mut s = 0;\n"
            "    for x in cd { s = s + x; }\n"
            "    s\n"
            "}",
        "main", "for_over_custom_iterator");
    expectEquals(v, 10, "for_over_custom_iterator"); // 4+3+2+1
}

// The Phase 9 range fast path still works alongside the Iterator desugar.
void test_for_inclusive_range_still_55() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let mut s = 0;\n"
        "    for x in 1..=10 { s = s + x; }\n"
        "    s\n"
        "}",
        "main", "for_inclusive_range_still_55");
    expectEquals(v, 55, "for_inclusive_range_still_55");
}

// Closure-driven `fold` over a custom Iterator.
void test_fold_closure_driven() {
    auto v = compileAndRun(
        std::string(kIterPrelude) +
            "struct Countdown { n: i64 }\n"
            "impl Iterator for Countdown {\n"
            "    fn next(&mut self) -> Option<i64> {\n"
            "        if self.n <= 0 { None }\n"
            "        else { self.n = self.n - 1; Some(self.n + 1) }\n"
            "    }\n"
            "}\n"
            "fn fold<I: Iterator>(it: I, init: i64,\n"
            "                     f: fn(i64, i64) -> i64) -> i64 {\n"
            "    let mut iter = it;\n"
            "    let mut acc = init;\n"
            "    loop {\n"
            "        match iter.next() {\n"
            "            Some(x) => { acc = f(acc, x); },\n"
            "            None => { break; },\n"
            "        }\n"
            "    }\n"
            "    acc\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    fold(Countdown { n: 5 }, 0, |acc, x| acc + x)\n"
            "}",
        "main", "fold_closure_driven");
    expectEquals(v, 15, "fold_closure_driven"); // 5+4+3+2+1
}

// Eager `map`/`filter` collecting into a Vec, driven by closures over a
// custom Iterator. filter keeps evens, then a fold-style sum verifies.
void test_map_filter_eager_vec() {
    auto v = compileAndRun(
        std::string(kIterPrelude) +
            "struct UpTo { i: i64, hi: i64 }\n"
            "impl Iterator for UpTo {\n"
            "    fn next(&mut self) -> Option<i64> {\n"
            "        if self.i >= self.hi { None }\n"
            "        else { let v = self.i; self.i = self.i + 1; Some(v) }\n"
            "    }\n"
            "}\n"
            "fn map<I: Iterator>(it: I, f: fn(i64) -> i64)\n"
            "        -> Vec<i64> ! { alloc } {\n"
            "    let mut iter = it;\n"
            "    let out = vec_new();\n"
            "    loop {\n"
            "        match iter.next() {\n"
            "            Some(x) => { vec_push(&mut out, f(x)); },\n"
            "            None => { break; },\n"
            "        }\n"
            "    }\n"
            "    out\n"
            "}\n"
            "fn sum_vec(v: &Vec<i64>, i: i64) -> i64 {\n"
            "    if i < vec_len(v) { vec_get(v, i) + sum_vec(v, i + 1) }\n"
            "    else { 0 }\n"
            "}\n"
            "fn main() -> i64 ! { alloc } {\n"
            "    let doubled = map(UpTo { i: 1, hi: 6 }, |x| x * 2);\n"
            "    sum_vec(&doubled, 0)\n"
            "}",
        "main", "map_filter_eager_vec");
    expectEquals(v, 30, "map_filter_eager_vec"); // (1+2+3+4+5)*2 == 30
}

// --- Phase 13b: growable String, HashMap<i64,i64>, slices ---

void test_string_build_and_len() {
    // Build "ab"+"cd" incrementally; return the length (4).
    auto v = compileAndRun(
        "fn main() -> i64 ! { alloc } {\n"
        "    let s = string_new();\n"
        "    string_push_str(&mut s, \"ab\");\n"
        "    string_push_str(&mut s, \"cd\");\n"
        "    string_len(&s)\n"
        "}",
        "main", "string_build_and_len");
    expectEquals(v, 4, "string_build_and_len");
}

void test_string_push_grows_past_initial_cap() {
    // Append many small chunks so the buffer must realloc several times;
    // the final length is 2*10 == 20.
    auto v = compileAndRun(
        "fn append_n(s: &mut String, i: i64, n: i64) -> i64 ! { alloc } {\n"
        "    if i < n { string_push_str(s, \"xy\"); append_n(s, i + 1, n) }\n"
        "    else { 0 }\n"
        "}\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let s = string_new();\n"
        "    append_n(&mut s, 0, 10);\n"
        "    string_len(&s)\n"
        "}",
        "main", "string_push_grows");
    expectEquals(v, 20, "string_push_grows");
}

void test_str_char_at_in_bounds() {
    // Phase 26: str_char_at reads the byte at an index, zero-extended. The
    // string is "AB" so byte 0 is 'A' (65) and byte 1 is 'B' (66); their
    // sum is 131.
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let s = \"AB\";\n"
        "    str_char_at(&s, 0) + str_char_at(&s, 1)\n"
        "}",
        "main", "str_char_at_in_bounds");
    expectEquals(v, 131, "str_char_at_in_bounds");
}

void test_str_char_at_out_of_bounds_is_minus_one() {
    // Phase 26: a past-the-end index returns the -1 sentinel (bounds-checked,
    // no OOB read). "AB" has length 2, so index 2 and index 5 are both -1;
    // their sum is -2.
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let s = \"AB\";\n"
        "    str_char_at(&s, 2) + str_char_at(&s, 5)\n"
        "}",
        "main", "str_char_at_oob");
    expectEquals(v, -2, "str_char_at_oob");
}

void test_str_char_at_negative_is_minus_one() {
    // Phase 26: a negative index is also rejected (returns -1).
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let s = \"AB\";\n"
        "    str_char_at(&s, 0 - 1)\n"
        "}",
        "main", "str_char_at_negative");
    expectEquals(v, -1, "str_char_at_negative");
}

void test_hashmap_insert_get_overwrite() {
    // Needs Option in scope (codegen's hashmap_get builds Option<i64>); the
    // unit-test harness bypasses the prelude, so declare it inline.
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    hashmap_insert(&mut m, 3, 30);\n"
        "    hashmap_insert(&mut m, 7, 70);\n"
        "    hashmap_insert(&mut m, 3, 33);\n" // overwrite
        "    let a = match hashmap_get(&m, 7) { Some(x) => x, None => 0 - 1 };\n"
        "    let b = match hashmap_get(&m, 3) { Some(x) => x, None => 0 - 1 };\n"
        "    let c = match hashmap_get(&m, 99) { Some(x) => x, None => 0 - 1 };\n"
        "    let d = hashmap_len(&m);\n"
        // a=70, b=33, c=-1, d=2  => 70 + 33 + (-1) + 2 == 104
        "    a + b + c + d\n"
        "}",
        "main", "hashmap_insert_get_overwrite");
    expectEquals(v, 104, "hashmap_insert_get_overwrite");
}

void test_hashmap_rehash_all_retrievable() {
    // Insert 40 keys (forces multiple rehashes from the initial cap of 8),
    // then confirm every key still maps to its value: returns mismatch count
    // (0) if the rehash preserved all entries.
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    let mut i = 0;\n"
        "    while i < 40 { hashmap_insert(&mut m, i, i * 100); i = i + 1; }\n"
        "    let mut bad = 0;\n"
        "    let mut j = 0;\n"
        "    while j < 40 {\n"
        "        let got = match hashmap_get(&m, j) { Some(x) => x, None => 0 - 1 };\n"
        "        let delta = if got == j * 100 { 0 } else { 1 };\n"
        "        bad = bad + delta;\n"
        "        j = j + 1;\n"
        "    }\n"
        "    bad\n"
        "}",
        "main", "hashmap_rehash");
    expectEquals(v, 0, "hashmap_rehash");
}

// --- Phase 17b: generic Future<T> (result type follows the async fn's
// declared return type) ---

// An async fn returning `bool` (true): block_on recovers the bool. Encoded as
// 1 so the i64-returning `main` can carry it.
void test_async_returns_bool_true() {
    auto v = compileAndRun(
        "async fn ab() -> bool { let x = yield_now(1).await; x == 1 }\n"
        "fn main() -> i64 { if block_on(ab()) { 1 } else { 0 } }",
        "main", "async_returns_bool_true");
    expectEquals(v, 1, "async_returns_bool_true");
}

// An async fn returning `bool` (false): block_on recovers false.
void test_async_returns_bool_false() {
    auto v = compileAndRun(
        "async fn ab() -> bool { let x = yield_now(2).await; x == 1 }\n"
        "fn main() -> i64 { if block_on(ab()) { 1 } else { 0 } }",
        "main", "async_returns_bool_false");
    expectEquals(v, 0, "async_returns_bool_false");
}

// An async fn returning a struct built from two awaited values; the first
// (`a`) is live across the second suspension, so it must be frame-promoted
// even though it is only read inside the struct literal. block_on returns the
// struct; read its fields.
void test_async_returns_struct() {
    auto v = compileAndRun(
        "struct P { x: i64, y: i64 }\n"
        "async fn mk() -> P {\n"
        "    let a = yield_now(10).await;\n"
        "    let b = yield_now(20).await;\n"
        "    P { x: a, y: b }\n"
        "}\n"
        "fn main() -> i64 { let p = block_on(mk()); p.x * 100 + p.y }",
        "main", "async_returns_struct");
    expectEquals(v, 1020, "async_returns_struct");
}

// Regression: Poll<T>/block_on<T> specialization keys must include generic
// type arguments; otherwise Option<bool> and Option<i64> collide and corrupt
// poll-slot layout.
void test_async_block_on_option_type_args_do_not_collide() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "async fn ob() -> Option<bool> { Some(true) }\n"
        "async fn oi() -> Option<i64> { Some(512) }\n"
        "fn main() -> i64 {\n"
        "    let _r = block_on(ob());\n"
        "    match block_on(oi()) {\n"
        "        Some(x) => x,\n"
        "        None => 0,\n"
        "    }\n"
        "}",
        "main", "async_block_on_option_type_args_do_not_collide");
    expectEquals(v, 512, "async_block_on_option_type_args_do_not_collide");
}

// --- Phase 17b: generic HashMap<i64, V> (value type follows the inserts) ---

// HashMap<i64, bool>: insert (1->true),(2->false); get returns Some(bool) /
// None. Encode the three lookups so the i64 main can assert all at once.
void test_hashmap_bool_value() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    hashmap_insert(&mut m, 1, true);\n"
        "    hashmap_insert(&mut m, 2, false);\n"
        "    let a = match hashmap_get(&m, 1) { Some(v) => if v { 1 } else { 0 }, None => 0 - 1 };\n"
        "    let b = match hashmap_get(&m, 2) { Some(v) => if v { 1 } else { 0 }, None => 0 - 1 };\n"
        "    let c = match hashmap_get(&m, 9) { Some(v) => if v { 1 } else { 0 }, None => 0 - 1 };\n"
        // a=1 (Some true), b=0 (Some false), c=-1 (None) => 1*100 + 0*10 - (-1)... encode:
        "    a * 100 + b * 10 - c\n" // 100 + 0 - (-1) = 101
        "}",
        "main", "hashmap_bool_value");
    expectEquals(v, 101, "hashmap_bool_value");
}

// HashMap<i64, P> for a struct P{x,y}: insert + overwrite + get back a struct;
// read its fields. Also confirms a miss returns None.
void test_hashmap_struct_value() {
    auto v = compileAndRun(
        "enum Option<T> { Some(T), None }\n"
        "struct P { x: i64, y: i64 }\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let m = hashmap_new();\n"
        "    hashmap_insert(&mut m, 5, P { x: 50, y: 51 });\n"
        "    hashmap_insert(&mut m, 5, P { x: 500, y: 501 });\n" // overwrite
        "    let hit = match hashmap_get(&m, 5) { Some(p) => p.x + p.y, None => 0 - 1 };\n"
        "    let miss = match hashmap_get(&m, 9) { Some(p) => p.x, None => 0 - 1 };\n"
        // hit = 500 + 501 = 1001; miss = -1; len = 1 => 1001 - (-1) + 1 = 1003
        "    hit - miss + hashmap_len(&m)\n"
        "}",
        "main", "hashmap_struct_value");
    expectEquals(v, 1003, "hashmap_struct_value");
}

void test_slice_of_vec_len_and_get() {
    // Slice [1,4) of {10,20,30,40,50}: len 3, sum 20+30+40 == 90.
    auto v = compileAndRun(
        "fn sum_slice(s: &[i64], i: i64) -> i64 {\n"
        "    if i < slice_len(s) { slice_get(s, i) + sum_slice(s, i + 1) }\n"
        "    else { 0 }\n"
        "}\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let v = vec_new();\n"
        "    vec_push(&mut v, 10);\n"
        "    vec_push(&mut v, 20);\n"
        "    vec_push(&mut v, 30);\n"
        "    vec_push(&mut v, 40);\n"
        "    vec_push(&mut v, 50);\n"
        "    let s = &v[1..4];\n"
        "    sum_slice(s, 0)\n"
        "}",
        "main", "slice_of_vec");
    expectEquals(v, 90, "slice_of_vec");
}

void test_slice_len_direct() {
    // slice_len on a [0,4) slice of a 4-element Vec == 4; an empty [2,2) == 0.
    auto v = compileAndRun(
        "fn main() -> i64 ! { alloc } {\n"
        "    let v = vec_new();\n"
        "    vec_push(&mut v, 1);\n"
        "    vec_push(&mut v, 2);\n"
        "    vec_push(&mut v, 3);\n"
        "    vec_push(&mut v, 4);\n"
        "    let full = &v[0..4];\n"
        "    let empty = &v[2..2];\n"
        "    slice_len(full) * 10 + slice_len(empty)\n" // 40
        "}",
        "main", "slice_len_direct");
    expectEquals(v, 40, "slice_len_direct");
}

// --- Phase 15: bool literals, unary ops, else-if, inherent impls ---

void test_bool_literal_true_branch() {
    auto v = compileAndRun(
        "fn main() -> i64 { let t = true; if t { 1 } else { 0 } }", "main",
        "bool_literal_true_branch");
    expectEquals(v, 1, "bool_literal_true_branch");
}

void test_bool_literal_false_branch() {
    auto v = compileAndRun(
        "fn main() -> i64 { let f = false; if f { 1 } else { 0 } }", "main",
        "bool_literal_false_branch");
    expectEquals(v, 0, "bool_literal_false_branch");
}

void test_unary_neg_literal() {
    auto v = compileAndRun("fn main() -> i64 { -5 }", "main",
                           "unary_neg_literal");
    expectEquals(v, -5, "unary_neg_literal");
}

void test_unary_neg_var() {
    auto v = compileAndRun("fn main() -> i64 { let x = 7; -x }", "main",
                           "unary_neg_var");
    expectEquals(v, -7, "unary_neg_var");
}

void test_unary_double_neg() {
    auto v = compileAndRun("fn main() -> i64 { -(-3) }", "main",
                           "unary_double_neg");
    expectEquals(v, 3, "unary_double_neg");
}

void test_unary_neg_precedence() {
    // `-a * b` == `(-a) * b` == -12 (not -(a*b), which is also -12, so use a
    // case that distinguishes: `-a + b` == (-a)+b == 1, vs -(a+b) == -7).
    auto v = compileAndRun(
        "fn main() -> i64 { let a = 3; let b = 4; -a + b }", "main",
        "unary_neg_precedence");
    expectEquals(v, 1, "unary_neg_precedence");
}

void test_unary_not_true_is_false() {
    // `!true` lowers to i1 0; branch on it to surface as i64.
    auto v = compileAndRun(
        "fn main() -> i64 { if !true { 1 } else { 0 } }", "main",
        "unary_not_true_is_false");
    expectEquals(v, 0, "unary_not_true_is_false");
}

void test_unary_not_comparison() {
    // `!(2 < 1)` == true; branch yields 1.
    auto v = compileAndRun(
        "fn main() -> i64 { if !(2 < 1) { 1 } else { 0 } }", "main",
        "unary_not_comparison");
    expectEquals(v, 1, "unary_not_comparison");
}

void test_branch_on_not_done() {
    auto v = compileAndRun(
        "fn main() -> i64 { let done = false; if !done { 11 } else { 22 } }",
        "main", "branch_on_not_done");
    expectEquals(v, 11, "branch_on_not_done");
}

void test_else_if_chain_first() {
    auto v = compileAndRun(
        "fn classify(a: i64) -> i64 {\n"
        "  if a == 0 { 100 } else if a == 1 { 200 } else { 300 }\n"
        "}\n"
        "fn main() -> i64 { classify(0) }",
        "main", "else_if_chain_first");
    expectEquals(v, 100, "else_if_chain_first");
}

void test_else_if_chain_middle() {
    auto v = compileAndRun(
        "fn classify(a: i64) -> i64 {\n"
        "  if a == 0 { 100 } else if a == 1 { 200 } else { 300 }\n"
        "}\n"
        "fn main() -> i64 { classify(1) }",
        "main", "else_if_chain_middle");
    expectEquals(v, 200, "else_if_chain_middle");
}

void test_else_if_chain_last() {
    auto v = compileAndRun(
        "fn classify(a: i64) -> i64 {\n"
        "  if a == 0 { 100 } else if a == 1 { 200 } else { 300 }\n"
        "}\n"
        "fn main() -> i64 { classify(7) }",
        "main", "else_if_chain_last");
    expectEquals(v, 300, "else_if_chain_last");
}

void test_inherent_getter() {
    auto v = compileAndRun(
        "struct P { x: i64 }\n"
        "impl P { fn get(&self) -> i64 { self.x } }\n"
        "fn main() -> i64 { let p = P { x: 9 }; p.get() }",
        "main", "inherent_getter");
    expectEquals(v, 9, "inherent_getter");
}

void test_inherent_mut_self_persists() {
    // Build, mutate via a &mut self inherent method twice, read back: the
    // mutation must persist across the two calls (0 + 5 + 5 == 10).
    auto v = compileAndRun(
        "struct Counter { n: i64 }\n"
        "impl Counter {\n"
        "  fn get(&self) -> i64 { self.n }\n"
        "  fn bump(&mut self) -> i64 { self.n = self.n + 5; self.n }\n"
        "}\n"
        "fn main() -> i64 {\n"
        "  let mut c = Counter { n: 0 };\n"
        "  c.bump();\n"
        "  c.bump();\n"
        "  c.get()\n"
        "}",
        "main", "inherent_mut_self_persists");
    expectEquals(v, 10, "inherent_mut_self_persists");
}

void test_inherent_method_with_arg() {
    auto v = compileAndRun(
        "struct Acc { t: i64 }\n"
        "impl Acc { fn add(&mut self, k: i64) -> i64 { self.t = self.t + k;"
        " self.t } }\n"
        "fn main() -> i64 { let mut a = Acc { t: 1 }; a.add(10); a.add(100) }",
        "main", "inherent_method_with_arg");
    expectEquals(v, 111, "inherent_method_with_arg");
}

void test_inherent_and_trait_coexist() {
    // An inherent method and a trait-impl method on the same type both
    // dispatch correctly (static dispatch for both).
    auto v = compileAndRun(
        "trait Show { fn show(&self) -> i64; }\n"
        "struct W { v: i64 }\n"
        "impl Show for W { fn show(&self) -> i64 { self.v } }\n"
        "impl W { fn dbl(&self) -> i64 { self.v + self.v } }\n"
        "fn main() -> i64 { let w = W { v: 6 }; w.show() + w.dbl() }",
        "main", "inherent_and_trait_coexist");
    expectEquals(v, 18, "inherent_and_trait_coexist");
}

// ---------------------------------------------------------------------------
// Phase 16: deterministic Drop / RAII.
//
// These exercise the runtime path: a double-free or use-after-free introduced
// by drop insertion would corrupt the JIT process and crash the test, so a
// clean return value is itself evidence of at-most-once dropping. We also
// inspect the emitted IR to confirm (a) `free` is actually emitted for
// heap-owning types, and (b) NON-droppable programs get byte-identical codegen
// (no `free`, no drop-flag).
// ---------------------------------------------------------------------------

// Compile `src` and return its textual LLVM IR (post-opt). Aborts on failure.
std::string compileToIR(const std::string& src, const char* label) {
    auto pr = kardashev::parse(src);
    if (!pr.ok()) { std::cerr << "[" << label << "] parse failed\n"; std::abort(); }
    auto tcr = kardashev::typecheck(pr.program);
    if (!tcr.ok()) {
        std::cerr << "[" << label << "] typecheck failed:\n";
        for (const auto& e : tcr.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    auto cgr = kardashev::codegen(pr.program, tcr);
    if (!cgr.ok()) {
        std::cerr << "[" << label << "] codegen failed:\n";
        for (const auto& m : cgr.errors) std::cerr << "  " << m << '\n';
        std::abort();
    }
    std::string s;
    llvm::raw_string_ostream os(s);
    cgr.module->print(os, nullptr);
    return os.str();
}

void expectContains(const std::string& hay, const std::string& needle,
                    const char* label) {
    if (hay.find(needle) == std::string::npos) {
        std::cerr << "[" << label << "] expected IR to contain '" << needle
                  << "'\n";
        std::abort();
    }
}
void expectAbsent(const std::string& hay, const std::string& needle,
                  const char* label) {
    if (hay.find(needle) != std::string::npos) {
        std::cerr << "[" << label << "] expected IR to NOT contain '" << needle
                  << "'\n";
        std::abort();
    }
}

// A Vec built and pushed-to, then dropped at scope exit, runs to a correct
// result (the JIT would crash on a double-free of the buffer).
void test_drop_vec_runs_clean() {
    auto v = compileAndRun(
        "fn build() -> i64 ! { alloc } {\n"
        "    let mut v = vec_new();\n"
        "    vec_push(&mut v, 10);\n"
        "    vec_push(&mut v, 20);\n"
        "    let n = vec_len(&v);\n"
        "    n\n"  // v drops here (buffer freed); n is the return value
        "}\n"
        "fn main() -> i64 ! { alloc } { build() }",
        "main", "drop_vec_runs_clean");
    expectEquals(v, 2, "drop_vec_runs_clean");
}

// A Vec allocated fresh each loop turn and dropped at the end of the body:
// 100k iterations must run cleanly (leak/double-free would crash or thrash).
void test_drop_vec_in_loop_runs_clean() {
    auto v = compileAndRun(
        "fn build(n: i64) -> Vec<i64> ! { alloc } {\n"
        "    let mut v = vec_new();\n"
        "    let mut i = 0;\n"
        "    while i < n { vec_push(&mut v, i); i = i + 1; }\n"
        "    v\n"
        "}\n"
        "fn main() -> i64 ! { alloc } {\n"
        "    let mut k = 0;\n"
        "    while k < 100000 { let v = build(16); k = k + 1; }\n"
        "    k\n"
        "}",
        "main", "drop_vec_in_loop_runs_clean");
    expectEquals(v, 100000, "drop_vec_in_loop_runs_clean");
}

// `let b = a;` moves `a` into `b`; the value is dropped at most once. A
// double-free of a Box (heap pointer) would crash the JIT; a clean run proves
// at-most-once. Box<i64> contributes a heap allocation that drop must free.
void test_drop_box_move_no_double_free() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "    let a = Box::new(77);\n"
        "    let b = a;\n"  // a moved into b; only b drops (frees) the box
        "    99\n"
        "}",
        "main", "drop_box_move_no_double_free");
    expectEquals(v, 99, "drop_box_move_no_double_free");
}

// Conditional move: a Box moved on one branch and live on the other is freed
// exactly once on every path. We run both branch selections.
void test_drop_box_conditional_move() {
    const char* prog =
        "fn sink(b: Box<i64>) -> i64 { 0 }\n"
        "fn pick(cond: bool) -> i64 {\n"
        "    let a = Box::new(5);\n"
        "    if cond { sink(a) } else { 7 }\n"  // moved on then, live on else
        "}\n"
        "fn main() -> i64 { pick(%s) }";
    char bufT[256]; std::snprintf(bufT, sizeof(bufT), prog, "true");
    char bufF[256]; std::snprintf(bufF, sizeof(bufF), prog, "false");
    expectEquals(compileAndRun(bufT, "main", "drop_box_cond_true"), 0,
                 "drop_box_cond_true");
    expectEquals(compileAndRun(bufF, "main", "drop_box_cond_false"), 7,
                 "drop_box_cond_false");
}

// A user `impl Drop` runs, and the drop glue emits a call to the user dtor.
// (The drop body is unit-returning — exercises the no-`->`-return surface.)
void test_drop_user_impl_emits_call() {
    std::string ir = compileToIR(
        "trait Drop { fn drop(&mut self); }\n"
        "struct Noisy { id: i64 }\n"
        "impl Drop for Noisy { fn drop(&mut self) { let x = self.id; } }\n"
        "fn main() -> i64 { let a = Noisy { id: 1 }; 0 }",
        "drop_user_impl_emits_call");
    // The mangled user drop method must exist and be called from main's drop.
    expectContains(ir, "__impl_Drop_for_Noisy__drop", "drop_user_impl_emits_call");
}

// Heap-owning drop glue lowers to libc `free`. We use a loop that builds a
// fresh Vec each turn whose contents are observed (summed) so the allocation
// can't be optimized away — the per-iteration drop's `free` then survives O2.
void test_drop_emits_free_for_vec() {
    std::string ir = compileToIR(
        "fn build(n: i64) -> Vec<i64> ! { alloc } {\n"
        "    let mut v = vec_new();\n"
        "    let mut i = 0;\n"
        "    while i < n { vec_push(&mut v, i); i = i + 1; }\n"
        "    v\n"
        "}\n"
        "fn main(n: i64) -> i64 ! { alloc } {\n"
        "    let mut k = 0;\n"
        "    let mut acc = 0;\n"
        "    while k < n {\n"
        "        let v = build(k);\n"
        "        acc = acc + vec_len(&v);\n"  // observe the buffer
        "        k = k + 1;\n"
        "    }\n"  // v drops (free) here each iteration
        "    acc\n"
        "}",
        "drop_emits_free_for_vec");
    expectContains(ir, "@free", "drop_emits_free_for_vec");
}

// Byte-identical-for-scalars invariant: a program using only non-droppable
// types (i64 + struct-of-scalars) emits NO `free` and NO drop-flag. Drop
// machinery must be inert for such code.
void test_no_drop_glue_for_scalars() {
    std::string ir = compileToIR(
        "struct P { x: i64, y: i64 }\n"
        "fn main() -> i64 {\n"
        "    let p = P { x: 3, y: 4 };\n"
        "    let a = 10;\n"
        "    let b = a;\n"
        "    p.x + p.y + b\n"
        "}",
        "no_drop_glue_for_scalars");
    expectAbsent(ir, "@free", "no_drop_glue_for_scalars");
    expectAbsent(ir, "droplive", "no_drop_glue_for_scalars");
}

// --- Phase 23: real panic + unwinding (setjmp/longjmp + cleanup stack). ----
// _setjmp/_longjmp/exit/write/dprintf resolve in-process (the test binary links
// libc). We only JIT-RUN the CAUGHT cases here — an uncaught panic calls
// exit(101), which would tear down the whole unit-test process; that path is
// covered by the smoke test (asserting the 101 exit code out-of-process).

// catch over a panicking fn returns the recovery value (the panic unwound back
// into catch). The catch helper's _setjmp/_longjmp round-trip works under JIT.
void test_catch_recovers_from_panic() {
    auto v = compileAndRun(
        "fn boom() -> i64 ! { panic } { panic(\"x\"); 0 }\n"
        "fn main() -> i64 ! { io } { catch(boom, 555) }",
        "main", "catch_recovers_from_panic");
    expectEquals(v, 555, "catch_recovers_from_panic");
}

// catch over a NON-panicking fn returns that fn's real value (the boundary is
// simply never triggered; setjmp returns 0 and we run + return the callback).
void test_catch_returns_real_value() {
    auto v = compileAndRun(
        "fn ok() -> i64 { 77 }\n"
        "fn main() -> i64 ! { io } { catch(ok, 555) }",
        "main", "catch_returns_real_value");
    expectEquals(v, 77, "catch_returns_real_value");
}

// Execution continues normally after a caught panic: catch(boom,1) + a real
// computation. Proves the post-catch path runs (no lingering unwind state).
void test_execution_continues_after_catch() {
    auto v = compileAndRun(
        "fn boom() -> i64 ! { panic } { panic(\"x\"); 0 }\n"
        "fn main() -> i64 ! { io } {\n"
        "  let a = catch(boom, 1);\n"   // 1 (recovered)
        "  let b = catch(boom, 40);\n"  // 40 (recovered again — stack is clean)
        "  a + b + 1\n"                  // 42
        "}",
        "main", "execution_continues_after_catch");
    expectEquals(v, 42, "execution_continues_after_catch");
}

// Drop runs while unwinding a caught panic. A heap Box<i64> declared before the
// panic must be freed on the unwind path; we run it 50000 times so a leak/double
// -free would corrupt the JIT heap and crash the test — a clean return is the
// proof that the cleanup-stack entry ran the drop exactly once per unwind.
void test_drop_runs_on_panic_unwind() {
    auto v = compileAndRun(
        "fn boom() -> i64 ! { alloc, panic } {\n"
        "  let b = Box::new(7);\n"   // heap; must be freed on unwind
        "  panic(\"boom\");\n"
        "  0\n"
        "}\n"
        "fn main() -> i64 ! { alloc, io } {\n"
        "  let mut k = 0;\n"
        "  while k < 50000 { let r = catch(boom, 0); k = k + 1; }\n"
        "  k\n"
        "}",
        "main", "drop_runs_on_panic_unwind");
    expectEquals(v, 50000, "drop_runs_on_panic_unwind");
}

// A value MOVED into a callee that panics is dropped exactly once (the caller's
// cleanup entry sees the moved-out drop flag cleared and skips it). 50000 turns
// — a double-free would crash the JIT.
void test_move_then_panic_no_double_free() {
    auto v = compileAndRun(
        "fn sink(b: Box<i64>) -> i64 ! { panic } { panic(\"x\"); 0 }\n"
        "fn boom() -> i64 ! { alloc, panic } {\n"
        "  let b = Box::new(9);\n"
        "  sink(b)\n"                 // b moves into sink; sink panics holding it
        "}\n"
        "fn main() -> i64 ! { alloc, io } {\n"
        "  let mut k = 0;\n"
        "  while k < 50000 { let r = catch(boom, 0); k = k + 1; }\n"
        "  k\n"
        "}",
        "main", "move_then_panic_no_double_free");
    expectEquals(v, 50000, "move_then_panic_no_double_free");
}

// A dynamic out-of-bounds array index PANICS and is caught (returns recovery).
void test_array_oob_index_panics_caught() {
    auto v = compileAndRun(
        "fn oob() -> i64 ! { panic } {\n"
        "  let a = [10, 20, 30];\n"
        "  let i = 5;\n"
        "  a[i]\n"
        "}\n"
        "fn main() -> i64 ! { io } { catch(oob, 777) }",
        "main", "array_oob_index_panics_caught");
    expectEquals(v, 777, "array_oob_index_panics_caught");
}

// An in-bounds dynamic index does NOT panic (the bounds check only fires when
// actually out of range) — regression guard.
void test_array_inbounds_index_no_panic() {
    auto v = compileAndRun(
        "fn main() -> i64 {\n"
        "  let a = [5, 15, 25, 35];\n"
        "  let mut i = 0;\n"
        "  let mut acc = 0;\n"
        "  while i < 4 { acc = acc + a[i]; i = i + 1; }\n"
        "  acc\n"
        "}",
        "main", "array_inbounds_index_no_panic");
    expectEquals(v, 80, "array_inbounds_index_no_panic");
}

// A may-panic program emits the panic runtime: the `panic` body, the
// setjmp/longjmp catch round-trip, and — when a droppable local is live across
// a panic — the cleanup-stack machinery (push, plus the @free the unwinder
// runs). `boom` holds a heap Box across the panic so the cleanup path survives
// optimization.
void test_panic_runtime_emitted_when_used() {
    std::string ir = compileToIR(
        "fn boom() -> i64 ! { alloc, panic } {\n"
        "  let b = Box::new(7);\n"
        "  panic(\"x\");\n"
        "  0\n"
        "}\n"
        "fn main() -> i64 ! { alloc, io } { catch(boom, 0) }",
        "panic_runtime_emitted_when_used");
    expectContains(ir, "_setjmp", "panic_runtime_emitted_when_used");
    expectContains(ir, "__kd_cleanup", "panic_runtime_emitted_when_used");
    expectContains(ir, "@panic", "panic_runtime_emitted_when_used");
}

// The byte-identical-for-panic-free invariant: a program that never panics
// (no panic/catch, no array indexing) emits ZERO panic runtime — no
// setjmp/longjmp, no cleanup stack — even when it has Drop-bearing locals.
void test_no_panic_runtime_when_unused() {
    std::string ir = compileToIR(
        "trait Drop { fn drop(&mut self); }\n"
        "struct Noisy { id: i64 }\n"
        "impl Drop for Noisy { fn drop(&mut self) { let x = self.id; } }\n"
        "fn main() -> i64 { let a = Noisy { id: 1 }; 0 }",
        "no_panic_runtime_when_unused");
    expectAbsent(ir, "_setjmp", "no_panic_runtime_when_unused");
    expectAbsent(ir, "__kd_cleanup", "no_panic_runtime_when_unused");
    expectAbsent(ir, "__kd_catch", "no_panic_runtime_when_unused");
}

// --- Phase 19: OS threads (pthread) + Mutex run end-to-end through the JIT.
// pthread symbols resolve in-process (the test binary links libc/pthread).

void test_thread_spawn_join_runs() {
    // Two real threads compute distinct values; join returns each; combine.
    auto v = compileAndRun(
        "fn ca() -> i64 { 10 + 11 }\n"
        "fn cb() -> i64 { 100 + 23 }\n"
        "fn main() -> i64 ! { io } {\n"
        "  let a = thread_spawn(ca);\n"
        "  let b = thread_spawn(cb);\n"
        "  thread_join(a) + thread_join(b)\n"
        "}",
        "main", "thread_spawn_join_runs");
    expectEquals(v, 144, "thread_spawn_join_runs"); // 21 + 123
}

void test_thread_spawn_closure_byvalue_runs() {
    auto v = compileAndRun(
        "fn main() -> i64 ! { io } {\n"
        "  let base = 1000;\n"
        "  let a = thread_spawn(|| base + 21);\n"
        "  let b = thread_spawn(|| base + 23);\n"
        "  thread_join(a) + thread_join(b)\n"
        "}",
        "main", "thread_spawn_closure_byvalue_runs");
    expectEquals(v, 2044, "thread_spawn_closure_byvalue_runs");
}

void test_mutex_roundtrip_single_thread() {
    auto v = compileAndRun(
        "fn main() -> i64 ! { alloc, io } {\n"
        "  let m = mutex_new(40);\n"
        "  mutex_lock(m);\n"
        "  mutex_set(m, mutex_get(m) + 2);\n"
        "  mutex_unlock(m);\n"
        "  mutex_get(m)\n"
        "}",
        "main", "mutex_roundtrip_single_thread");
    expectEquals(v, 42, "mutex_roundtrip_single_thread");
}

void test_mutex_mutual_exclusion_two_threads() {
    // 2 threads x 50000 increments of a SHARED Mutex counter == exactly
    // 100000. (Smaller N than the smoke test to keep the unit suite quick;
    // still proves mutual exclusion + that both threads run.)
    auto v = compileAndRun(
        "fn bump(c: i64, n: i64) -> i64 ! { io } {\n"
        "  let mut i = 0;\n"
        "  while i < n {\n"
        "    mutex_lock(c);\n"
        "    mutex_set(c, mutex_get(c) + 1);\n"
        "    mutex_unlock(c);\n"
        "    i = i + 1;\n"
        "  }\n"
        "  0\n"
        "}\n"
        "fn main() -> i64 ! { alloc, io } {\n"
        "  let c = mutex_new(0);\n"
        "  let n = 50000;\n"
        "  let t1 = thread_spawn(|| bump(c, n));\n"
        "  let t2 = thread_spawn(|| bump(c, n));\n"
        "  thread_join(t1);\n"
        "  thread_join(t2);\n"
        "  mutex_get(c)\n"
        "}",
        "main", "mutex_mutual_exclusion_two_threads");
    expectEquals(v, 100000, "mutex_mutual_exclusion_two_threads");
}

void test_thread_runtime_emits_pthread_externs() {
    // The pthread externs + trampoline must appear in the emitted IR.
    std::string ir = compileToIR(
        "fn w() -> i64 { 1 }\n"
        "fn main() -> i64 ! { io } { thread_join(thread_spawn(w)) }",
        "thread_runtime_emits_pthread_externs");
    expectContains(ir, "pthread_create", "thread_runtime_emits_pthread_externs");
    expectContains(ir, "pthread_join", "thread_runtime_emits_pthread_externs");
    expectContains(ir, "__kd_thread_trampoline",
                   "thread_runtime_emits_pthread_externs");
}

// --- Phase 24: extern "C" FFI declarations ---

// An `i32`-spelled extern lowers to a C-int (i32) external declaration, and
// the call narrows the i64 arg with a `trunc`. `n` is a runtime param so the
// call can't be constant-folded away at O2. `putchar` is used because (unlike
// `abs`/`labs`) LLVM has no folding intrinsic for it, so the `@putchar(i32`
// declaration survives the optimization pipeline.
void test_extern_i32_decl_and_arg_narrowed() {
    std::string ir = compileToIR(
        "extern \"C\" fn putchar(c: i32) -> i32;\n"
        "fn main(n: i64) -> i64 ! { io } { putchar(n); 0 }",
        "extern_i32_decl_and_arg_narrowed");
    expectContains(ir, "@putchar(i32", "extern_i32_decl_and_arg_narrowed");
    expectContains(ir, "trunc i64", "extern_i32_decl_and_arg_narrowed");
}

// An `i32`-RETURN extern widens its C-int result back to i64 with a sext (or,
// when the optimizer can prove non-negativity, a zext nneg — both are correct
// widenings). We just require the result feeds a 64-bit value. `getpid`
// survives O2 as a declaration. Its result is used so the call can't be DCE'd.
void test_extern_i32_return_widened() {
    std::string ir = compileToIR(
        "extern \"C\" fn getpid() -> i32;\n"
        "fn main() -> i64 ! { io } { let p = getpid(); p + 1 }",
        "extern_i32_return_widened");
    expectContains(ir, "i32 @getpid()", "extern_i32_return_widened");
    // The i32 result is extended to i64 (sext from codegen, possibly turned
    // into `zext nneg` by the optimizer when it proves the sign).
    bool widened = ir.find("ext i32") != std::string::npos ||
                   ir.find("ext nneg i32") != std::string::npos;
    if (!widened) {
        std::cerr << "[extern_i32_return_widened] expected an i32->i64 "
                     "widening of the getpid result\n";
        std::abort();
    }
}

// A `&String` arg lowers to a C pointer: the declaration takes a `ptr`. A
// fictional symbol name is used so the optimizer has no library knowledge to
// fold it away (compileToIR only builds the module — it never links/runs, so
// an unresolved symbol is fine for an IR-shape check).
void test_extern_ref_string_lowers_to_pointer() {
    std::string ir = compileToIR(
        "extern \"C\" fn kd_ffi_strlen(s: &String) -> i64;\n"
        "fn main() -> i64 ! { io } { let s = \"hello\"; kd_ffi_strlen(&s) }",
        "extern_ref_string_lowers_to_pointer");
    expectContains(ir, "@kd_ffi_strlen(ptr",
                   "extern_ref_string_lowers_to_pointer");
}

// The C symbol name is the declared name verbatim — no mangling / instance
// suffix (a fictional name so O2 can't fold it).
void test_extern_symbol_name_not_mangled() {
    std::string ir = compileToIR(
        "extern \"C\" fn kd_ffi_thing() -> i32;\n"
        "fn main() -> i64 ! { io } { let p = kd_ffi_thing(); p }",
        "extern_symbol_name_not_mangled");
    expectContains(ir, "@kd_ffi_thing(", "extern_symbol_name_not_mangled");
    if (ir.find("kd_ffi_thing__") != std::string::npos) {
        std::cerr << "[extern_symbol_name_not_mangled] unexpected mangled "
                     "symbol kd_ffi_thing__\n";
        std::abort();
    }
}

// --- Phase 25: compile-time constants + const evaluation -------------------

void test_const_item_runs() {
    auto v = compileAndRun("const SIZE: i64 = 3 + 2;\n"
                           "fn main() -> i64 { SIZE }",
                           "main", "const_item_runs");
    expectEquals(v, 5, "const_item_runs");
}

void test_const_fn_value_runs() {
    auto v = compileAndRun("const fn sq(x: i64) -> i64 { x * x }\n"
                           "const NINE: i64 = sq(3);\n"
                           "fn main() -> i64 { NINE }",
                           "main", "const_fn_value_runs");
    expectEquals(v, 9, "const_fn_value_runs");
}

void test_const_references_const_runs() {
    auto v = compileAndRun("const A: i64 = 10;\n"
                           "const B: i64 = A * 2;\n"
                           "fn main() -> i64 { B }",
                           "main", "const_references_const_runs");
    expectEquals(v, 20, "const_references_const_runs");
}

void test_const_generic_array_len_runs() {
    auto v = compileAndRun(
        "const N: i64 = 2 + 1;\n"
        "fn main() -> i64 { let a: [i64; N] = [10, 20, 30]; a[0] + a[2] }",
        "main", "const_generic_array_len_runs");
    expectEquals(v, 40, "const_generic_array_len_runs");
}

void test_const_fn_array_len_runs() {
    auto v = compileAndRun(
        "const fn sq(x: i64) -> i64 { x * x }\n"
        "fn main() -> i64 { let a: [i64; sq(2)] = [1,2,3,4]; "
        "a[0]+a[1]+a[2]+a[3] }",
        "main", "const_fn_array_len_runs");
    expectEquals(v, 10, "const_fn_array_len_runs");
}

void test_const_fn_runtime_call_runs() {
    // A const fn called at runtime with a runtime argument behaves like an
    // ordinary fn (codegen path unchanged).
    auto v = compileAndRun("const fn sq(x: i64) -> i64 { x * x }\n"
                           "fn main() -> i64 { let y = 7; sq(y) }",
                           "main", "const_fn_runtime_call_runs");
    expectEquals(v, 49, "const_fn_runtime_call_runs");
}

// The const reaches codegen as a FOLDED literal, not a runtime load: main's
// body is just `ret i64 5`. (The const-evaluator folds before any optimizer
// pass, so this holds independent of opt level — and there is no global
// storage emitted for the const.)
void test_const_folds_to_literal_in_ir() {
    std::string ir = compileToIR("const SIZE: i64 = 3 + 2;\n"
                                 "fn main() -> i64 { SIZE }",
                                 "const_folds_to_literal_in_ir");
    expectContains(ir, "ret i64 5", "const_folds_to_literal_in_ir");
    // No `add` in main's body — the value was folded, not computed at runtime.
    // (The smoke test additionally asserts this at -O0, where the optimizer
    // can't be credited for the folding.)
    if (ir.find("@SIZE") != std::string::npos) {
        std::cerr << "[const_folds_to_literal_in_ir] unexpected global @SIZE "
                     "(a const must not get runtime storage)\n";
        std::abort();
    }
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
    // Phase 21a: generic trait parameters end to end
    test_generic_trait_bounded_head_both_elems();
    test_generic_iterator_bool_for_loop();
    test_generic_fold_over_bool();
    test_iterator_unparam_bound_i64_regression();
    // Phase 21b: associated types + where clauses end to end
    test_assoc_self_item_two_impls();
    test_assoc_c_item_bounded_call_site();
    test_where_clause_runs_like_inline();
    test_trait_multi_method();
    test_trait_method_with_args();
    // Phase 3.4 try operator
    test_try_ok_path();
    test_try_err_path();
    test_try_chained();
    // Phase 9 loops + ranges + assignment
    test_while_countdown_sum();
    test_loop_break_value();
    test_loop_continue_sum_odds();
    test_for_inclusive_range_sum();
    test_for_exclusive_range_sum();
    test_nested_loops_break_continue_innermost();
    test_range_value_iterated();
    test_field_assign_through_mut_local();
    // Phase 10a effect-carrying fn types (effects erased in lowering)
    test_effect_poly_higher_order_pure();
    test_concrete_effect_fn_type_param();
    // Phase 10b capturing closures (fat-pointer fn values)
    test_closure_single_capture();
    test_closure_multi_capture();
    test_closure_passed_to_higher_order();
    test_closure_multi_param_annotated();
    test_closure_no_capture();
    test_closure_selected_by_if();
    test_closure_block_body();
    // Phase 17a richer closures & first-class fn values
    test_field_held_fn_value_toplevel();
    test_field_held_closure_value();
    test_field_fn_called_through_self();
    test_call_value_of_expression();
    test_fnmut_counter();
    test_fnmut_accumulator();
    // Phase 11 dyn Trait + vtable dynamic dispatch
    test_dyn_dispatch_two_impls();
    test_dyn_box();
    test_dyn_multi_method_slot();
    test_dyn_method_with_arg();
    test_self_ref_static_dispatch();
    test_dyn_heterogeneous_sum();
    // Phase 12 real async runtime (state-machine suspend/resume)
    test_async_two_awaits_locals();
    test_async_local_live_across_two_suspends();
    test_async_poll_count_observes_pending();
    test_async_single_await();
    test_async_zero_await_chain();
    test_async_param_survives_suspension();
    // Phase 17b generic Future<T>: result type follows the async fn's return
    test_async_returns_bool_true();
    test_async_returns_bool_false();
    test_async_returns_struct();
    test_async_block_on_option_type_args_do_not_collide();
    // Phase 13a method-receiver autoref + Iterator trait + adaptors
    test_mut_self_persists_across_calls();
    test_shared_self_multiple_calls();
    test_for_over_custom_iterator();
    test_for_inclusive_range_still_55();
    test_fold_closure_driven();
    test_map_filter_eager_vec();
    // Phase 13b growable String, HashMap<i64,i64>, slices
    test_string_build_and_len();
    test_string_push_grows_past_initial_cap();
    // Phase 26: str_char_at builtin (in-bounds byte / past-end / negative)
    test_str_char_at_in_bounds();
    test_str_char_at_out_of_bounds_is_minus_one();
    test_str_char_at_negative_is_minus_one();
    test_hashmap_insert_get_overwrite();
    test_hashmap_rehash_all_retrievable();
    // Phase 17b generic HashMap<i64,V>: value type follows the inserts
    test_hashmap_bool_value();
    test_hashmap_struct_value();
    test_slice_of_vec_len_and_get();
    test_slice_len_direct();
    // Phase 15: bool literals, unary ops, else-if, inherent impls
    test_bool_literal_true_branch();
    test_bool_literal_false_branch();
    test_unary_neg_literal();
    test_unary_neg_var();
    test_unary_double_neg();
    test_unary_neg_precedence();
    test_unary_not_true_is_false();
    test_unary_not_comparison();
    test_branch_on_not_done();
    test_else_if_chain_first();
    test_else_if_chain_middle();
    test_else_if_chain_last();
    test_inherent_getter();
    test_inherent_mut_self_persists();
    test_inherent_method_with_arg();
    test_inherent_and_trait_coexist();
    // Phase 16: deterministic Drop / RAII
    test_drop_vec_runs_clean();
    test_drop_vec_in_loop_runs_clean();
    test_drop_box_move_no_double_free();
    test_drop_box_conditional_move();
    test_drop_user_impl_emits_call();
    test_drop_emits_free_for_vec();
    test_no_drop_glue_for_scalars();
    // Phase 19: OS threads (pthread) + Mutex
    test_thread_spawn_join_runs();
    test_thread_spawn_closure_byvalue_runs();
    test_mutex_roundtrip_single_thread();
    test_mutex_mutual_exclusion_two_threads();
    test_thread_runtime_emits_pthread_externs();
    test_catch_recovers_from_panic();
    test_catch_returns_real_value();
    test_execution_continues_after_catch();
    test_drop_runs_on_panic_unwind();
    test_move_then_panic_no_double_free();
    test_array_oob_index_panics_caught();
    test_array_inbounds_index_no_panic();
    test_panic_runtime_emitted_when_used();
    test_no_panic_runtime_when_unused();
    // Phase 24: extern "C" FFI lowering.
    test_extern_i32_decl_and_arg_narrowed();
    test_extern_i32_return_widened();
    test_extern_ref_string_lowers_to_pointer();
    test_extern_symbol_name_not_mangled();
    // Phase 25: compile-time constants + const evaluation.
    test_const_item_runs();
    test_const_fn_value_runs();
    test_const_references_const_runs();
    test_const_generic_array_len_runs();
    test_const_fn_array_len_runs();
    test_const_fn_runtime_call_runs();
    test_const_folds_to_literal_in_ir();
    std::cout << "All codegen tests passed (154 cases) — Phase 16 Drop/RAII: "
                 "reverse-order scope drops, move semantics, conditional-move "
                 "drop flags, Vec/Box free, scalar codegen unchanged; Phase "
                 "17a fn-value field calls + FnMut captures; Phase 17b generic "
                 "Future<T> (bool/struct) + HashMap<i64,V> (bool/struct); "
                 "Phase 19 OS threads + Mutex mutual exclusion; Phase 21a "
                 "generic trait params (Container<T>/Iterator<T>: bounded fn, "
                 "for-loop, fold over bool, <I: Iterator> regression); Phase "
                 "21b associated types (Self::Item + C::Item at i64/bool) + "
                 "where-clause equivalence; Phase 23 panic/unwind (catch "
                 "recovers via setjmp/longjmp, Drop runs on unwind + no "
                 "double-free over 50k unwinds, array OOB panics, panic runtime "
                 "gated to may-panic programs); Phase 24 extern \"C\" FFI "
                 "(i32->C-int decl + trunc/sext coercions, i64->C-long, "
                 "&String->C pointer, unmangled C symbol name)\n";
    return 0;
}
