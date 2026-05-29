#!/usr/bin/env bash
# Phase 48 smoke test: extend #[derive] to Hash, Ord, and Default.
#   1. #[derive(Hash, Eq)] -> the type is usable as a HashMap KEY (the derived
#      hash combines field hashes; the derived eq compares field-wise). Proven
#      for a struct key and via lookups.
#   2. #[derive(Ord)] -> the type SORTS (sort<T: Ord>). Proven for a struct
#      (lexicographic fields) AND an enum (variant-ordinal then payload).
#   3. #[derive(Default)] -> the type constructs via the STATIC associated
#      method Type::default() (a new associated-function capability: no-self
#      trait methods + `Type::method()` call resolution). Field-wise defaults,
#      including a nested user type and a Vec field.
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

run() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f" | tail -1)
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT exit $r"
}

# --- 1+2. derived Hash key + derived Ord (struct + enum) ---
cat > "$TMP/hashord.kd" <<'EOF'
#[derive(Hash, Eq, Clone)]
struct Key { region: String, id: i64 }

#[derive(Ord, Clone)]
struct Score { pts: i64, name: String }

#[derive(Hash, Ord, Eq)]
enum Tag { Low, Mid(i64), High(i64, i64) }

fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, Key { region: "us", id: 1 }, 100);
    hashmap_insert(&mut m, Key { region: "eu", id: 2 }, 200);
    let look = match hashmap_get(&m, Key { region: "eu", id: 2 }) { Some(v) => v, None => 0 };
    let n = hashmap_len(&m);

    let mut v = vec_new();
    vec_push(&mut v, Score { pts: 30, name: "c" });
    vec_push(&mut v, Score { pts: 10, name: "a" });
    vec_push(&mut v, Score { pts: 20, name: "b" });
    sort(&mut v);
    let s0 = vec_get_ref(&v, 0).pts;
    let s2 = vec_get_ref(&v, 2).pts;

    let mut t = vec_new();
    vec_push(&mut t, High(1, 2));
    vec_push(&mut t, Low);
    vec_push(&mut t, Mid(5));
    sort(&mut t);                                  // Low < Mid(_) < High(_,_)
    let tord = match vec_get_ref(&t, 0) { Low => 0, Mid(a) => 1, High(a, b) => 2 };

    look + n * 10 + s0 * 100 + s2 * 1000 + tord    // 200+20+1000+30000+0 = 31220
}
EOF
run hashord "$TMP/hashord.kd" 31220

# --- 3. derived Default via Type::default() (static associated method) ---
cat > "$TMP/default.kd" <<'EOF'
#[derive(Default)]
struct Inner { count: i64, label: String }
#[derive(Default)]
struct Config { name: String, retries: i64, flags: Vec<i64>, inner: Inner }
fn main() -> i64 ! { alloc } {
    let c = Config::default();
    let r = str_len(&c.name) + c.retries + vec_len(&c.flags)
          + c.inner.count + str_len(&c.inner.label);   // all default => 0
    let i = Inner::default();
    r + i.count + str_len(&i.label) + 42                // 42
}
EOF
run default "$TMP/default.kd" 42

# --- negative: an unknown static method on a real type is still an error ---
cat > "$TMP/neg.kd" <<'EOF'
struct P { x: i64 }
fn main() -> i64 { P::nonexistent() }
EOF
if "$KARDC" "$TMP/neg.kd" >/dev/null 2>&1; then
    echo "FAIL [neg]: P::nonexistent() should not resolve"; exit 1
fi
echo "PASS [neg]: unknown static method rejected"

echo "PASS: Phase 48 — #[derive(Hash, Ord, Default)] + associated functions (JIT + AOT)"
