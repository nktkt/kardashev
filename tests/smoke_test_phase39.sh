#!/usr/bin/env bash
# Phase 39 smoke test: f64 floating-point.
#   1. Float literals (`1.5`, `3e8`), arithmetic (`+ - * /`), comparisons, and
#      unary negation compute correctly.
#   2. `to_f64` / `float_to_int` convert both ways (the latter truncates).
#   3. A `Vec<f64>` is built, summed by reference, and the total is exact.
#   4. `print_f64` prints a double; an f64 program returns an i64 via
#      float_to_int (JIT entry / AOT exit stay i64).
# JIT + AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check() { # name file expected-i64
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f")
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT matches"
}

# --- 1. arithmetic + comparison + negation ---
cat > "$TMP/arith.kd" <<'EOF'
fn main() -> i64 {
    let x = 1.5;
    let y = 2.5;
    let sum = x + y;          // 4.0
    let prod = x * y;         // 3.75
    let diff = -y + x;        // -1.0
    let q = sum / 2.0;        // 2.0
    let ok = if x < y { if prod > 3.0 { 1.0 } else { 0.0 } } else { 0.0 };
    // 4.0 + 3.75 + (-1.0) + 2.0 + 1.0 = 9.75 -> 9
    float_to_int(sum + prod + diff + q + ok)
}
EOF
check arith "$TMP/arith.kd" 9

# --- 2. conversions round-trip; exponent literal ---
cat > "$TMP/conv.kd" <<'EOF'
fn main() -> i64 {
    let big = 3e3;                 // 3000.0
    let half = to_f64(7) / 2.0;    // 3.5
    float_to_int(big + half)       // 3003.5 -> 3003
}
EOF
check conv "$TMP/conv.kd" 3003

# --- 3. Vec<f64> sum by reference ---
cat > "$TMP/vec.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 1.25);
    vec_push(&mut v, 2.5);
    vec_push(&mut v, 4.25);
    let mut i = 0;
    let mut acc = 0.0;
    let n = vec_len(&v);
    while i < n { acc = acc + *vec_get_ref(&v, i); i = i + 1; }
    float_to_int(acc * 4.0)        // 8.0 * 4 = 32.0 -> 32
}
EOF
check vec "$TMP/vec.kd" 32

# --- 4. print_f64 (io) ---
cat > "$TMP/print.kd" <<'EOF'
fn main() -> i64 ! { io } {
    let pi = 3.14159;
    print_f64(pi);
    print_f64(pi * 2.0);
    float_to_int(pi * 100.0)       // 314.159 -> 314
}
EOF
jit=$("$KARDC" "$TMP/print.kd")
echo "$jit" | grep -qx "3.14159" || { echo "FAIL [print]: missing 3.14159"; exit 1; }
echo "$jit" | grep -qx "6.28318" || { echo "FAIL [print]: missing 6.28318"; exit 1; }
echo "$jit" | tail -1 | grep -qx "314" || { echo "FAIL [print]: signal not 314"; exit 1; }
"$KARDC" --no-cache -o "$TMP/print" "$TMP/print.kd" >/dev/null
set +e; "$TMP/print" >/dev/null; r=$?; set -e
[[ "$r" -ne $((314 % 256)) ]] && { echo "FAIL [print/aot]: exit $r"; exit 1; }
echo "PASS [print]: print_f64 + i64 signal, JIT + AOT"

echo "PASS: Phase 39 — f64 floating-point (JIT + AOT)"
