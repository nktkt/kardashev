#!/usr/bin/env bash
# Phase 7.1 smoke test: `mod foo;` resolves a sibling .kd file and
# merges its declarations into the program. Exercises both file mode
# (JIT) and AOT mode through a multi-file program.
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

cat > "$TMP/util.kd" <<'EOF'
fn double(n: i64) -> i64 { n + n }
fn triple(n: i64) -> i64 { n + n + n }
EOF

cat > "$TMP/main.kd" <<'EOF'
mod util;
fn main() -> i64 { double(5) + triple(4) }
EOF

# JIT mode: kardc prints the result to stdout.
JIT_OUT=$("$KARDC" "$TMP/main.kd")
if [[ "$JIT_OUT" != "22" ]]; then
    echo "FAIL: JIT mod-resolution gave '$JIT_OUT' (expected 22)"
    exit 1
fi
echo "JIT result: 22"

# AOT mode: produced binary returns 22 as exit code.
"$KARDC" -o "$TMP/prog" "$TMP/main.kd"
set +e
"$TMP/prog"
rc=$?
set -e
if [[ "$rc" -ne 22 ]]; then
    echo "FAIL: AOT mod-resolution executable returned $rc (expected 22)"
    exit 1
fi

echo "PASS: mod foo; resolves siblings; JIT returns 22, AOT exit code 22"
