#!/usr/bin/env bash
# Phase 5.x smoke test: the built-in `Vec` growable buffer + four
# operations (vec_new / vec_push / vec_get / vec_len) round-trip
# through both JIT and AOT.
#
# Test program builds a Vec, pushes three elements (forcing a heap
# realloc since vec_new starts at cap 0), then computes the sum
# recursively (no loops in the language yet) and prints individual
# entries + the total. Exit code is the sum so we can verify across
# the wire.
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

cat > "$TMP/vec.kd" <<'EOF'
fn sum_from(v: &Vec<i64>, i: i64) -> i64 {
    if i < vec_len(v) { vec_get(v, i) + sum_from(v, i + 1) }
    else              { 0 }
}
fn main() -> i64 ! { alloc, io } {
    let v = vec_new();
    vec_push(&mut v, 10);
    vec_push(&mut v, 20);
    vec_push(&mut v, 30);
    print(vec_len(&v));
    sum_from(&v, 0)
}
EOF

# JIT mode: kardc prints `vec_len` (=3) plus main's return (=60).
JIT_OUT=$("$KARDC" "$TMP/vec.kd")
EXPECTED=$'3\n60'
if [[ "$JIT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: JIT Vec output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$JIT_OUT"
    exit 1
fi
echo "JIT Vec sum: 60 (10+20+30)"

# AOT: the program prints `3` once, then returns 60 as the exit code.
# Wrap both invocations in `set +e` because $() and direct runs propagate
# the non-zero exit code (60) and would otherwise trip `set -e`.
"$KARDC" -o "$TMP/prog" "$TMP/vec.kd"
set +e
AOT_STDOUT=$("$TMP/prog")
"$TMP/prog" > /dev/null
rc=$?
set -e
if [[ "$AOT_STDOUT" != "3" ]]; then
    echo "FAIL: AOT Vec stdout was '$AOT_STDOUT' (expected '3')"
    exit 1
fi
if [[ "$rc" -ne 60 ]]; then
    echo "FAIL: AOT Vec exit code was $rc (expected 60)"
    exit 1
fi

echo "PASS: Vec round-trips through JIT + AOT (push×3, len=3, sum=60)"
