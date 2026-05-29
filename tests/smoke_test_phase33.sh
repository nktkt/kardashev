#!/usr/bin/env bash
# Phase 33 smoke test:
#   1. The two firsthand-missing operators — `%` integer modulo and `&&`
#      short-circuit logical-and (the rhs must NOT evaluate when lhs is false).
#   2. HashMap/HashSet interior drop + rehash-buffer reclaim: a map built with
#      droppable (heap String) keys, rehashed several times, then dropped in a
#      large loop runs in constant memory — no leaked keys, no leaked old
#      bucket buffers.
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

rss_kb() {
    if /usr/bin/time -v true >/dev/null 2>&1; then
        /usr/bin/time -v "$1" 2>&1 | awk '/Maximum resident/ {print $NF}'
    else
        # No GNU `/usr/bin/time -v`: cannot measure RSS, so the leak gate cannot
        # run. FAIL LOUDLY rather than echo 0 (a silent false-green).
        echo "FAIL: GNU /usr/bin/time -v unavailable; cannot run the memory gate" >&2
        exit 1
    fi
}

# --- 1. operators: %, &&, and short-circuit (10/0 in a dead && rhs must not trap) ---
cat > "$TMP/ops.kd" <<'EOF'
fn main() -> i64 {
    let a = 17 % 5;                                   // 2
    let t = true;
    let f = false;
    let r1 = if (a == 2) && t { 10 } else { 0 };      // 10
    let r2 = if t && f { 100 } else { 0 };            // 0
    let r3 = if f && ((10 / 0) == 0) { 1000 } else { 5 };  // 5 (rhs short-circuited)
    a + r1 + r2 + r3 + (100 % 7)                        // 2+10+0+5+2 = 19
}
EOF
jit=$("$KARDC" "$TMP/ops.kd")
[[ "$jit" != "19" ]] && { echo "FAIL [ops/jit]: expected 19, got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/ops" "$TMP/ops.kd" >/dev/null
set +e; "$TMP/ops" >/dev/null; rc=$?; set -e
[[ "$rc" -ne 19 ]] && { echo "FAIL [ops/aot]: exit $rc (expected 19)"; exit 1; }
echo "PASS [ops]: % modulo + && short-circuit (dead rhs not evaluated) — JIT + AOT"

# --- 2. HashMap interior drop + rehash reclaim: constant memory ---
cat > "$TMP/hmdrop.kd" <<'EOF'
fn build(n: i64) -> i64 ! { alloc } {
    let mut m = hashmap_new();
    let mut i = 0;
    while i < n {
        let key = int_to_string(i);     // heap String key (droppable)
        hashmap_insert(&mut m, key, i);
        i = i + 1;
    }
    hashmap_len(&m)                       // m dropped at scope exit
}
fn main() -> i64 ! { alloc } {
    let mut iter = 0;
    let mut acc = 0;
    while iter < 500000 {
        acc = acc + build(10);            // 10 keys -> rehash; map+keys freed each iter
        iter = iter + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/hmdrop" "$TMP/hmdrop.kd" >/dev/null
rss=$(rss_kb "$TMP/hmdrop")
echo "INFO [hmdrop]: peak RSS over 500k maps × 10 heap-String keys = ${rss} KB"
if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
    echo "FAIL [hmdrop]: RSS ${rss} KB > 32 MB — interior keys or old bucket buffers are leaking"
    exit 1
fi
echo "PASS [hmdrop]: HashMap interior drop + rehash reclaim — RSS flat (<= 32 MB)"

echo "PASS: Phase 33 — % / && operators and HashMap interior drop + rehash reclaim work in JIT + AOT"
