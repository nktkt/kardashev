#!/usr/bin/env bash
# Phase 22 smoke test: fixed-size arrays `[T; N]` + tuples `(A, B)`, end to end
# through JIT and AOT.
#
#   1. Array literal `[a, b, c]` + indexing `arr[i]` (constant + dynamic).
#   2. Array as a fn param `fn sum3(a: [i64; 3]) -> i64`, an array of bool, and
#      an array field inside a struct.
#   3. Mutating an array element `a[i] = x` (a `let mut` array is a value type).
#   4. Tuple literal `(a, b)` + field access `t.0` / `t.1`.
#   5. `let (x, y) = t;` destructuring, a tuple RETURNED from a fn and
#      destructured, and a nested tuple `((1,2),3)` accessed via `n.0.0`.
#   6. A combined program: a fn returning `(i64, bool)`, destructured, plus an
#      array summed in a loop.
#   7. Negative cases: a compile-time-constant out-of-range index, a non-Copy
#      element type, and a destructuring arity mismatch are all rejected.
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
    echo "FAIL: kardc binary not found in runfiles"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Helper: run a program through JIT and assert its full stdout. Args:
#   $1 = label, $2 = source path, $3 = expected stdout.
jit_expect() {
    local label="$1" src="$2" want="$3"
    local got
    got=$("$KARDC" "$src")
    if [[ "$got" != "$want" ]]; then
        echo "FAIL [$label]: JIT output mismatch"
        echo "expected:"; echo "$want"
        echo "got:"; echo "$got"
        exit 1
    fi
}

# Helper: AOT-compile $2, run it, assert stdout ($3) and exit code ($4).
aot_expect() {
    local label="$1" src="$2" want_out="$3" want_rc="$4"
    "$KARDC" -o "$TMP/prog" "$src"
    set +e
    local got_out
    got_out=$("$TMP/prog")
    "$TMP/prog" > /dev/null
    local rc=$?
    set -e
    if [[ "$got_out" != "$want_out" ]]; then
        echo "FAIL [$label]: AOT stdout mismatch"
        echo "expected:"; echo "$want_out"
        echo "got:"; echo "$got_out"
        exit 1
    fi
    if [[ "$rc" -ne "$want_rc" ]]; then
        echo "FAIL [$label]: AOT exit code was $rc (expected $want_rc)"
        exit 1
    fi
}

# Helper: assert the compiler REJECTS $2 with a diagnostic matching $3.
reject_expect() {
    local label="$1" src="$2" pattern="$3"
    set +e
    local out
    out=$("$KARDC" "$src" 2>&1)
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        echo "FAIL [$label]: expected a compile error, but it compiled"
        exit 1
    fi
    if ! echo "$out" | grep -q "$pattern"; then
        echo "FAIL [$label]: expected a diagnostic matching '$pattern', got:"
        echo "$out"
        exit 1
    fi
}

# ===========================================================================
# 1. Array literal + indexing
# ===========================================================================
cat > "$TMP/arr.kd" <<'EOF'
fn main() -> i64 {
    let a = [10, 20, 30];
    a[0] + a[2]            // 40
}
EOF
jit_expect "array_index" "$TMP/arr.kd" "40"
aot_expect "array_index" "$TMP/arr.kd" "" 40
echo "PASS [array_index]: [10,20,30]; a[0]+a[2] == 40 (JIT + AOT)"

# Dynamic (variable) index.
cat > "$TMP/arr_dyn.kd" <<'EOF'
fn main() -> i64 ! { io } {
    let a = [5, 15, 25, 35];
    let mut i = 0;
    let mut acc = 0;
    while i < 4 { acc = acc + a[i]; i = i + 1; }
    print(acc);           // 80
    acc
}
EOF
jit_expect "array_dyn_index" "$TMP/arr_dyn.kd" $'80\n80'
aot_expect "array_dyn_index" "$TMP/arr_dyn.kd" "80" 80
echo "PASS [array_dyn_index]: dynamic index sum of [5,15,25,35] == 80 (JIT + AOT)"

