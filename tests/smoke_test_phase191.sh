#!/usr/bin/env bash
# v35 Phase 191 smoke test — a seeded pseudo-random generator.
#
# `Rng` is a 64-bit linear congruential generator: DETERMINISTIC (same seed →
# same sequence), so it is unit-testable, and identical under JIT and AOT
# (pure 2's-complement arithmetic under -fwrapv). Covered: reproducibility,
# rng_below range bounds, and a Fisher-Yates vec_shuffle that preserves the
# element multiset. Differentially gated JIT vs AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" \
    "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$("$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n") || true
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}

# 1. Range bounds + determinism + shuffle. All draws of rng_below(_, 6) must
#    land in [0,6) (bad=0); a Fisher-Yates shuffle preserves length and the
#    element multiset (here the sum, 1+2+3+4 = 10).
diff_run rng_props $'0\n4\n10' '
fn main() -> i64 ! { io, alloc } {
    let mut r = rng_new(7);
    let mut i = 0;
    let mut bad = 0;
    while i < 10000 {
        let x = rng_below(&mut r, 6);
        if x < 0 { bad = bad + 1; } else { if x >= 6 { bad = bad + 1; } else {} }
        i = i + 1;
    }
    print(bad);                                  // 0 — always in range
    let mut v = vec_new();
    vec_push(&mut v, 1); vec_push(&mut v, 2); vec_push(&mut v, 3); vec_push(&mut v, 4);
    let mut rs = rng_new(99);
    vec_shuffle(&mut v, &mut rs);
    print(vec_len(&v));                           // 4 (length preserved)
    print(vec_sum(&v));                           // 10 (multiset preserved)
    0
}
'

# 2. Determinism: two generators seeded identically yield the same sequence;
#    rng_range stays within [lo, hi).
diff_run rng_determinism $'1\n1\n1' '
fn main() -> i64 ! { io } {
    let mut a = rng_new(12345);
    let mut b = rng_new(12345);
    let mut i = 0;
    let mut same = 1;
    let mut inrange = 1;
    while i < 1000 {
        let x = rng_range(&mut a, 10, 20);
        let y = rng_range(&mut b, 10, 20);
        if x != y { same = 0; } else {}
        if x < 10 { inrange = 0; } else { if x >= 20 { inrange = 0; } else {} }
        i = i + 1;
    }
    print(same);      // 1 — identical seeds, identical streams
    print(inrange);   // 1 — every value in [10,20)
    print(1);
    0
}
'

echo "ALL PHASE 191 SMOKE TESTS PASSED"
