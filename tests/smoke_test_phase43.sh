#!/usr/bin/env bash
# Phase 43 smoke test: runtime string escapes + closing the async-frame leak.
#   1. str_unescape / str_escape round-trip a runtime string with escape
#      sequences byte-exactly (built from bytes so it's a RUNTIME string, not a
#      lexer-decoded literal); \uXXXX decodes the Latin-1 subset.
#   2. An async program that completes many futures via block_on holds peak-RSS
#      flat — the completed frames + poll slots are reclaimed.
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

check() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f")
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT matches"
}

# --- 1. escape round-trip: build `a\nb\tc\"d` (10 bytes), decode -> 7, re-encode -> 10, byte-exact ---
cat > "$TMP/esc.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut raw = string_new();
    str_push_byte(&mut raw, 97);  str_push_byte(&mut raw, 92); str_push_byte(&mut raw, 110);
    str_push_byte(&mut raw, 98);  str_push_byte(&mut raw, 92); str_push_byte(&mut raw, 116);
    str_push_byte(&mut raw, 99);  str_push_byte(&mut raw, 92); str_push_byte(&mut raw, 34);
    str_push_byte(&mut raw, 100);
    let dec = str_unescape(&raw);          // 7 bytes: a LF b TAB c " d
    let enc = str_escape(&dec);            // back to the 10-char escaped form
    let same = if str_eq(&raw, &enc) { 1 } else { 0 };
    str_len(&dec) * 100 + str_len(&enc) + same * 1000   // 700 + 10 + 1000 = 1710
}
EOF
check esc "$TMP/esc.kd" 1710

# --- 2. \u decode (A -> 'A' = 65) ---
cat > "$TMP/u.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut raw = string_new();
    str_push_byte(&mut raw, 92); str_push_byte(&mut raw, 117); // \ u
    str_push_byte(&mut raw, 48); str_push_byte(&mut raw, 48);  // 0 0
    str_push_byte(&mut raw, 52); str_push_byte(&mut raw, 49);  // 4 1
    let dec = str_unescape(&raw);
    str_char_at(&dec, 0)                   // 'A' = 65
}
EOF
check u "$TMP/u.kd" 65

# --- 3. async constant-memory: complete 300k futures via block_on ---
cat > "$TMP/async.kd" <<'EOF'
async fn add(a: i64, b: i64) -> i64 { a + b }
async fn double(n: i64) -> i64 { add(n, n).await }
fn main() -> i64 ! { io } {
    let mut i = 0;
    let mut acc = 0;
    while i < 300000 { acc = acc + block_on(double(i)); i = i + 1; }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/async" "$TMP/async.kd" >/dev/null
set +e; "$TMP/async" >/dev/null; ar=$?; set -e
[[ "$ar" -ne 0 ]] && { echo "FAIL [async]: exit $ar"; exit 1; }
rss=$(peak_rss_kb "$TMP/async")
if [[ -z "$rss" ]]; then
    echo "SKIP [async]: no GNU/BSD /usr/bin/time available for the RSS gate"
else
    echo "INFO [async]: peak RSS over 300k block_on(double) = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [async]: RSS ${rss} KB > 32 MB — async frames leak"; exit 1
    fi
    echo "PASS [async]: 300k completed futures — RSS flat (frames reclaimed)"
fi

echo "PASS: Phase 43 — runtime string escapes + async-frame reclaim (JIT + AOT)"
