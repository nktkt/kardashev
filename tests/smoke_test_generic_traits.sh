#!/usr/bin/env bash
# Phase 21a smoke test: generic trait *parameters* — `trait Foo<T> { ... }`,
# `impl Foo<Concrete> for Type`, and bounds `<C: Foo<T>>`. End to end through
# JIT and AOT.
#
#   1. A user generic trait `Container<T>` with impls at TWO element types
#      (i64 + bool); `.first()` on each yields the right-typed value.
#   2. A generic-trait-bounded fn `head<T, C: Container<T>>(c: C) -> T` used at
#      both element types (the element type resolves through the bound).
#   3. The migrated prelude `Iterator<T>`: a custom `impl Iterator<bool> for
#      Count`, iterated with `for x in c` — the `for` desugar derives the bool
#      element type from `next()`'s `Option<bool>`.
#   4. A generic adaptor `fold<T, I: Iterator<T>>` folding BOOLEANS, proving
#      the adaptors are generic over the element type now.
#   5. Regression: the pre-21a `<I: Iterator>` spelling (omitted element arg)
#      still folds an i64 iterator (the `Iterator<i64>` migration kept i64
#      iteration working).
#   6. Negative: `dyn Iterator` (a generic trait used as a trait object) is
#      rejected with a clear "not supported" diagnostic.
#
# The prelude supplies the generic `trait Iterator<T>` + `impl Iterator<i64>
# for Range`, so range programs rely on it implicitly (cases that define their
# own `Iterator` suppress the prelude's).
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

# Run a program through both JIT (stdout, last line) and AOT (exit code) and
# assert both yield the expected i64. `main` returns the value.
run_case() {
    local name="$1" src="$2" want="$3"
    printf '%s' "$src" > "$TMP/$name.kd"

    local jit
    jit=$("$KARDC" "$TMP/$name.kd" | tail -n 1)
    if [[ "$jit" != "$want" ]]; then
        echo "FAIL [$name]: JIT printed '$jit', expected '$want'"
        exit 1
    fi

    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd"
    set +e
    "$TMP/$name" > /dev/null
    local code=$?
    set -e
    if [[ "$code" != "$want" ]]; then
        echo "FAIL [$name]: AOT exit code $code, expected $want"
        exit 1
    fi
    echo "PASS [$name]: JIT + AOT == $want"
}

# 1 + 2. A user generic trait with impls at two element types, plus a
# generic-trait-bounded fn used at both. The bool path gates the i64 path, so a
# correct element-type resolution returns 42 (direct `.first()` AND `head`).
run_case container_two_elems '
trait Container<T> { fn first(&self) -> T; }
struct IntBox { v: i64 }
struct BoolBox { b: bool }
impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }
impl Container<bool> for BoolBox { fn first(&self) -> bool { self.b } }
fn head<T, C: Container<T>>(c: C) -> T { c.first() }
fn main() -> i64 ! { alloc } {
    let ib = IntBox { v: 42 };
    let bb = BoolBox { b: true };
    let direct = ib.first();
    let viaHead = head(ib);
    let bd = bb.first();
    let bh = head(bb);
    if bd {
        if bh { if direct == viaHead { viaHead } else { 0 } } else { 0 }
    } else { 0 }
}' 42

# 3. The migrated Iterator<T>: a custom `impl Iterator<bool> for Count`,
# iterated with `for`. Count{n:5} yields elements for n=4,3,2,1,0; isEven is
# true for 4,2,0 -> 3 trues. Defines its own `Iterator` (suppresses prelude).
run_case for_iterator_bool '
enum Option<T> { Some(T), None }
trait Iterator<T> { fn next(&mut self) -> Option<T>; }
struct Count { n: i64 }
impl Iterator<bool> for Count {
    fn next(&mut self) -> Option<bool> {
        if self.n <= 0 {
            None
        } else {
            self.n = self.n - 1;
            let isEven = self.n - (self.n / 2) * 2 == 0;
            Some(isEven)
        }
    }
}
fn main() -> i64 ! { alloc } {
    let c = Count { n: 5 };
    let mut trues = 0;
    for b in c {
        if b { trues = trues + 1; } else { trues = trues; }
    }
    trues
}' 3

