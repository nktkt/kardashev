#!/usr/bin/env bash
# v37 — turbofish: explicit generic type arguments `f::<T, ...>(args)`.
#
# Explicit type args bind a generic fn's parameters positionally, constraining
# inference; inference still works when they're omitted. Too many args / a
# conflict with the argument types is a clear error. Differential JIT vs AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" \
    "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$("$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n") || true
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}
expect_err() {
    local name="$1" needle="$2" src="$3"
    printf '%s' "$src" > "$TMP/$name.kd"
    local err; err=$("$KARDC" "$TMP/$name.kd" 2>&1 >/dev/null || true)
    echo "$err" | grep -qi "$needle" || { echo "FAIL [$name]: expected '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# A battery of generic helpers exercised with explicit type args.
PRE='
fn id<T>(x: T) -> T { x }
fn first<A, B>(a: A, b: B) -> A { a }
fn second<A, B>(a: A, b: B) -> B { b }
fn pair<A, B>(a: A, b: B) -> (A, B) { (a, b) }
'
diff_run id_i64        $'42' "$PRE"$'fn main() -> i64 ! { io } { print(id::<i64>(42)); 0 }'
diff_run id_infer      $'42' "$PRE"$'fn main() -> i64 ! { io } { print(id(42)); 0 }'
diff_run id_bool       $'1'  "$PRE"$'fn main() -> i64 ! { io } { if id::<bool>(true) { print(1); } else { print(0); } 0 }'
diff_run first_two     $'7'  "$PRE"$'fn main() -> i64 ! { io } { print(first::<i64, bool>(7, true)); 0 }'
diff_run second_two    $'9'  "$PRE"$'fn main() -> i64 ! { io } { print(second::<bool, i64>(true, 9)); 0 }'
diff_run pair_tuple    $'3\n4' "$PRE"$'fn main() -> i64 ! { io } { let p = pair::<i64, i64>(3, 4); print(p.0); print(p.1); 0 }'
diff_run nested        $'5'  "$PRE"$'fn main() -> i64 ! { io } { print(id::<i64>(id::<i64>(5))); 0 }'
diff_run mixed         $'8'  "$PRE"$'fn main() -> i64 ! { io } { print(id::<i64>(3) + id(5)); 0 }'

# NEGATIVE: too many type arguments.
expect_err too_many 'type argument' "$PRE"$'fn main() -> i64 { id::<i64, bool>(1) }'
# NEGATIVE: explicit type arg conflicts with the argument type.
expect_err conflict 'expected' "$PRE"$'fn main() -> i64 ! { io } { print(id::<bool>(42)); 0 }'

echo "ALL TURBOFISH SMOKE TESTS PASSED"
