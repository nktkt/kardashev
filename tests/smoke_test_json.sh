#!/usr/bin/env bash
# Phase 38 capstone smoke test: the full nested-JSON example
# (examples/json/main.kd) — the v6 north star.
#   1. It parses `{"a":[1,{"b":true},"x"],"n":null,"k":-42}` into a recursive
#      `enum Json` (Vec<Json> arrays, HashMap<String,Json> objects), serializes
#      it back through a `Display` impl, RE-PARSES, and asserts DEEP (order-
#      independent) equality — round-trips => the program returns 966.
#   2. A constant-memory gate: a 200k-iteration parse+serialize+drop loop over a
#      deeply-nested document holds peak RSS flat (recursive Drop frees the whole
#      tree; string_push_str / hashmap get-ops free the values they consume).
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

# Locate the example source (Bazel runfiles or the source tree).
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

# 1. correctness: JIT prints the round-trip line + returns 966; AOT exits 966&255.
jit=$("$KARDC" "$SRC")
echo "$jit"
echo "$jit" | grep -q "round-trips = yes" || { echo "FAIL: round-trip not confirmed"; exit 1; }
echo "$jit" | tail -1 | grep -qx "966" || { echo "FAIL: expected signal 966, got $(echo "$jit" | tail -1)"; exit 1; }
"$KARDC" --no-cache -o "$TMP/json" "$SRC" >/dev/null
set +e; "$TMP/json" >/dev/null; rc=$?; set -e
[[ "$rc" -ne $((966 % 256)) ]] && { echo "FAIL [aot]: exit $rc expected $((966 % 256))"; exit 1; }
echo "PASS [parse/serialize/round-trip]: JIT signal 966, AOT exit $rc"

# 2. constant memory over a deeply-nested parse+serialize+drop loop.
cat > "$TMP/loop.kd" <<EOF
$(sed '/^fn main()/,$d' "$SRC")
fn main() -> i64 ! { io, alloc } {
    let input = "{ \"a\": [1, { \"b\": true }, \"x\"], \"n\": null, \"k\": -42, \"deep\": [[[1,2],[3]],{\"z\":[true,false,null]}] }";
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
echo "INFO [loop]: peak RSS over 200k nested parse+serialize+drop = ${rss} KB"
if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
    echo "FAIL [loop]: RSS ${rss} KB > 32 MB — the JSON pipeline leaks"; exit 1
fi
echo "PASS [loop]: nested JSON parse+serialize+drop — RSS flat (<= 32 MB)"

echo "PASS: Phase 38 — full nested JSON (parse + serialize, leak-free) JIT + AOT"
