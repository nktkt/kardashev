#!/usr/bin/env bash
# Phase 45 smoke test: bounded type params (`K: Hash + Eq`) satisfy HashMap's
# key requirement from INSIDE a generic body. Before v8 the key-hashable gate
# only accepted a concrete i64/String/derived type, so a generic fn/impl could
# not name or build a `HashMap<K,V>`.
#   1. A hand-written `fn count_keys<K: Hash + Eq, V>(m: &HashMap<K,V>)` compiles
#      and runs for BOTH HashMap<String,i64> and HashMap<i64,i64> (one generic
#      fn, two key-type monomorphizations).
#   2. A generic-impl method (`impl<K: Hash + Eq> Pair<K>`) whose body BUILDS and
#      QUERIES a `HashMap<K,i64>` with K abstract — K inferred from the receiver
#      — for Pair<String> AND Pair<i64>.
#   3. The bound is enforced: a `count_keys<K, V>` with NO Hash+Eq on K is
#      rejected at typecheck (the `&HashMap<K,V>` param fails the key gate).
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

# --- 1+2. positive: free fn + generic-impl method over HashMap<K,V> ---
cat > "$TMP/p45.kd" <<'EOF'
fn count_keys<K: Hash + Eq, V>(m: &HashMap<K, V>) -> i64 ! {} {
    hashmap_len(m)
}

struct Pair<K> { a: K, b: K }
impl<K: Hash + Eq> Pair<K> {
    fn map_size(self) -> i64 ! { alloc } {
        let mut m = hashmap_new();
        hashmap_insert(&mut m, self.a, 1);
        hashmap_insert(&mut m, self.b, 2);
        hashmap_len(&m)
    }
}

fn main() -> i64 ! { io, alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, "alpha", 10);
    hashmap_insert(&mut m, "beta", 20);
    hashmap_insert(&mut m, "gamma", 30);
    let k = count_keys(&m);              // K = String, V = i64

    let mut mi = hashmap_new();
    hashmap_insert(&mut mi, 7, 700);
    let ki = count_keys(&mi);            // K = i64, V = i64 — same generic fn

    let p = Pair { a: "p", b: "q" };
    let ps = p.map_size();               // impl-level K = String (from receiver)

    let p2 = Pair { a: 100, b: 200 };
    let ps2 = p2.map_size();             // impl-level K = i64 (second instance)

    let msg = "phase45: ok"; println(&msg);
    k * 1000 + ki * 100 + ps * 10 + ps2  // 3*1000 + 1*100 + 2*10 + 2 = 3122
}
EOF

jit=$("$KARDC" "$TMP/p45.kd")
echo "$jit"
echo "$jit" | grep -q "phase45: ok" || { echo "FAIL: missing ok line"; exit 1; }
sig=$(echo "$jit" | tail -1)
[[ "$sig" == "3122" ]] || { echo "FAIL [jit]: expected 3122 got $sig"; exit 1; }
"$KARDC" --no-cache -o "$TMP/p45" "$TMP/p45.kd" >/dev/null
set +e; "$TMP/p45" >/dev/null; rc=$?; set -e
exp=$((3122 % 256))
[[ "$rc" -ne "$exp" ]] && { echo "FAIL [aot]: exit $rc expected $exp"; exit 1; }
echo "PASS [bounded-key container ops]: JIT 3122, AOT exit $rc"

# --- 3. negative: an UNBOUNDED key param must be rejected ---
cat > "$TMP/neg.kd" <<'EOF'
fn count_keys<K, V>(m: &HashMap<K, V>) -> i64 ! {} { hashmap_len(m) }
fn main() -> i64 { 0 }
EOF
if "$KARDC" "$TMP/neg.kd" >/dev/null 2>&1; then
    echo "FAIL [neg]: unbounded HashMap key param should be rejected"; exit 1
fi
echo "PASS [neg]: unbounded-key HashMap param rejected at typecheck"

echo "PASS: Phase 45 — bounded type params in container ops (JIT + AOT)"
