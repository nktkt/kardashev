#!/usr/bin/env bash
# v28 Phase 152: const-eval beyond i64/bool — array / tuple / struct / enum
# const VALUES, built + projected (A[i], p.field, t.0) at compile time, and
# usable as runtime values (the initializer is re-emitted per use). Bounds /
# non-const leaves are compile errors. JIT + AOT.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
run_eq() { local jit; jit=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$jit" == "$3" ]] || { echo "FAIL [$1/jit]: want $3 got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq $(( $3 & 255 )) ]] || { echo "FAIL [$1/aot]: exit $rc want $(( $3 & 255 ))"; exit 1; }
    echo "PASS [$1]: $4"; }
expect_err() { local out; out=$("$KARDC" "$2" 2>&1)
    [[ $? -ne 0 ]] || { echo "FAIL [$1]: expected error, compiled"; exit 1; }
    echo "$out" | grep -qiE "$3" || { echo "FAIL [$1]: want /$3/, got: $out"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) array const: compile-time projection into another const
cat > "$TMP/a.kd" <<'EOF'
const ARR: [i64; 3] = [10, 20, 30];
const SUM: i64 = ARR[0] + ARR[1] + ARR[2];
fn main() -> i64 { SUM }
EOF
run_eq array_proj "$TMP/a.kd" 60 "const array + compile-time index projection (60)"

# 2) array-repeat const [v; N]
cat > "$TMP/rep.kd" <<'EOF'
const Z: [i64; 4] = [7; 4];
const S: i64 = Z[0] + Z[3];
fn main() -> i64 { S }
EOF
run_eq array_repeat "$TMP/rep.kd" 14 "const [v; N] array-repeat (7+7=14)"

# 3) struct const: field projection
cat > "$TMP/s.kd" <<'EOF'
struct Point { x: i64, y: i64 }
const P: Point = Point { x: 3, y: 4 };
const X: i64 = P.x + P.y;
fn main() -> i64 { X }
EOF
run_eq struct_field "$TMP/s.kd" 7 "const struct + field projection (3+4=7)"

# 4) tuple const + .N projection
cat > "$TMP/t.kd" <<'EOF'
const PAIR: (i64, i64) = (8, 9);
const T0: i64 = PAIR.0;
fn main() -> i64 { T0 + PAIR.1 }
EOF
run_eq tuple "$TMP/t.kd" 17 "const tuple + .N projection (8+9=17)"

# 5) an aggregate const used as a whole RUNTIME value (initializer re-emitted)
cat > "$TMP/rt.kd" <<'EOF'
struct Point { x: i64, y: i64 }
const ORIGIN: Point = Point { x: 5, y: 6 };
fn sum(p: Point) -> i64 { p.x + p.y }
fn main() -> i64 { sum(ORIGIN) + ORIGIN.x }
EOF
run_eq runtime_value "$TMP/rt.kd" 16 "aggregate const as a runtime value (11 + 5 = 16)"

# 6) enum const value (payload + unit variant), matched at runtime
cat > "$TMP/e.kd" <<'EOF'
enum Opt { Nothing, Just(i64) }
const A: Opt = Just(42);
const B: Opt = Nothing;
fn val(o: Opt) -> i64 { match o { Just(x) => x, Nothing => 100 } }
fn main() -> i64 { val(A) + val(B) }
EOF
run_eq enum_const "$TMP/e.kd" 142 "const enum (Just(42) + Nothing -> 42 + 100 = 142)"

# 7) nested aggregate: a struct holding an array
cat > "$TMP/n.kd" <<'EOF'
struct Row { vals: [i64; 2], tag: i64 }
const R: Row = Row { vals: [3, 4], tag: 100 };
const SUM: i64 = R.vals[0] + R.vals[1] + R.tag;
fn main() -> i64 { SUM }
EOF
run_eq nested "$TMP/n.kd" 107 "nested const (struct holding an array, 3+4+100=107)"

# 8) out-of-bounds const index is a compile error
cat > "$TMP/oob.kd" <<'EOF'
const A: [i64; 2] = [1, 2];
const X: i64 = A[5];
fn main() -> i64 { X }
EOF
expect_err oob "$TMP/oob.kd" "out of bounds" "out-of-bounds const array index rejected"

echo "PASS: Phase 152 — const-eval for array/tuple/struct/enum aggregates"
