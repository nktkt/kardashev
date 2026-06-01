#!/usr/bin/env bash
# v36 Phase 194 smoke test — `kardc --doc` Markdown API documentation.
#
# `--doc` parses the RAW user source (no prelude) and emits Markdown for every
# top-level fn / struct / enum: a rendered signature plus the item's `///` doc
# comment (captured in Phase 134). This asserts the generated doc contains the
# expected signatures, doc text, fields, and enum variants.
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

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/lib.kd" <<'EOF'
/// Adds two numbers and returns the sum.
pub fn add(a: i64, b: i64) -> i64 { a + b }

/// A 2D point in the plane.
pub struct Point { x: i64, y: i64 }

/// The result of an operation.
enum Status { Ok(i64), Failed }

fn undocumented(v: &Vec<i64>) -> i64 { vec_len(v) }
EOF

OUT=$("$KARDC" --doc "$TMP/lib.kd" 2>&1)

want() {
    # `grep -F -- "$1"`: the `--` is required because several patterns begin
    # with `-` (markdown list items) and would otherwise look like grep options.
    echo "$OUT" | grep -qF -- "$1" || { echo "FAIL: expected doc to contain: $1"; echo "--- got ---"; echo "$OUT"; exit 1; }
    echo "PASS: contains '$1'"
}

want '# API Documentation'
want '## Functions'
want '### `pub fn add(a: i64, b: i64) -> i64`'
want 'Adds two numbers and returns the sum.'
want '### `fn undocumented(v: &Vec<i64>) -> i64`'   # signature even without a doc
want '## Structs'
want '### `pub struct Point`'
want 'A 2D point in the plane.'
want '- `x: i64`'
want '## Enums'
want '### `enum Status`'
want '- `Ok(i64)`'
want '- `Failed`'

# A doc comment from the prelude must NOT leak in (raw source only).
echo "$OUT" | grep -q 'vec_new' && { echo "FAIL: prelude leaked into docs"; exit 1; }
echo "PASS: prelude excluded"

echo "ALL PHASE 194 SMOKE TESTS PASSED"