# ===========================================================================
# 2. Array as a fn param, array of bool, array field inside a struct
# ===========================================================================
cat > "$TMP/arr_param.kd" <<'EOF'
fn sum3(a: [i64; 3]) -> i64 { a[0] + a[1] + a[2] }
fn main() -> i64 {
    let a = [3, 5, 7];
    sum3(a)               // 15
}
EOF
jit_expect "array_param" "$TMP/arr_param.kd" "15"
aot_expect "array_param" "$TMP/arr_param.kd" "" 15
echo "PASS [array_param]: fn sum3([i64;3]) over [3,5,7] == 15 (JIT + AOT)"

cat > "$TMP/arr_bool.kd" <<'EOF'
fn main() -> i64 {
    let flags = [true, false, true];
    if flags[0] {
        if flags[1] { 0 } else { 7 }   // -> 7
    } else { 0 - 1 }
}
EOF
jit_expect "array_bool" "$TMP/arr_bool.kd" "7"
aot_expect "array_bool" "$TMP/arr_bool.kd" "" 7
echo "PASS [array_bool]: [bool;3] element reads drive control flow -> 7 (JIT + AOT)"

cat > "$TMP/arr_struct.kd" <<'EOF'
struct Grid { cells: [i64; 4], n: i64 }
fn main() -> i64 {
    let g = Grid { cells: [2, 4, 6, 8], n: 4 };
    g.cells[1] + g.cells[3] + g.n      // 4 + 8 + 4 = 16
}
EOF
jit_expect "array_in_struct" "$TMP/arr_struct.kd" "16"
aot_expect "array_in_struct" "$TMP/arr_struct.kd" "" 16
echo "PASS [array_in_struct]: an [i64;4] field inside a struct, g.cells[1]+g.cells[3]+g.n == 16 (JIT + AOT)"

# ===========================================================================
# 3. Mutating an array element (value-type array)
# ===========================================================================
cat > "$TMP/arr_mut.kd" <<'EOF'
fn main() -> i64 {
    let mut a = [1, 2, 3];
    a[1] = 20;
    a[0] + a[1] + a[2]    // 1 + 20 + 3 = 24
}
EOF
jit_expect "array_mut" "$TMP/arr_mut.kd" "24"
aot_expect "array_mut" "$TMP/arr_mut.kd" "" 24
echo "PASS [array_mut]: a[1] = 20 then sum == 24 (JIT + AOT)"

# ===========================================================================
# 4. Tuple literal + field access
# ===========================================================================
cat > "$TMP/tuple_field.kd" <<'EOF'
fn main() -> i64 ! { io } {
    let t = (1, true);
    print(t.0);                       // 1
    if t.1 { t.0 } else { 0 - 1 }     // -> 1
}
EOF
jit_expect "tuple_field" "$TMP/tuple_field.kd" $'1\n1'
aot_expect "tuple_field" "$TMP/tuple_field.kd" "1" 1
echo "PASS [tuple_field]: (1, true); t.0 == 1, t.1 used (JIT + AOT)"

# ===========================================================================
# 5. let-destructuring, a tuple returned from a fn, a nested tuple
# ===========================================================================
cat > "$TMP/destr.kd" <<'EOF'
fn main() -> i64 {
    let (x, y) = (3, 4);
    x + y                 // 7
}
EOF
jit_expect "let_destructure" "$TMP/destr.kd" "7"
aot_expect "let_destructure" "$TMP/destr.kd" "" 7
echo "PASS [let_destructure]: let (x, y) = (3, 4); x + y == 7 (JIT + AOT)"

cat > "$TMP/tuple_ret.kd" <<'EOF'
fn divmod(a: i64, b: i64) -> (i64, i64) { (a / b, a - (a / b) * b) }
fn main() -> i64 {
    let (q, r) = divmod(17, 5);
    q * 100 + r           // 3*100 + 2 = 302
}
EOF
jit_expect "tuple_return" "$TMP/tuple_ret.kd" "302"
# 302 mod 256 = 46 as a process exit code.
aot_expect "tuple_return" "$TMP/tuple_ret.kd" "" 46
echo "PASS [tuple_return]: fn -> (i64,i64) returned + destructured, q*100+r == 302 (JIT; AOT exit 46)"

