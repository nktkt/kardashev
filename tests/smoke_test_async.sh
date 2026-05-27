#!/usr/bin/env bash
# Phase 6.1 smoke test: async fn returns Future; .await unwraps. The
# program calls an async fn that calls another async fn, and the outer
# await reaches all the way through.
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

cat > "$TMP/async.kd" <<'EOF'
async fn add(a: i64, b: i64) -> i64 { a + b }
async fn double(n: i64) -> i64 { add(n, n).await }
fn main() -> i64 ! { async, io } {
    let v = double(21).await;
    print(v);
    0
}
EOF

JIT_OUT=$("$KARDC" "$TMP/async.kd")
EXPECTED=$'42\n0'
if [[ "$JIT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: JIT async output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$JIT_OUT"
    exit 1
fi
echo "JIT async: double(21).await = 42"

"$KARDC" -o "$TMP/prog" "$TMP/async.kd"
AOT_OUT=$("$TMP/prog")
if [[ "$AOT_OUT" != "42" ]]; then
    echo "FAIL: AOT async stdout was '$AOT_OUT' (expected '42')"
    exit 1
fi
echo "AOT async: print(42) on stdout, exit 0"

echo "PASS: async fn returns Future; .await chain unwraps to 42"
