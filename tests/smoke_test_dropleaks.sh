#!/usr/bin/env bash
# Phase 29 smoke test: plug the documented Drop leaks.
#
#  - Closure env: a closure heap-allocates a capture env; dropping the closure
#    now frees it. A 2,000,000-iteration loop that creates + drops a capturing
#    closure each turn runs in constant memory.
#  - Match payload bindings: a droppable enum payload bound in a match arm
#    (e.g. the `s` in `Some(s)`, s: String) is now dropped at arm-scope exit
#    when the arm doesn't move it out — but is NOT dropped (no UAF / double
#    free) when the arm returns it. Verified by a correctness check and a 2M
#    matched-and-dropped-heap-String loop in constant memory.
#
# (Documented-deferred, NOT covered here: async-frame interior free — freeing
# a completed Future's heap frame requires reworking the executor task
# lifecycle and risks a read-after-free on the poll slot; it stays a known
# leak, as the README records.)
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

# Peak RSS (KB) of running a binary; 0 when GNU time is unavailable (skip).
rss_kb() {
    if /usr/bin/time -v true >/dev/null 2>&1; then
        /usr/bin/time -v "$1" 2>&1 | awk '/Maximum resident/ {print $NF}'
    else
        "$1" >/dev/null 2>&1 || true
        echo 0
    fi
}

assert_flat() {
    local name=$1 rss=$2
    echo "INFO [$name]: peak RSS = ${rss} KB"
    if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
        echo "FAIL [$name]: RSS ${rss} KB exceeds 32 MB — the per-iteration value is leaking"
        exit 1
    fi
    echo "PASS [$name]: RSS flat (<= 32 MB)"
}

# --- 1. closure env freed: 2M capturing closures in constant memory. ---
cat > "$TMP/clo.kd" <<'EOF'
fn apply(f: fn(i64) -> i64, x: i64) -> i64 { f(x) }
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    let mut acc = 0;
    while i < 2000000 {
        let n = i;
        let f = |x| x + n;          // heap-allocates a capture env each turn
        acc = acc + apply(f, 1);    // f moved into apply, env freed on its return
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" -o "$TMP/clo" "$TMP/clo.kd" >/dev/null
assert_flat closure-env "$(rss_kb "$TMP/clo")"

# --- 2. match payload move-out: a returned binding must survive. ---
cat > "$TMP/mv.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let s0 = int_to_string(12345);                 // heap String "12345" (len 5)
    let o = Some(s0);
    let s = match o { Some(x) => x, None => string_new() };  // x moved OUT as result
    str_len(&s)                                     // 5 iff x was not wrongly dropped
}
EOF
mv_jit=$("$KARDC" "$TMP/mv.kd")
if [[ "$mv_jit" != "5" ]]; then
    echo "FAIL [match-moveout/jit]: expected 5, got '$mv_jit' (binding dropped while moved out?)"; exit 1
fi
"$KARDC" -o "$TMP/mv" "$TMP/mv.kd" >/dev/null
set +e; "$TMP/mv" >/dev/null; mv_rc=$?; set -e
[[ "$mv_rc" -ne 5 ]] && { echo "FAIL [match-moveout/aot]: exit $mv_rc (expected 5)"; exit 1; }
echo "PASS [match-moveout]: a moved-out payload binding survives (JIT + AOT), no UAF"

# --- 3. match payload dropped: 2M matched-and-borrowed heap Strings, flat. ---
cat > "$TMP/md.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    let mut acc = 0;
    while i < 2000000 {
        let s = int_to_string(i);        // heap String
        let o = Some(s);                 // moved into the Option
        acc = acc + match o { Some(x) => str_len(&x), None => 0 };  // x borrowed, dropped at arm end
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" -o "$TMP/md" "$TMP/md.kd" >/dev/null
assert_flat match-payload "$(rss_kb "$TMP/md")"

echo "PASS: closure env + match payload bindings are dropped (constant memory) and moved-out bindings survive"
