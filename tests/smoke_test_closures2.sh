#!/usr/bin/env bash
# Phase 17a smoke test: richer closures & first-class fn values.
#
#   1. Field-held fn value: a `struct Adder { f: fn(i64)->i64 }` built with a
#      top-level fn, called via `(a.f)(10)`; and one built with a CAPTURING
#      closure, called via `(b.f)(5)`.
#   2. Lazy-adaptor shape: a `struct MapIter { base, f }` whose inherent
#      `step(self)` applies `(self.f)(self.base)` — proves a stored closure
#      field is callable through `self`, for both a top-level fn and a closure.
#   3. Call a fn value returned by an expression: `(make_adder())(41)`.
#   4. FnMut counter: `let mut n=0; let mut inc=|| { n=n+1; }; inc();inc();inc(); n`
#      => 3 (mutation visible after the closure call).
#   5. FnMut accumulator: a closure adding its arg to a captured running total
#      across several calls.
#   6. Soundness: returning a closure that captures by reference is rejected;
#      mutating a non-`mut` captured binding is rejected.
#
# Every positive case is run through BOTH the JIT (default) and AOT (`-o`).
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

# Helper: compile+run a program through JIT then AOT, comparing the printed
# lines to an expected multi-line string. (AOT output excludes the final
# `return` value that the JIT REPL echoes.)
run_jit_aot() {
    local name="$1" expected="$2"
    local jit
    jit=$("$KARDC" "$TMP/$name.kd"); jit=$(head -n "$(echo "$expected" | wc -l)" <<< "$jit")
    if [[ "$jit" != "$expected" ]]; then
        echo "FAIL: $name JIT output mismatch"
        echo "expected:"; echo "$expected"
        echo "got:"; echo "$jit"
        exit 1
    fi
    "$KARDC" -o "$TMP/$name.bin" "$TMP/$name.kd"
    local aot
    aot=$("$TMP/$name.bin")
    if [[ "$aot" != "$expected" ]]; then
        echo "FAIL: $name AOT output mismatch"
        echo "expected:"; echo "$expected"
        echo "got:"; echo "$aot"
        exit 1
    fi
    echo "OK ($name): JIT + AOT match"
}

# --- 1. Field-held fn value (top-level fn + capturing closure). ---
cat > "$TMP/field.kd" <<'EOF'
struct Adder { f: fn(i64) -> i64 }
fn inc(x: i64) -> i64 { x + 1 }
fn main() -> i64 ! {io} {
    let a = Adder { f: inc };
    print((a.f)(10));          // 11
    let base = 100;
    let b = Adder { f: |x| x + base };
    print((b.f)(5));           // 105
    0
}
EOF
run_jit_aot field $'11\n105'

# --- 2. Lazy-adaptor shape: `(self.f)(self.base)` through an inherent method. ---
cat > "$TMP/mapiter.kd" <<'EOF'
struct MapIter { base: i64, f: fn(i64) -> i64 }
impl MapIter {
    fn step(self) -> i64 { (self.f)(self.base) }
}
fn dbl(x: i64) -> i64 { x + x }
fn main() -> i64 ! {io} {
    let m = MapIter { base: 21, f: dbl };
    print(m.step());           // 42
    let bump = 100;
    let m2 = MapIter { base: 5, f: |x| x + bump };
    print(m2.step());          // 105
    0
}
EOF
run_jit_aot mapiter $'42\n105'

# --- 3. Call a fn value produced by an arbitrary expression. ---
cat > "$TMP/exprcallee.kd" <<'EOF'
fn make_adder() -> fn(i64) -> i64 { |x| x + 1 }
fn main() -> i64 ! {io} {
    print((make_adder())(41)); // 42
    0
}
EOF
run_jit_aot exprcallee $'42'

# --- 4. FnMut counter: mutation persists after the call (-> 3). ---
cat > "$TMP/counter.kd" <<'EOF'
fn main() -> i64 ! {io} {
    let mut n = 0;
    let mut inc = || { n = n + 1; };
    inc();
    inc();
    inc();
    print(n);                  // 3
    0
}
EOF
run_jit_aot counter $'3'

# The counter's RETURN value (the JIT echoes it) must also be 3.
COUNTER_RET=$("$KARDC" "$TMP/counter.kd" | tail -n 1)
if [[ "$COUNTER_RET" != "0" ]]; then
    echo "FAIL: counter main returns $COUNTER_RET (expected trailing 0)"
    exit 1
fi
cat > "$TMP/counter_ret.kd" <<'EOF'
fn main() -> i64 ! {io} {
    let mut n = 0;
    let mut inc = || { n = n + 1; };
    inc(); inc(); inc();
    n
}
EOF
# AOT exit code carries the (truncated) return value: 3.
"$KARDC" -o "$TMP/counter_ret.bin" "$TMP/counter_ret.kd"
set +e
"$TMP/counter_ret.bin"
RET=$?
set -e
if [[ "$RET" != "3" ]]; then
    echo "FAIL: FnMut counter return value is $RET, expected 3"
    exit 1
fi
echo "OK (counter): FnMut counter reaches 3 (printed + returned)"

# --- 5. FnMut accumulator: captured running total across calls. ---
cat > "$TMP/accum.kd" <<'EOF'
fn main() -> i64 ! {io} {
    let mut total = 0;
    let mut add = |x| { total = total + x; total };
    print(add(5));             // 5
    print(add(10));            // 15
    print(add(3));             // 18
    print(total);              // 18
    0
}
EOF
run_jit_aot accum $'5\n15\n18\n18'

# --- 6a. Soundness: returning a by-ref (FnMut) closure is rejected. ---
cat > "$TMP/escape.kd" <<'EOF'
fn bad() -> fn() -> i64 {
    let mut n = 0;
    || { n = n + 1; n }
}
fn main() -> i64 { 0 }
EOF
set +e
ESC_OUT=$("$KARDC" "$TMP/escape.kd" 2>&1)
ESC_RC=$?
set -e
if [[ "$ESC_RC" -eq 0 ]]; then
    echo "FAIL: returning a by-ref capturing closure should be rejected"
    exit 1
fi
if ! grep -q "by reference" <<< "$ESC_OUT"; then
    echo "FAIL: expected an escape diagnostic mentioning by-reference capture"
    echo "$ESC_OUT"
    exit 1
fi
echo "OK (escape): returning an FnMut closure is rejected"

# --- 6b. Soundness: mutating a non-`mut` captured binding is rejected. ---
cat > "$TMP/nomut.kd" <<'EOF'
fn main() -> i64 {
    let n = 0;
    let f = || { n = n + 1; };
    f();
    0
}
EOF
set +e
NOMUT_OUT=$("$KARDC" "$TMP/nomut.kd" 2>&1)
NOMUT_RC=$?
set -e
if [[ "$NOMUT_RC" -eq 0 ]]; then
    echo "FAIL: mutating a non-mut captured binding should be rejected"
    exit 1
fi
if ! grep -q "not declared \`let mut\`" <<< "$NOMUT_OUT"; then
    echo "FAIL: expected a 'not declared let mut' diagnostic"
    echo "$NOMUT_OUT"
    exit 1
fi
echo "OK (nomut): mutating a non-mut capture is rejected"

echo "PASS: field-held fn values callable via (s.f)(x); lazy-adaptor shape;"
echo "      expression callee; FnMut counter -> 3; FnMut accumulator; escape"
echo "      and non-mut-capture soundness — all in JIT + AOT"
