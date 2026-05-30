#!/usr/bin/env bash
# JSON capstone smoke test: the full nested-JSON example (examples/json/main.kd).
# Roadmap v8 (Phase 50) — JSON 3.0: HashMap objects + full derive + canonical
# sorted-key output.
#   1. It parses `{ "b": 2, "a": [1, 2.5], "c": "x\ny" }` (keys OUT of order)
#      into a recursive `#[derive(Clone, Eq)] enum Json` whose objects are a
#      `JObj(HashMap<String, Json>)`, decodes the runtime string escape, then
#      serializes through a `Display` impl that SORTS the map keys (P47 sort) for
#      CANONICAL byte-stable output `{"a":[1,2.5],"b":2,"c":"x\ny"}`, RE-PARSES,
#      and confirms a round trip via the DERIVED `Eq` — whose HashMap arm is
#      ORDER-INDEPENDENT (no hand-written json_eq, no clone intrinsic).
#      Returns the serialized byte length (30) on a successful round trip.
#   2. A constant-memory gate: a 200k-iteration parse+serialize+drop loop holds
#      peak RSS flat (recursive Drop frees the whole tree incl. the HashMaps;
#      the string builders free what they consume).
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

# Portable peak-RSS (KB): GNU `time -v` (Linux) or BSD `time -l` (macOS); empty
# if neither exists (caller SKIPs the gate). Safe under `set -e`/`pipefail`.
peak_rss_kb() {
    local f; f=$(mktemp)
    if /usr/bin/time -v true >/dev/null 2>&1; then
        { /usr/bin/time -v "$@" >/dev/null; } 2>"$f" || true
        awk '/Maximum resident set size/ {print $NF}' "$f"
    elif /usr/bin/time -l true >/dev/null 2>&1; then
        { /usr/bin/time -l "$@" >/dev/null; } 2>"$f" || true
        awk '/maximum resident set size/ {print int($1/1024)}' "$f"
    fi
    rm -f "$f"
}

# 1. correctness: round-trips via derived Eq; signal = serialized length (51).
jit=$("$KARDC" "$SRC")
echo "$jit"
grep -q "round-trips = yes" <<< "$jit" || { echo "FAIL: round-trip not confirmed"; exit 1; }
grep -q 'serialized = {"a":\[1,2.5\],"b":2,"c":"x\\ny"}' <<< "$jit" \
    || { echo "FAIL: canonical (sorted-key) serialized form unexpected"; exit 1; }
sig=$(echo "$jit" | tail -1)
echo "$jit" | tail -1 | grep -qx "30" || { echo "FAIL: expected signal 30, got $sig"; exit 1; }
"$KARDC" --no-cache -o "$TMP/json" "$SRC" >/dev/null
set +e; "$TMP/json" >/dev/null; rc=$?; set -e
[[ "$rc" -ne $((30 % 256)) ]] && { echo "FAIL [aot]: exit $rc expected $((30 % 256))"; exit 1; }
echo "PASS [parse/serialize/derived-Eq round-trip, canonical sorted keys]: JIT signal 30, AOT exit $rc"

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
rss=$(peak_rss_kb "$TMP/loop")
if [[ -z "$rss" ]]; then
    echo "SKIP [loop]: no GNU/BSD /usr/bin/time available for the RSS gate"
else
    echo "INFO [loop]: peak RSS over 200k JSON-3.0 parse+serialize+drop = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [loop]: RSS ${rss} KB > 32 MB — the JSON pipeline leaks"; exit 1
    fi
    echo "PASS [loop]: JSON-3.0 parse+serialize+drop (incl. HashMap objects) — RSS flat (<= 32 MB)"
fi

echo "PASS: Phase 50 — JSON 3.0 (HashMap objects + full derive + canonical sorted output) JIT + AOT"
