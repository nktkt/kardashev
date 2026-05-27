#!/usr/bin/env bash
# Phase 8.3 smoke test: kard-lsp speaks LSP over stdio and emits
# publishDiagnostics for a file with a deliberate type error.
#
# We send three messages (initialize, initialized, didOpen) and read
# back two responses (the initialize result + the publishDiagnostics
# notification). We verify the diagnostic text mentions the expected
# error.
set -euo pipefail

LSP=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kard-lsp" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kard-lsp" \
    "${RUNFILES_DIR:-}/_main/compiler/kard-lsp" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kard-lsp" \
    "./compiler/kard-lsp"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        LSP="$candidate"
        break
    fi
done

if [[ -z "$LSP" ]]; then
    echo "FAIL: kard-lsp binary not found"
    exit 1
fi

echo "Using kard-lsp at: $LSP"

# Build a multi-message stream. Each LSP message is preceded by a
# `Content-Length:` header and a blank line; the body is JSON.
encode() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
# Deliberate type error: `print(42)` from a fn without `! { io }`.
SRC='fn main() -> i64 { print(42); 0 }'
# Escape source for JSON (no embedded quotes / newlines, fortunately).
DIDOPEN=$(printf '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/t.kd","languageId":"kardashev","version":1,"text":"%s"}}}' "$SRC")
SHUT='{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

INPUT="$(encode "$INIT")$(encode "$INITED")$(encode "$DIDOPEN")$(encode "$SHUT")$(encode "$EXIT")"

OUT=$(printf '%s' "$INPUT" | "$LSP")

if ! grep -q '"capabilities"' <<< "$OUT"; then
    echo "FAIL: no initialize result in LSP output"
    echo "$OUT"
    exit 1
fi
if ! grep -q 'publishDiagnostics' <<< "$OUT"; then
    echo "FAIL: no publishDiagnostics notification in LSP output"
    echo "$OUT"
    exit 1
fi
if ! grep -q 'effect `io`' <<< "$OUT"; then
    echo "FAIL: diagnostic text did not mention the expected io-effect error"
    echo "$OUT"
    exit 1
fi

echo "PASS: kard-lsp emits the io-effect diagnostic via publishDiagnostics"
