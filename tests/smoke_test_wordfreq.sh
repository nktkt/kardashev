#!/usr/bin/env bash
# Word-frequency capstone smoke test — Roadmap v9 (Phase 56). The full pipeline
# in examples/wordfreq/main.kd, integrating the whole v9 line:
#   tokenize (str_split/str_trim, P54) -> count in a HashMap<String,i64>
#   (P45/46) -> hashmap_entries -> Vec<(String,i64)> (P55) -> wrap in a
#   #[derive(Ord, Eq)] Count + sort (P47/P48) -> print top-N (deterministic).
#   1. Over the fixed text "the cat sat on the mat the cat ran", the top 3 are
#      `the 3`, `cat 2`, `mat 1` (ties among the 1-counts broken by word asc),
#      and the packed signal is 321.
#   2. A constant-memory gate: a 200k count+rank+drop loop holds peak RSS flat —
#      the fix that made this pass is hashmap_insert freeing the redundant key +
#      old value on a DUPLICATE-key insert (the counter hot path).
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
    "${TEST_SRCDIR:-}/_main/examples/wordfreq/main.kd" \
    "${TEST_SRCDIR:-}/kardashev/examples/wordfreq/main.kd" \
    "${RUNFILES_DIR:-}/_main/examples/wordfreq/main.kd" \
    "${RUNFILES_DIR:-}/kardashev/examples/wordfreq/main.kd" \
    "examples/wordfreq/main.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/wordfreq/main.kd not found"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. correctness: top-3 lines + the packed signal (321).
jit=$("$KARDC" "$SRC")
echo "$jit"
grep -qx "the 3" <<< "$jit" || { echo "FAIL: expected 'the 3'"; exit 1; }
grep -qx "cat 2" <<< "$jit" || { echo "FAIL: expected 'cat 2'"; exit 1; }
grep -qx "mat 1" <<< "$jit" || { echo "FAIL: expected 'mat 1'"; exit 1; }
echo "$jit" | tail -1 | grep -qx "321" || { echo "FAIL: expected signal 321, got $(echo "$jit" | tail -1)"; exit 1; }
"$KARDC" --no-cache -o "$TMP/wf" "$SRC" >/dev/null
set +e; "$TMP/wf" >/dev/null; rc=$?; set -e
[[ "$rc" -ne $((321 % 256)) ]] && { echo "FAIL [aot]: exit $rc expected $((321 % 256))"; exit 1; }
echo "PASS [pipeline]: top-3 the/cat/mat, JIT signal 321, AOT exit $rc"

# 2. constant memory over a count+rank+drop loop (exercises duplicate-key inserts).
cat > "$TMP/loop.kd" <<EOF
$(sed '/^fn main()/,$d' "$SRC")
fn main() -> i64 ! { io, alloc } {
    let text = "the cat sat on the mat the cat ran over the lazy dog and the cat sat";
    let mut i = 0;
    let mut acc = 0;
    while i < 200000 {
        let m = count_words(&text);
        let ranked = rank(&m);
        acc = acc + vec_len(&ranked);
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
# Portable peak-RSS (KB) for a command: GNU `time -v` (Linux) or BSD `time -l`
# (macOS); empty if neither exists (the caller then SKIPs the gate). Never trips
# `set -e`/`pipefail`, even when the measured program exits non-zero.
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

"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$(peak_rss_kb "$TMP/loop")
if [[ -z "$rss" ]]; then
    echo "SKIP [loop]: no GNU/BSD /usr/bin/time available for the RSS leak gate"
else
    echo "INFO [loop]: peak RSS over 200k count+rank+drop = ${rss} KB"
    [[ "$rss" -gt 32768 ]] && { echo "FAIL [loop]: RSS ${rss} KB > 32 MB — the word-count pipeline leaks"; exit 1; }
    echo "PASS [loop]: count+rank+drop (duplicate-key inserts) — RSS flat (<= 32 MB)"
fi

echo "PASS: Phase 56 — word-frequency histogram capstone (JIT + AOT)"
