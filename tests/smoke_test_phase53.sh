#!/usr/bin/env bash
# Phase 53 smoke test: generic Vec higher-order combinators over closures.
#   1. vec_map<T,U> maps Vec<i64> -> Vec<String>; vec_filter<T> keeps the evens;
#      vec_fold<T,A> sums — each driven by a closure (param types annotated;
#      the closure receives `&T`). All are effect-polymorphic in the closure's
#      effect row.
#   2. A constant-memory gate: a 200k-iteration map+filter+fold+drop loop holds
#      peak RSS flat (the fresh Vecs + cloned String elements drop each round).
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

# --- 1. map / filter / fold correctness ---
cat > "$TMP/comb.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 1); vec_push(&mut v, 2); vec_push(&mut v, 3); vec_push(&mut v, 4);
    let strs = vec_map(&v, |x: &i64| int_to_string(*x * 10));         // ["10".."40"]
    let lensum = vec_fold(&strs, 0, |a: i64, s: &String| a + str_len(s)); // 8
    let evens = vec_filter(&v, |x: &i64| *x % 2 == 0);                // [2,4]
    let ecount = vec_len(&evens);                                     // 2
    let total = vec_fold(&v, 0, |a: i64, x: &i64| a + *x);            // 10
    lensum * 1000 + ecount * 100 + total                             // 8210
}
EOF
jit=$("$KARDC" "$TMP/comb.kd" | tail -1)
[[ "$jit" == "8210" ]] || { echo "FAIL [comb/jit]: expected 8210 got $jit"; exit 1; }
"$KARDC" --no-cache -o "$TMP/comb" "$TMP/comb.kd" >/dev/null
set +e; "$TMP/comb" >/dev/null; rc=$?; set -e
exp=$((8210 % 256))
[[ "$rc" -ne "$exp" ]] && { echo "FAIL [comb/aot]: exit $rc expected $exp"; exit 1; }
echo "PASS [map/filter/fold]: JIT 8210, AOT exit $rc"

# --- 2. constant memory over a combinator loop ---
cat > "$TMP/loop.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut base = vec_new();
    let mut k = 0;
    while k < 16 { vec_push(&mut base, k); k = k + 1; }
    let mut i = 0;
    let mut acc = 0;
    while i < 200000 {
        let strs = vec_map(&base, |x: &i64| int_to_string(*x));
        let kept = vec_filter(&base, |x: &i64| *x % 2 == 0);
        let s = vec_fold(&strs, 0, |a: i64, t: &String| a + str_len(t));
        acc = acc + s + vec_len(&kept);
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$( /usr/bin/time -v "$TMP/loop" 2>&1 | awk '/Maximum resident/ {print $NF}' )
echo "INFO [loop]: peak RSS over 200k map+filter+fold+drop = ${rss} KB"
if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
    echo "FAIL [loop]: RSS ${rss} KB > 32 MB — a combinator leaks"; exit 1
fi
echo "PASS [loop]: combinator pipeline — RSS flat (<= 32 MB)"

echo "PASS: Phase 53 — Vec map/filter/fold combinators (JIT + AOT)"
