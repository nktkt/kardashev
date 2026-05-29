#!/usr/bin/env bash
# JSON capstone smoke test: the full nested-JSON example (examples/json/main.kd).
# Roadmap v7 (Phase 44) — JSON 2.0: floats + string escapes + #[derive(Clone, Eq)].
#   1. It parses `{"pi":3.14159,"msg":"a\nb","xs":[1.5,-2.0,true,null]}` into a
#      recursive `#[derive(Clone, Eq)] enum Json` with `JNum(f64)`, decodes the
#      runtime string escape, serializes back through a `Display` impl, RE-PARSES,
#      and confirms a round trip via the DERIVED `Eq` (no hand-written json_eq).
#      Returns the serialized byte length (51) on a successful round trip.
#   2. A constant-memory gate: a 200k-iteration parse+serialize+drop loop holds
#      peak RSS flat (recursive Drop frees the whole tree; the string builders
#      free what they consume).
# JIT + AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

SRC=""
for cand in \
    "${TEST_SRCDIR:-}/_main/examples/json/main.kd" \
    "${TEST_SRCDIR:-}/kardashev/examples/json/main.kd" \
    "${RUNFILES_DIR:-}/_main/examples/json/main.kd" \
    "${RUNFILES_DIR:-}/kardashev/examples/json/main.kd" \
    "examples/json/main.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/json/main.kd not found"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. correctness: round-trips via derived Eq; signal = serialized length (51).
jit=$("$KARDC" "$SRC")
echo "$jit"
echo "$jit" | grep -q "round-trips = yes" || { echo "FAIL: round-trip not confirmed"; exit 1; }
echo "$jit" | grep -q 'serialized = {"pi":3.14159,"msg":"a\\nb","xs":\[1.5,-2,true,null\]}' \
    || { echo "FAIL: serialized form unexpected"; exit 1; }
sig=$(echo "$jit" | tail -1)
echo "$jit" | tail -1 | grep -qx "51" || { echo "FAIL: expected signal 51, got $sig"; exit 1; }
"$KARDC" --no-cache -o "$TMP/json" "$SRC" >/dev/null
set +e; "$TMP/json" >/dev/null; rc=$?; set -e
[[ "$rc" -ne $((51 % 256)) ]] && { echo "FAIL [aot]: exit $rc expected $((51 % 256))"; exit 1; }
echo "PASS [parse/serialize/derived-Eq round-trip]: JIT signal 51, AOT exit $rc"

# 2. constant memory over a parse+serialize+drop loop (floats + escapes + nesting).
cat > "$TMP/loop.kd" <<EOF
$(sed '/^fn main()/,$d' "$SRC")
fn main() -> i64 ! { io, alloc } {
    let input = "{ \"pi\": 3.14159, \"msg\": \"a\\\\nb\\\\tc\", \"xs\": [1.5, -2.0, true, null, {\"k\": 7.25}] }";
    let mut i = 0;
    let mut acc = 0;
    while i < 200000 {
        match parse(clone(&input)) {
            Ok(j) => { let t = j.to_string(); acc = acc + str_len(&t); i = i + 1; },
            Err(e) => { i = i + 1; },
        }
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$( /usr/bin/time -v "$TMP/loop" 2>&1 | awk '/Maximum resident/ {print $NF}' )
echo "INFO [loop]: peak RSS over 200k JSON-2.0 parse+serialize+drop = ${rss} KB"
if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
    echo "FAIL [loop]: RSS ${rss} KB > 32 MB — the JSON pipeline leaks"; exit 1
fi
echo "PASS [loop]: JSON-2.0 parse+serialize+drop — RSS flat (<= 32 MB)"

echo "PASS: Phase 44 — JSON 2.0 (floats + escapes + #[derive(Clone, Eq)]) JIT + AOT"
