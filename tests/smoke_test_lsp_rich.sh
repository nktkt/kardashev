#!/usr/bin/env bash
# Phase 14b smoke test: kard-lsp answers the rich requests (hover,
# completion, definition) in addition to diagnostics.
#
# We drive a small two-function program over stdio:
#   line 0: fn greet(n: i64) -> i64 ! { io } { print(n); n }
#   line 1: fn main() -> i64 ! { io } { let x = greet(7); x }
# then send initialize + didOpen + three requests + shutdown/exit.
#
# Asserts:
#   (a) hover on the `greet` call shows the function's effect row (! { io }),
#   (b) completion includes a known function (`greet`) AND a keyword (`match`),
#   (c) definition on the `greet` call returns a Location with a plausible
#       range pointing at the declaration line (line 0).
#
# Each LSP message is Content-Length framed (CRLF + blank line + JSON body).
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

# Frame a single LSP message with its Content-Length header.
encode() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

# Two-line program. We pass the newline as a literal "\n" escape inside the
# JSON string; the server decodes it back to a real newline (so line 1 is the
# `main` function). Column offsets below are 0-based into the decoded text.
#   line 1: `fn main() -> i64 ! { io } { let x = greet(7); x }`
#            0123456789...                              ^col 36 = `greet`
GREET_COL=36
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
SRC='fn greet(n: i64) -> i64 ! { io } { print(n); n }\nfn main() -> i64 ! { io } { let x = greet(7); x }'
DIDOPEN=$(printf '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/t.kd","languageId":"kardashev","version":1,"text":"%s"}}}' "$SRC")
HOVER=$(printf '{"jsonrpc":"2.0","id":10,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/t.kd"},"position":{"line":1,"character":%d}}}' "$GREET_COL")
COMPLETION=$(printf '{"jsonrpc":"2.0","id":11,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/t.kd"},"position":{"line":1,"character":%d}}}' "$GREET_COL")
DEFINITION=$(printf '{"jsonrpc":"2.0","id":12,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/t.kd"},"position":{"line":1,"character":%d}}}' "$GREET_COL")
SHUT='{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

INPUT="$(encode "$INIT")$(encode "$INITED")$(encode "$DIDOPEN")$(encode "$HOVER")$(encode "$COMPLETION")$(encode "$DEFINITION")$(encode "$SHUT")$(encode "$EXIT")"

OUT=$(printf '%s' "$INPUT" | "$LSP")

# Normalise CRLF so grep can scan line-by-line.
OUT_NL=$(printf '%s' "$OUT" | tr '\r' '\n')

# 0. Capabilities must advertise the three providers.
for cap in hoverProvider completionProvider definitionProvider; do
    if ! grep -q "\"$cap\"" <<< "$OUT_NL"; then
        echo "FAIL: initialize result missing capability $cap"
        echo "$OUT_NL"
        exit 1
    fi
done

# Extract just the hover response (id 10) for a focused effect-row check.
HOVER_RESP=$(grep '"id":10' <<< "$OUT_NL" || true)
# (a) hover shows greet's effect row.
if ! grep -q 'fn greet' <<< "$HOVER_RESP"; then
    echo "FAIL: hover did not return greet's signature"
    echo "$OUT_NL"
    exit 1
fi
if ! grep -q '! { io }' <<< "$HOVER_RESP"; then
    echo "FAIL: hover signature did not include the effect row '! { io }'"
    echo "$OUT_NL"
    exit 1
fi

# (b) completion includes a known function + a keyword.
COMPL_RESP=$(grep '"id":11' <<< "$OUT_NL" || true)
if ! grep -q '"label":"greet"' <<< "$COMPL_RESP"; then
    echo "FAIL: completion did not include the function 'greet'"
    echo "$OUT_NL"
    exit 1
fi
if ! grep -q '"label":"match"' <<< "$COMPL_RESP"; then
    echo "FAIL: completion did not include the keyword 'match'"
    echo "$OUT_NL"
    exit 1
fi

# (c) definition returns a Location with a plausible range on the decl line.
DEF_RESP=$(grep '"id":12' <<< "$OUT_NL" || true)
if ! grep -q '"uri":"file:///tmp/t.kd"' <<< "$DEF_RESP"; then
    echo "FAIL: definition did not return a Location with the document uri"
    echo "$OUT_NL"
    exit 1
fi
# greet is declared on line 0; the returned range must start there.
if ! grep -q '"start":{"line":0' <<< "$DEF_RESP"; then
    echo "FAIL: definition range does not point at greet's declaration line (0)"
    echo "$OUT_NL"
    exit 1
fi

echo "PASS: kard-lsp serves effect-row hover, completion (fn + keyword), and go-to-definition"
