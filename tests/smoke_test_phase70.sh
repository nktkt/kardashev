#!/usr/bin/env bash
# Phase 70 smoke test (Roadmap v12 — the real stdlib): Vec mutation + query —
# vec_pop / vec_remove / vec_insert / vec_reverse (built-ins) and vec_contains /
# vec_index_of (Eq-bounded prelude scans).
#   1. The mutators compose on an i64 Vec to a known witness; JIT + AOT.
#   2. vec_pop / vec_remove MOVE a non-Copy element out (len decremented, the
#      Vec no longer owns it) — no double-free over String elements (AOT clean).
#   3. vec_contains / vec_index_of find (or report absent: index -1).
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

# 1+3: the i64 mutator + query composite.
cat > "$TMP/v.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 10);
    vec_push(&mut v, 20);
    vec_push(&mut v, 30);
    vec_insert(&mut v, 1, 15);       // [10,15,20,30]
    let r = vec_remove(&mut v, 0);   // r=10, v=[15,20,30]
    vec_reverse(&mut v);             // [30,20,15]
    let p = vec_pop(&mut v);         // p=15, v=[30,20]
    let n30 = 30; let n99 = 99; let n20 = 20;
    let c1 = vec_contains(&v, &n30); // true
    let c2 = vec_contains(&v, &n99); // false
    let ix = vec_index_of(&v, &n20); // 1
    let mut w = r + p + vec_len(&v) * 100 + ix * 10;
    if c1 { w = w + 1000; } else {}
    if c2 { w = w + 5000; } else {}
    w   // 10 + 15 + 200 + 10 + 1000 = 1235
}
EOF
jit=$("$KARDC" "$TMP/v.kd" 2>/dev/null | tail -1)
[[ "$jit" == "1235" ]] || { echo "FAIL [vec/jit]: expected 1235 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/v" "$TMP/v.kd" >/dev/null 2>&1
set +e; "$TMP/v" >/dev/null; rc=$?; set -e
[[ "$rc" -eq $((1235 & 255)) ]] || { echo "FAIL [vec/aot]: exit $rc expected $((1235 & 255))"; exit 1; }
echo "PASS [vec-ops]: insert/remove/reverse/pop + contains/index_of -> 1235, JIT+AOT"

# 2: non-Copy String elements moved out by remove/pop — no double-free.
cat > "$TMP/vs.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    let a = "aa"; let b = "bbb"; let c = "cccc";
    vec_push(&mut v, clone(&a));
    vec_push(&mut v, clone(&b));
    vec_push(&mut v, clone(&c));
    let x = vec_remove(&mut v, 1);   // moves out "bbb"; v=["aa","cccc"]
    let y = vec_pop(&mut v);         // moves out "cccc"; v=["aa"]
    str_len(&x) + str_len(&y) + vec_len(&v)   // 3 + 4 + 1 = 8
}
EOF
jit=$("$KARDC" "$TMP/vs.kd" 2>/dev/null | tail -1)
[[ "$jit" == "8" ]] || { echo "FAIL [vec-string/jit]: expected 8 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/vs" "$TMP/vs.kd" >/dev/null 2>&1
set +e; "$TMP/vs" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 8 ]] || { echo "FAIL [vec-string/aot]: exit $rc expected 8 (double-free?)"; exit 1; }
echo "PASS [vec-string-move]: remove/pop MOVE non-Copy elements, no double-free, JIT+AOT 8"

# index-absent returns -1.
cat > "$TMP/idx.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 1); vec_push(&mut v, 2);
    let n = 9;
    vec_index_of(&v, &n)   // -1
}
EOF
[[ "$("$KARDC" "$TMP/idx.kd" 2>/dev/null | tail -1)" == "-1" ]] || { echo "FAIL [idx-absent]: expected -1"; exit 1; }
echo "PASS [idx-absent]: vec_index_of of an absent element is -1"

echo "ALL PHASE 70 SMOKE TESTS PASSED"
