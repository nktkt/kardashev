#!/usr/bin/env bash
# Phase 49 smoke test: generic trait objects `dyn Trait<T>` + dynamic dispatch
# over a heterogeneous Vec<Box<dyn ...>> collection.
#   1. A PARAMETERIZED trait object `Box<dyn Producer<i64>>`: the trait's type
#      arg is carried on the Dyn type and bound when resolving the method
#      signature, so `produce(&self) -> T` returns i64. A Vec of them holds two
#      different impls dispatched at one call site (the Phase-21 gap, closed).
#   2. A NON-parameterized `Vec<Box<dyn Display>>`: dispatch THROUGH the
#      `&Box<dyn>` that vec_get_ref returns (previously "unsupported receiver").
#   3. Negative: a trait-arg arity mismatch (`dyn Producer` with no <i64>) is
#      rejected.
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

cat > "$TMP/p49.kd" <<'EOF'
trait Producer<T> { fn produce(&self) -> T; }
struct Const { v: i64 }
struct Doubler { base: i64 }
impl Producer<i64> for Const { fn produce(&self) -> i64 { self.v } }
impl Producer<i64> for Doubler { fn produce(&self) -> i64 { self.base * 2 } }

struct Lbl { n: i64 }
impl Display for Lbl { fn to_string(&self) -> String ! { alloc } { int_to_string(self.n) } }

fn main() -> i64 ! { io, alloc } {
    let mut ps: Vec<Box<dyn Producer<i64>>> = vec_new();
    vec_push(&mut ps, Box::new(Const { v: 10 }));
    vec_push(&mut ps, Box::new(Doubler { base: 20 }));   // 40
    vec_push(&mut ps, Box::new(Const { v: 3 }));
    let mut sum = 0;
    let mut i = 0;
    while i < vec_len(&ps) {
        sum = sum + vec_get_ref(&ps, i).produce();        // dynamic dispatch
        i = i + 1;
    }                                                     // 10+40+3 = 53

    let mut ds: Vec<Box<dyn Display>> = vec_new();
    vec_push(&mut ds, Box::new(Lbl { n: 7 }));
    vec_push(&mut ds, Box::new(Lbl { n: 88 }));
    let mut chars = 0;
    let mut j = 0;
    while j < vec_len(&ds) {
        let s = vec_get_ref(&ds, j).to_string();
        chars = chars + str_len(&s);
        j = j + 1;
    }                                                     // len("7")+len("88") = 3

    let msg = "phase49: ok"; println(&msg);
    sum * 100 + chars                                     // 5303
}
EOF

jit=$("$KARDC" "$TMP/p49.kd")
echo "$jit"
grep -q "phase49: ok" <<< "$jit" || { echo "FAIL: missing ok line"; exit 1; }
sig=$(echo "$jit" | tail -1)
[[ "$sig" == "5303" ]] || { echo "FAIL [jit]: expected 5303 got $sig"; exit 1; }
"$KARDC" --no-cache -o "$TMP/p49" "$TMP/p49.kd" >/dev/null
set +e; "$TMP/p49" >/dev/null; rc=$?; set -e
exp=$((5303 % 256))
[[ "$rc" -ne "$exp" ]] && { echo "FAIL [aot]: exit $rc expected $exp"; exit 1; }
echo "PASS [dyn Trait<T> + Vec<Box<dyn>>]: JIT 5303, AOT exit $rc"

# negative: trait-arg arity mismatch
cat > "$TMP/neg.kd" <<'EOF'
trait Producer<T> { fn produce(&self) -> T; }
struct C { v: i64 }
impl Producer<i64> for C { fn produce(&self) -> i64 { self.v } }
fn main() -> i64 ! { alloc } {
    let b: Box<dyn Producer> = Box::new(C { v: 1 });   // missing <i64>
    b.produce()
}
EOF
if "$KARDC" "$TMP/neg.kd" >/dev/null 2>&1; then
    echo "FAIL [neg]: `dyn Producer` without trait args should be rejected"; exit 1
fi
echo "PASS [neg]: dyn trait-arg arity mismatch rejected"

echo "PASS: Phase 49 — dyn Trait<T> generic trait objects (JIT + AOT)"
