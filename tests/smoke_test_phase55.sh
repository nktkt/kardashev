#!/usr/bin/env bash
# Phase 55 smoke test: HashMap entry iteration -> Vec<(K, V)>.
#   1. hashmap_entries<K,V>(&HashMap<K,V>) -> Vec<(K,V)> gives each entry as a
#      deep-cloned (key, value) tuple. A HashMap<String,i64> is turned into a
#      Vec<Count> (a #[derive(Ord, Eq)] wrapper) and SORTED, so the map is
#      ranked without manual key lookups.
#   2. A constant-memory gate: a 200k entries+rank+drop loop holds peak RSS flat
#      (the entry tuples + their String keys drop each round).
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

# Portable peak-RSS (KB): GNU `time -v` (Linux) or BSD `time -l` (macOS); empty
# if neither exists (caller SKIPs the gate). Safe under `set -e`/`pipefail`.
peak_rss_kb() {
    local f; f=$(mktemp)
    if /usr/bin/time -v true >/dev/null 2>&1; then
        { /usr/bin/time -v "$@" >/dev/null; } 2>"$f" || true
        awk '/Maximum resident set size/ {print $NF}' "$f"
    elif /usr/bin/time -l true >/dev/null 2>&1; then
        { /usr/bin/time -l "$@" >/dev/null; } 2>"$f" || true
        awk '/maximum resident set size/ {print int($1/1024)}' "$f"
    fi
    rm -f "$f"
}

# --- 1. entries -> Vec<(K,V)> -> ranked via derived Ord ---
cat > "$TMP/ent.kd" <<'EOF'
#[derive(Ord, Eq, Clone)]
struct Count { n: i64, word: String }
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, int_to_string(1), 5);
    hashmap_insert(&mut m, int_to_string(2), 9);
    hashmap_insert(&mut m, int_to_string(3), 1);
    let es = hashmap_entries(&m);                  // Vec<(String, i64)>
    let mut cs = vec_new();
    let mut i = 0;
    while i < vec_len(&es) {
        let e = vec_get_ref(&es, i);
        vec_push(&mut cs, Count { n: e.1, word: e.0.clone() });
        i = i + 1;
    }
    sort(&mut cs);                                 // ascending by (n, word)
    let lo = vec_get_ref(&cs, 0).n;                // 1
    let hi = vec_get_ref(&cs, 2).n;                // 9
    vec_len(&es) * 1000 + lo * 100 + hi            // 3109
}
EOF
jit=$("$KARDC" "$TMP/ent.kd" | tail -1)
[[ "$jit" == "3109" ]] || { echo "FAIL [ent/jit]: expected 3109 got $jit"; exit 1; }
"$KARDC" --no-cache -o "$TMP/ent" "$TMP/ent.kd" >/dev/null
set +e; "$TMP/ent" >/dev/null; rc=$?; set -e
exp=$((3109 % 256))
[[ "$rc" -ne "$exp" ]] && { echo "FAIL [ent/aot]: exit $rc expected $exp"; exit 1; }
echo "PASS [entries+rank]: JIT 3109, AOT exit $rc"

# --- 2. constant memory over an entries+rank loop ---
cat > "$TMP/loop.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    let mut k = 0;
    while k < 12 { hashmap_insert(&mut m, int_to_string(k), k * 2); k = k + 1; }
    let mut i = 0;
    let mut acc = 0;
    while i < 200000 {
        let es = hashmap_entries(&m);
        acc = acc + vec_len(&es);
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$(peak_rss_kb "$TMP/loop")
if [[ -z "$rss" ]]; then
    echo "SKIP [loop]: no GNU/BSD /usr/bin/time available for the RSS gate"
else
    echo "INFO [loop]: peak RSS over 200k entries+drop = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [loop]: RSS ${rss} KB > 32 MB — hashmap_entries leaks"; exit 1
    fi
    echo "PASS [loop]: hashmap_entries+drop — RSS flat (<= 32 MB)"
fi

echo "PASS: Phase 55 — HashMap entries -> Vec<(K,V)> (JIT + AOT)"