cat > "$TMP/tuple_nested.kd" <<'EOF'
fn main() -> i64 {
    let n = ((1, 2), 3);
    n.0.0 + n.0.1 + n.1   // 1 + 2 + 3 = 6
}
EOF
jit_expect "tuple_nested" "$TMP/tuple_nested.kd" "6"
aot_expect "tuple_nested" "$TMP/tuple_nested.kd" "" 6
echo "PASS [tuple_nested]: ((1,2),3); n.0.0 + n.0.1 + n.1 == 6 (JIT + AOT)"

# ===========================================================================
# 6. Combined: a fn returning (i64, bool), destructured, plus an array summed
# ===========================================================================
cat > "$TMP/combined.kd" <<'EOF'
fn classify(n: i64) -> (i64, bool) { (n * 2, n > 0) }
fn main() -> i64 ! { io } {
    let (doubled, positive) = classify(21);   // (42, true)
    let a = [doubled, doubled + 1, doubled + 2]; // [42, 43, 44]
    let mut sum = 0;
    let mut i = 0;
    while i < 3 { sum = sum + a[i]; i = i + 1; }  // 129
    print(sum);                                   // 129
    if positive { sum } else { 0 }                // -> 129
}
EOF
jit_expect "combined" "$TMP/combined.kd" $'129\n129'
aot_expect "combined" "$TMP/combined.kd" "129" 129
echo "PASS [combined]: classify(21) -> (42,true) destructured + [42,43,44] summed == 129 (JIT + AOT)"

# ===========================================================================
# 7. Negative cases
# ===========================================================================
cat > "$TMP/bad_oob.kd" <<'EOF'
fn main() -> i64 {
    let a = [1, 2, 3];
    a[5]
}
EOF
reject_expect "neg_oob" "$TMP/bad_oob.kd" "out of bounds"
echo "PASS [neg_oob]: a compile-time-constant out-of-range index a[5] is rejected"

# Phase 61 (v10): a fixed-size array over a NON-Copy element type (String) is
# now allowed — element-wise clone + element-wise Drop. (The old Copy-only
# restriction is lifted.) Build the AOT binary and run it to exercise the
# element drops without a leak/crash.
cat > "$TMP/noncopy.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let a: [String; 2] = [int_to_string(11), int_to_string(222)];
    str_len(&a[0]) + str_len(&a[1])
}
EOF
jit=$("$KARDC" "$TMP/noncopy.kd" 2>/dev/null | tail -1)
[[ "$jit" == "5" ]] || { echo "FAIL [noncopy]: expected 5 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/noncopy" "$TMP/noncopy.kd" >/dev/null 2>&1
set +e; "$TMP/noncopy"; rc=$?; set -e
[[ "$rc" -eq 5 ]] || { echo "FAIL [noncopy/aot]: exit $rc expected 5"; exit 1; }
# moving a non-Copy element OUT of an array by index is rejected (clone/borrow).
cat > "$TMP/noncopy_move.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let a: [String; 1] = [int_to_string(1)];
    let x = a[0];
    str_len(&x)
}
EOF
reject_expect "neg_noncopy_move" "$TMP/noncopy_move.kd" "cannot move a non-Copy"
echo "PASS [noncopy]: [String; 2] non-Copy array clones/drops; index move-out rejected (JIT + AOT)"

cat > "$TMP/bad_arity.kd" <<'EOF'
fn main() -> i64 {
    let (x, y, z) = (1, 2);
    x
}
EOF
reject_expect "neg_arity" "$TMP/bad_arity.kd" "tuple pattern binds"
echo "PASS [neg_arity]: a tuple-destructuring arity mismatch is rejected"

echo "PASS: fixed-size arrays [T; N] + tuples (A, B) work in JIT + AOT (literals, indexing, fn params/returns, struct fields, mutation, destructuring, nesting)"
