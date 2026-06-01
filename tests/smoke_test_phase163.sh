#!/usr/bin/env bash
# v30 Phase 163: the C backend (`--emit-c`) grows to scalar-element `Vec`
# (Vec<i64> / Vec<bool>, both int64_t elements). A `struct kdvec { int64_t*
# data; int64_t len; int64_t cap; }` runtime is emitted (only when the program
# uses Vec), mirroring the LLVM Vec builtins: vec_new / vec_push (double-grow
# from 4) / vec_get (OOB -> 0) / vec_get_ref (OOB -> null) / vec_len / vec_pop /
# vec_remove / vec_insert / vec_reverse / vec_swap. A Vec with a non-scalar
# element (Vec<struct>/Vec<String>) is REFUSED (a later piece monomorphizes per
# element type). Differentially gated: LLVM-AOT exit == emitted-C exit. Skips
# cleanly with no C compiler.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
CC_BIN="$(command -v cc || command -v gcc || command -v clang || true)"
[[ -z "$CC_BIN" ]] && { echo "SKIP: no C compiler"; exit 0; }
echo "Using kardc at: $KARDC ; cc: $CC_BIN"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    local lo; lo=$("$TMP/llvm" 2>&1); local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused in-subset: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C:"; head -5 "$TMP/cc"; exit 1; }
    local co; co=$("$TMP/cbin" 2>&1); local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    [[ "$lo" == "$co" ]] || { echo "FAIL [$name]: stdout differs; LLVM=[$lo] C=[$co]"; exit 1; }
    echo "PASS [$name]: LLVM == C (exit $lrc)"; }

# 1) push / get / len
cat > "$TMP/a.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 10); vec_push(&mut v, 20); vec_push(&mut v, 30);
    vec_get(&v, 0) + vec_get(&v, 1) + vec_get(&v, 2) + vec_len(&v)
}
EOF
diff_ok push_get_len "$TMP/a.kd"

# 2) Vec as a fn param (&Vec<i64>) + a while-loop sum
cat > "$TMP/b.kd" <<'EOF'
fn sum(v: &Vec<i64>) -> i64 { let mut s = 0; let mut i = 0; while i < vec_len(v) { s = s + vec_get(v, i); i = i + 1; } s }
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 1); vec_push(&mut v, 2); vec_push(&mut v, 3); vec_push(&mut v, 4);
    sum(&v)
}
EOF
diff_ok vec_param "$TMP/b.kd"

# 3) pop / get_ref (deref) / swap. The get_ref borrow is read+finished BEFORE
#    any &mut mutation (borrow-check requires the shared borrow not outlive a
#    later mutable borrow).
cat > "$TMP/c.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 5); vec_push(&mut v, 6); vec_push(&mut v, 7);
    vec_swap(&mut v, 0, 2);
    let first = (*vec_get_ref(&v, 0));
    let p = vec_pop(&mut v);
    first * 100 + p * 10 + vec_len(&v)
}
EOF
diff_ok pop_ref_swap "$TMP/c.kd"

# 4) insert / remove / reverse
cat > "$TMP/d.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 1); vec_push(&mut v, 3);
    vec_insert(&mut v, 1, 2);
    let r = vec_remove(&mut v, 0);
    vec_reverse(&mut v);
    r * 100 + vec_get(&v, 0) * 10 + vec_get(&v, 1)
}
EOF
diff_ok insert_remove_reverse "$TMP/d.kd"

# 5) Vec<bool> (a bool element drives an `if` directly)
cat > "$TMP/e.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, true); vec_push(&mut v, false); vec_push(&mut v, true);
    let mut cnt = 0; let mut i = 0;
    while i < vec_len(&v) { if vec_get(&v, i) { cnt = cnt + 1; } else { } i = i + 1; }
    cnt
}
EOF
diff_ok vec_bool "$TMP/e.kd"

# 6) a Vec with a non-scalar element is refused
cat > "$TMP/vs.kd" <<'EOF'
struct P { x: i64 }
fn main() -> i64 ! { alloc } { let mut v = vec_new(); vec_push(&mut v, P { x: 5 }); 0 }
EOF
out=$("$KARDC" --emit-c "$TMP/vs.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [vec_struct_refused]: a Vec<struct> should be refused"; exit 1; }
echo "$out" | grep -qi "non-scalar element\|outside the C-backend subset" || { echo "FAIL [vec_struct_refused]: missing diagnostic: $out"; exit 1; }
echo "PASS [vec_struct_refused]: a Vec with a non-scalar element is refused"

# 7) an unimplemented builtin (hashmap) is still refused (Phase 163 part 1)
cat > "$TMP/hm.kd" <<'EOF'
fn main() -> i64 ! { alloc } { let mut m = hashmap_new(); 0 }
EOF
out=$("$KARDC" --emit-c "$TMP/hm.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [hashmap_refused]: hashmap_new should be refused"; exit 1; }
echo "PASS [hashmap_refused]: an unimplemented builtin (hashmap_new) is refused"

echo "PASS: Phase 163 — C backend scalar-element Vec (differentially gated vs LLVM)"
