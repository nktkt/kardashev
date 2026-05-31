#!/usr/bin/env bash
# v24 Phase 134 smoke test: `///` doc comments captured in the AST and surfaced.
#   - the formatter (kardfmt) round-trips them (capture -> attach -> print);
#   - LSP hover shows the doc below the signature fence;
#   - a doc'd program still compiles + runs;
#   - a plain `//` comment is NOT captured as doc.
set -uo pipefail

find_bin() {
    for c in \
        "${TEST_SRCDIR:-}/_main/compiler/$1" "${TEST_SRCDIR:-}/kardashev/compiler/$1" \
        "${RUNFILES_DIR:-}/_main/compiler/$1" "${RUNFILES_DIR:-}/kardashev/compiler/$1" \
        "./compiler/$1" "./build.local/$1"; do
        if [[ -n "$c" && -x "$c" ]]; then echo "$c"; return; fi
    done
}
KARDC=$(find_bin kardc); KARDFMT=$(find_bin kardfmt); KARDLSP=$(find_bin kard-lsp)
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
[[ -z "$KARDFMT" ]] && { echo "FAIL: kardfmt not found"; exit 1; }
echo "Using kardc=$KARDC kardfmt=$KARDFMT kard-lsp=${KARDLSP:-<none>}"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/d.kd" <<'EOF'
/// Adds two integers.
/// Returns their sum.
fn add(a: i64, b: i64) -> i64 { a + b } // a trailing comment, not doc
// a plain comment, not doc
fn main() -> i64 { add(20, 22) }
EOF

# --- compiles + runs (doc comments are transparent to compilation) ---
got=$("$KARDC" "$TMP/d.kd" 2>&1); [[ "$got" == "42" ]] || { echo "FAIL [run]: expected 42, got '$got'"; exit 1; }
echo "PASS [run]: a doc'd program compiles + runs"

# --- formatter preserves the doc lines (and only the doc lines) ---
fmt=$("$KARDFMT" "$TMP/d.kd")
[[ "$(grep -c '^/// ' <<<"$fmt")" -eq 2 ]] || { echo "FAIL [fmt]: expected 2 doc lines"; echo "$fmt"; exit 1; }
grep -q '/// Adds two integers.' <<<"$fmt" || { echo "FAIL [fmt]: first doc line lost"; echo "$fmt"; exit 1; }
grep -q 'plain comment' <<<"$fmt" && { echo "FAIL [fmt]: a plain // comment was captured as doc"; echo "$fmt"; exit 1; }
# round-trip stable
echo "$fmt" > "$TMP/d1.kd"; "$KARDFMT" "$TMP/d1.kd" > "$TMP/d2.kd"
diff -q "$TMP/d1.kd" "$TMP/d2.kd" >/dev/null || { echo "FAIL [fmt]: not idempotent"; exit 1; }
echo "PASS [fmt]: kardfmt round-trips the 2 doc lines (plain // dropped), idempotent"

# --- LSP hover surfaces the doc (if kard-lsp built) ---
if [[ -n "$KARDLSP" ]]; then
    txt=$(python3 -c 'import json;print(json.dumps(open("'"$TMP"'/d.kd").read()))')
    init='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":null,"capabilities":{}}}'
    open=$(printf '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file://%s/d.kd","languageId":"kardashev","version":1,"text":%s}}}' "$TMP" "$txt")
    hov=$(printf '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file://%s/d.kd"},"position":{"line":4,"character":19}}}' "$TMP")
    resp=$({ for m in "$init" "$open" "$hov"; do printf 'Content-Length: %d\r\n\r\n%s' "${#m}" "$m"; done; } | "$KARDLSP" 2>/dev/null)
    grep -q 'Adds two integers.' <<<"$resp" || { echo "FAIL [hover]: doc not in hover response"; exit 1; }
    echo "PASS [hover]: LSP hover on the call surfaces the function's doc"
else
    echo "SKIP [hover]: kard-lsp not built"
fi

echo "PASS: Phase 134 — /// doc comments captured + surfaced (formatter + LSP hover)"