# 4. A generic adaptor folding BOOLEANS: fold<T, I: Iterator<T>> with a
# closure-free fn(i64, bool) -> i64. 3 evens among Count{n:5} -> 3.
run_case fold_over_bool '
enum Option<T> { Some(T), None }
trait Iterator<T> { fn next(&mut self) -> Option<T>; }
struct Count { n: i64 }
impl Iterator<bool> for Count {
    fn next(&mut self) -> Option<bool> {
        if self.n <= 0 {
            None
        } else {
            self.n = self.n - 1;
            let isEven = self.n - (self.n / 2) * 2 == 0;
            Some(isEven)
        }
    }
}
fn fold<T, I: Iterator<T>>(it: I, init: i64, f: fn(i64, T) -> i64) -> i64 ! { alloc } {
    let mut iter = it;
    let mut acc = init;
    loop {
        match iter.next() {
            Some(x) => { acc = f(acc, x); },
            None => { break; },
        }
    }
    acc
}
fn count_true(acc: i64, b: bool) -> i64 { if b { acc + 1 } else { acc } }
fn main() -> i64 ! { alloc } {
    let c = Count { n: 5 };
    fold(c, 0, count_true)
}' 3

# 5. Regression: a generic adaptor folding over an i64 element via the
# parameterized bound, plus the prelude Iterator<i64> for Range over a literal
# range fast path. fold of Countdown{n:4}: 4+3+2+1 == 10, plus 1..=5 == 15 via
# the range -> 25. Proves i64 iteration survives the migration.
run_case fold_i64_and_range '
fn fold<T, I: Iterator<T>>(it: I, init: i64, f: fn(i64, T) -> i64) -> i64 ! { alloc } {
    let mut iter = it;
    let mut acc = init;
    loop {
        match iter.next() {
            Some(x) => { acc = f(acc, x); },
            None => { break; },
        }
    }
    acc
}
struct Countdown { n: i64 }
impl Iterator<i64> for Countdown {
    fn next(&mut self) -> Option<i64> {
        if self.n <= 0 { None } else { self.n = self.n - 1; Some(self.n + 1) }
    }
}
fn add(a: i64, b: i64) -> i64 { a + b }
fn main() -> i64 ! { alloc } {
    let c = Countdown { n: 4 };
    let cd = fold(c, 0, add);
    let mut rs = 0;
    for x in 1..=5 { rs = rs + x; }
    cd + rs
}' 25

# 5b. Regression: the OLD `<I: Iterator>` spelling (element arg OMITTED) still
# compiles + runs over i64 after migrating Iterator to Iterator<T>. Sum the
# prelude Range via fold (1..=10 -> 55).
run_case unparam_iterator_bound '
fn fold<I: Iterator>(it: I, init: i64, f: fn(i64, i64) -> i64) -> i64 ! { alloc } {
    let mut iter = it;
    let mut acc = init;
    loop {
        match iter.next() {
            Some(x) => { acc = f(acc, x); },
            None => { break; },
        }
    }
    acc
}
fn add(a: i64, b: i64) -> i64 { a + b }
fn main() -> i64 ! { alloc } {
    fold(1..=10, 0, add)
}' 55

# 6. Phase 49: a generic trait CAN now be used as a trait object when its type
# args are supplied (`dyn Iterator<i64>`); see smoke_test_phase49 for dispatch.
# The remaining negative is an arity MISMATCH — a bare `dyn Iterator` (no
# `<...>`) for a 1-param trait is rejected with a "trait type arg" diagnostic.
cat > "$TMP/dyn_generic.kd" <<'EOF'
trait Iterator<T> { fn next(&mut self) -> T; }
fn use_it(it: &dyn Iterator) -> i64 { 0 }
fn main() -> i64 { 0 }
EOF
set +e
DYN_OUT=$("$KARDC" "$TMP/dyn_generic.kd" 2>&1)
DYN_RC=$?
set -e
if [[ "$DYN_RC" -eq 0 ]]; then
    echo "FAIL [dyn_generic_arity]: bare `dyn Iterator` (missing <T>) should be rejected"
    exit 1
fi
if ! grep -qi 'trait type arg' <<< "$DYN_OUT"; then
    echo "FAIL [dyn_generic_arity]: expected a 'trait type arg' arity diagnostic, got:"
    echo "$DYN_OUT"
    exit 1
fi
echo "PASS [dyn_generic_arity]: bare dyn of a 1-param trait is rejected (arity)"

echo "PASS: generic trait params work in JIT + AOT (Container<T> at i64+bool; head<T, C: Container<T>>; Iterator<bool> via for; fold over bool; i64 iteration + <I: Iterator> regression; dyn-generic arity checked)"
