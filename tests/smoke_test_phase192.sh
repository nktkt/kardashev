#!/usr/bin/env bash
# v36 Phase 192 smoke test — LSP `textDocument/documentSymbol` (file outline).
#
# The server now advertises `documentSymbolProvider` and answers
# documentSymbol with a flat SymbolInformation[] of the USER's top-level
# fn / struct / enum declarations (parsed raw — no prelude noise), each with
# its LSP SymbolKind (Function=12, Struct=23, Enum=10) and 0-based position.
# Driven over stdio JSON-RPC with Content-Length framing.
set -euo pipefail

LSP=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kard-lsp" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kard-lsp" \
    "${RUNFILES_DIR:-}/_main/compiler/kard-lsp" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kard-lsp" \
    "./compiler/kard-lsp" \
    "./build.local/kard-lsp"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then LSP="$candidate"; break; fi
done
[[ -z "$LSP" ]] && { echo "FAIL: kard-lsp binary not found"; exit 1; }
echo "Using kard-lsp at: $LSP"

encode() { local body="$1"; printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body"; }
want() { echo "$OUT" | grep -qF -- "$1" || { echo "FAIL: response missing: $1"; echo "--- got ---"; echo "$OUT"; exit 1; }; echo "PASS: $1"; }

SRC='fn add(a: i64, b: i64) -> i64 { a + b }\nstruct Point { x: i64, y: i64 }\nenum Status { Ok, Bad }'
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
DIDOPEN=$(printf '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/t.kd","languageId":"kardashev","version":1,"text":"%s"}}}' "$SRC")
DOCSYM='{"jsonrpc":"2.0","id":50,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/t.kd"}}}'
SHUTDOWN='{"jsonrpc":"2.0","id":99,"method":"shutdown","params":{}}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":{}}'

OUT=$( { encode "$INIT"; encode "$INITED"; encode "$DIDOPEN"; encode "$DOCSYM"; encode "$SHUTDOWN"; encode "$EXIT"; } | "$LSP" 2>/dev/null )

# Capability handshake.
want 'documentSymbolProvider":true'
# Each top-level item with its SymbolKind.
want '"name":"add","kind":12'      # Function
want '"name":"Point","kind":23'    # Struct
want '"name":"Status","kind":10'   # Enum
# Positions are present + 0-based (add is on the first line).
want '"name":"add","kind":12,"location":{"uri":"file:///tmp/t.kd","range":{"start":{"line":0,"character":0}'
# No prelude symbols leak into the outline.
echo "$OUT" | grep -o '"id":50,"result":\[[^]]*\]' | grep -q 'vec_new\|hashmap_new\|option_map' && { echo "FAIL: prelude symbol leaked into documentSymbol"; exit 1; }
echo "PASS: prelude excluded from outline"

echo "ALL PHASE 192 SMOKE TESTS PASSED"
