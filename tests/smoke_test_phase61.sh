#!/usr/bin/env bash
# Phase 61 smoke test (Roadmap v10): RingBuffer<T, const CAP> — a struct generic
# over BOTH a type param and a const param, with Drop + deep clone over a
# NON-Copy element type, plus closure-param inference.
#   1. A fixed-size array over a non-Copy element (`[String; N]`) deep-clones and
#      drops element-wise. (Lifted the old Copy-only array restriction.)
#   2. `#[derive(Clone)] struct RingBuffer<T, const CAP>` clones over non-Copy T
#      (symbolic const params flow through the generic impl); distinct CAP →
#      distinct LLVM type. JIT + AOT.
#   3. `RingBuffer<String, N>` drops its element Strings at scope exit (AOT runs
#      clean — element drop works).
#   4. Closure-param inference: `vec_map(v, |x| ..)` needs no annotation on `x`.
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

run_jit_aot() { # name file expected
    local n=$1 f=$2 want=$3 jit rc
    jit=$("$KARDC" "$f" 2>/dev/null | tail -1)
    [[ "$jit" == "$want" ]] || { echo "FAIL [$n/jit]: expected $want got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null 2>&1
    set +e; "$TMP/$n" >/dev/null; rc=$?; set -e
    [[ "$rc" -eq "$want" ]] || { echo "FAIL [$n/aot]: exit $rc expected $want"; exit 1; }
    echo "PASS [$n]: JIT $want, AOT exit $want"
}

# 1. non-Copy fixed array: deep clone + element drop.
cat > "$TMP/arr.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let a: [String; 2] = [int_to_string(11), int_to_string(222)];
    let b = clone(&a);
    str_len(&b[0]) + str_len(&b[1]) + str_len(&a[0])
}
EOF
run_jit_aot noncopy-array "$TMP/arr.kd" 7

# 2. RingBuffer<T, const CAP> derive(Clone) over non-Copy T.
cat > "$TMP/rb.kd" <<'EOF'
#[derive(Clone)]
struct RingBuffer<T, const CAP: i64> { data: [T; CAP], len: i64 }
fn main() -> i64 ! { alloc } {
    let rb: RingBuffer<String, 2> = RingBuffer { data: [int_to_string(11), int_to_string(222)], len: 2 };
    let rb2 = rb.clone();
    str_len(&rb2.data[0]) + str_len(&rb2.data[1]) + rb2.len + str_len(&rb.data[0])
}
EOF
run_jit_aot ringbuffer-clone "$TMP/rb.kd" 9
ll=$("$KARDC" --emit-llvm "$TMP/rb.kd" 2>/dev/null)
grep -q "%RingBuffer__String_c2 = type { \[2 x %String\], i64 }" <<<"$ll" || {
    echo "FAIL [rb/llvm]: missing %RingBuffer__String_c2 = { [2 x %String], i64 }"; exit 1; }
echo "PASS [ringbuffer-llvm]: RingBuffer<String,2> -> [2 x %String] (const CAP monomorphized)"

# 3. distinct CAP -> distinct monomorph (Mat<3>/Mat<5> already in phase58; here
#    a mixed type+const struct used at two sizes is two LLVM types).
cat > "$TMP/two.kd" <<'EOF'
struct Buf<T, const N: i64> { data: [T; N] }
fn main() -> i64 ! { alloc } {
    let a: Buf<i64, 2> = Buf { data: [10, 20] };
    let b: Buf<i64, 3> = Buf { data: [1, 2, 3] };
    a.data[0] + b.data[2]
}
EOF
run_jit_aot mixed-two-sizes "$TMP/two.kd" 13

# 4. closure-param inference: no annotation on `x`.
cat > "$TMP/clo.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 3);
    vec_push(&mut v, 4);
    let w = vec_map(&v, |x| *x * 10);
    vec_get(&w, 0) + vec_get(&w, 1)
}
EOF
run_jit_aot closure-infer "$TMP/clo.kd" 70

echo "ALL PHASE 61 SMOKE TESTS PASSED"
