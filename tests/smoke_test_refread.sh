#!/usr/bin/env bash
# Phase 34 smoke test: read-without-move.
#   1. vec_get_ref / hashmap_get_ref return a BORROW into the container (no
#      copy); `*r` derefs a scalar reference.
#   2. match THROUGH a reference (`match &e`) binds payloads as borrows, so the
#      scrutinee is not moved — it can be matched again (no UAF).
#   3. A 1,000,000-iteration loop that builds a heap-String-carrying enum,
#      matches it BY REFERENCE, and drops it runs in constant memory (the
#      by-ref read copies nothing; the enum's String payload is dropped once).
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

check() { # name file jit-out aot-exit
    local n=$1 f=$2 w=$3 rc=$4 jit
    jit=$("$KARDC" "$f")
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    [[ "$r" -ne "$rc" ]] && { echo "FAIL [$n/aot]: exit $r expected $rc"; exit 1; }
    echo "PASS [$n]: JIT + AOT"
}

# --- 1. borrow reads: vec_get_ref (String + scalar/deref) + hashmap_get_ref ---
cat > "$TMP/read.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, int_to_string(100));
    let a = str_len(vec_get_ref(&v, 0));          // &String borrow -> 3
    let mut nums = vec_new();
    vec_push(&mut nums, 40);
    let b = *vec_get_ref(&nums, 0);               // &i64 -> deref -> 40
    let mut m = hashmap_new();
    hashmap_insert(&mut m, "k", int_to_string(99999));
    let c = match hashmap_get_ref(&m, "k") { Some(p) => str_len(p), None => 0 }; // 5
    a + b + c                                       // 48
}
EOF
check read "$TMP/read.kd" 48 48

# --- 2. match through &Enum: bind payloads as borrows; match the same e twice. ---
cat > "$TMP/refmatch.kd" <<'EOF'
enum E { A(i64), B(String), N }
fn describe(e: &E) -> i64 ! { alloc } {
    match e { A(n) => *n, B(s) => str_len(s), N => 0 }
}
fn main() -> i64 ! { alloc } {
    let e1 = A(42);
    let e2 = B(int_to_string(12345));   // len 5
    describe(&e1) + describe(&e2) + describe(&e2)   // 42 + 5 + 5 = 52, e2 not moved
}
EOF
check refmatch "$TMP/refmatch.kd" 52 52

# --- 3. constant memory: build + match-by-ref + drop a heap-String enum in a loop ---
cat > "$TMP/loop.kd" <<'EOF'
enum E { B(String), N }
fn describe(e: &E) -> i64 ! { alloc } { match e { B(s) => str_len(s), N => 0 } }
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    let mut acc = 0;
    while i < 1000000 {
        let e = B(int_to_string(i));     // heap String payload
        acc = acc + describe(&e);        // matched BY REFERENCE (no copy)
        i = i + 1;                        // e dropped each iter -> String freed
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$(peak_rss_kb "$TMP/loop")
if [[ -z "$rss" ]]; then
    echo "SKIP [loop]: no GNU/BSD /usr/bin/time available for the RSS gate"
else
    echo "INFO [loop]: peak RSS over 1M build+match-by-ref+drop = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [loop]: RSS ${rss} KB > 32 MB — by-ref match copied/leaked the payload"; exit 1
    fi
    echo "PASS [loop]: match-by-ref + enum-String drop — RSS flat (<= 32 MB)"
fi

echo "PASS: Phase 34 — read-without-move (vec_get_ref / hashmap_get_ref / deref / match &Enum) works in JIT + AOT"
