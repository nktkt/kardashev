#!/usr/bin/env bash
# Phase 15 smoke test: everyday surface features — boolean literals
# (`true`/`false`), prefix unary operators (`-x`, `!x`), `else if` chains,
# and inherent impls (`impl Type { ... }`, including a `&mut self` mutator
# whose effect persists). Each program computes a known value so we can
# assert on JIT stdout AND AOT exit code, mirroring smoke_test_loops.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Run a program through both JIT (stdout) and AOT (exit code) and assert
# both yield the expected value. `main` returns the value; JIT prints it,
# AOT uses it as the process exit code.
run_case() {
    local name="$1" src="$2" want="$3"
    printf '%s' "$src" > "$TMP/$name.kd"

    local jit
    jit=$("$KARDC" "$TMP/$name.kd")
    if [[ "$jit" != "$want" ]]; then
        echo "FAIL [$name]: JIT printed '$jit', expected '$want'"
        exit 1
    fi

    "$KARDC" -o "$TMP/$name" "$TMP/$name.kd"
    set +e
    "$TMP/$name"
    local code=$?
    set -e
    if [[ "$code" != "$want" ]]; then
        echo "FAIL [$name]: AOT exit code $code, expected $want"
        exit 1
    fi
    echo "PASS [$name]: JIT + AOT == $want"
}

# 1. boolean literals select the if-branch: `true` -> 1, `false` -> 0.
run_case bool_true '
fn main() -> i64 {
    let t = true;
    if t { 1 } else { 0 }
}
' 1

run_case bool_false '
fn main() -> i64 {
    let f = false;
    if f { 1 } else { 0 }
}
' 0

# 2. unary minus: a literal, a variable, and double negation `-(-3)` == 3.
run_case neg_literal '
fn main() -> i64 { let x = -5; 0 - x }
' 5

run_case neg_var '
fn main() -> i64 { let x = 7; let y = -x; 0 - y }
' 7

run_case double_neg '
fn main() -> i64 { -(-3) }
' 3

# 3. logical not: `!(2 < 1)` is true -> branch yields 1; branch on `!done`.
run_case not_comparison '
fn main() -> i64 { if !(2 < 1) { 1 } else { 0 } }
' 1

run_case branch_on_not_done '
fn main() -> i64 {
    let done = false;
    if !done { 7 } else { 99 }
}
' 7

# A `bool`-returning main: `!true` is false (exit/print 0). Exercises the
# i1-return adaptation in both the JIT entry call and the AOT C-main wrapper.
run_case bool_main_not_true '
fn main() -> bool { !true }
' 0

# 4. a 3-way else-if chain returns the right branch per input.
run_case else_if_first '
fn classify(a: i64) -> i64 {
    if a == 0 { 1 } else if a == 1 { 2 } else { 3 }
}
fn main() -> i64 { classify(0) }
' 1

run_case else_if_middle '
fn classify(a: i64) -> i64 {
    if a == 0 { 1 } else if a == 1 { 2 } else { 3 }
}
fn main() -> i64 { classify(1) }
' 2

run_case else_if_last '
fn classify(a: i64) -> i64 {
    if a == 0 { 1 } else if a == 1 { 2 } else { 3 }
}
fn main() -> i64 { classify(9) }
' 3

# 5. inherent impl with a &self getter and a &mut self mutator: build,
#    mutate twice via the inherent method, read back. The mutation must
#    persist across calls (0 + 5 + 5 == 10).
run_case inherent_mut_self '
struct Counter { n: i64 }
impl Counter {
    fn get(&self) -> i64 { self.n }
    fn bump(&mut self) -> i64 { self.n = self.n + 5; self.n }
}
fn main() -> i64 {
    let mut c = Counter { n: 0 };
    c.bump();
    c.bump();
    c.get()
}
' 10

# An inherent method and a trait-impl method on the same type coexist.
run_case inherent_and_trait '
trait Show { fn show(&self) -> i64; }
struct W { v: i64 }
impl Show for W { fn show(&self) -> i64 { self.v } }
impl W { fn dbl(&self) -> i64 { self.v + self.v } }
fn main() -> i64 {
    let w = W { v: 6 };
    w.show() + w.dbl()
}
' 18

# 6. pub on type kinds parses + compiles (visibility is permissive under the
#    flat-merge model, so the program still runs).
run_case pub_types '
pub struct S { v: i64 }
pub enum E { A, B(i64) }
pub trait T { fn m(&self) -> i64; }
fn main() -> i64 { let s = S { v: 4 }; s.v }
' 4

echo "PASS: bool literals / unary ops / else-if / inherent impls work in JIT + AOT"
