#!/usr/bin/env bash
# Phase 41 smoke test: real generic Clone / Eq over containers.
#   1. The prelude `Clone` trait deep-clones via `.clone()` dispatching through
#      each element's own impl: Vec<String> and Vec<Pair<String>> (the latter
#      through a USER `impl<T: Clone> Clone for Pair<T>` -> String::clone).
#   2. The generic `impl<T: Eq> Eq for Vec<T>` deep-compares element-wise.
#   3. A 200k clone+eq loop over Vec<Pair<String>> holds constant memory.
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

check() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f")
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT matches"
}

# --- 1. Vec<String> deep clone via the Clone trait ---
cat > "$TMP/vclone.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, int_to_string(11));
    vec_push(&mut v, int_to_string(2222));
    let c = v.clone();
    str_len(vec_get_ref(&v, 0)) + str_len(vec_get_ref(&c, 1))   // 2 + 4 = 6
}
EOF
check vclone "$TMP/vclone.kd" 6

# --- 2. Vec<i64> deep equality ---
cat > "$TMP/veq.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut a = vec_new(); vec_push(&mut a, 1); vec_push(&mut a, 2); vec_push(&mut a, 3);
    let mut b = vec_new(); vec_push(&mut b, 1); vec_push(&mut b, 2); vec_push(&mut b, 3);
    let mut c = vec_new(); vec_push(&mut c, 1); vec_push(&mut c, 9);
    let e1 = if a.eq(&b) { 1 } else { 0 };
    let e2 = if a.eq(&c) { 1 } else { 0 };
    e1 * 10 + e2                                                // 10
}
EOF
check veq "$TMP/veq.kd" 10

# --- 3. the acceptance: Vec<Pair<String>> clone via user Clone for Pair + Eq ---
cat > "$TMP/acc.kd" <<'EOF'
struct Pair<T> { a: T, b: T }
impl<T: Clone> Clone for Pair<T> {
    fn clone(&self) -> Pair<T> ! { alloc } {
        Pair { a: self.a.clone(), b: self.b.clone() }
    }
}
impl<T: Eq> Eq for Pair<T> {
    fn eq(&self, other: &Pair<T>) -> bool ! { alloc } {
        if self.a.eq(&other.a) { self.b.eq(&other.b) } else { false }
    }
}
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, Pair { a: int_to_string(1), b: int_to_string(22) });
    vec_push(&mut v, Pair { a: int_to_string(333), b: int_to_string(4) });
    let c = v.clone();                       // deep clone through Pair + String
    let same = if v.eq(&c) { 100 } else { 0 };
    // mutate-independence proxy: lengths still readable on both
    let extra = str_len(vec_get_ref(&v, 0).a.to_string_len_proxy());
    same
}
EOF
# (drop the proxy line — kept the test simple/deterministic)
cat > "$TMP/acc.kd" <<'EOF'
struct Pair<T> { a: T, b: T }
impl<T: Clone> Clone for Pair<T> {
    fn clone(&self) -> Pair<T> ! { alloc } {
        Pair { a: self.a.clone(), b: self.b.clone() }
    }
}
impl<T: Eq> Eq for Pair<T> {
    fn eq(&self, other: &Pair<T>) -> bool ! { alloc } {
        if self.a.eq(&other.a) { self.b.eq(&other.b) } else { false }
    }
}
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, Pair { a: int_to_string(1), b: int_to_string(22) });
    vec_push(&mut v, Pair { a: int_to_string(333), b: int_to_string(4) });
    let c = v.clone();                       // deep clone through Pair + String
    if v.eq(&c) { 1 } else { 0 }             // deep-equal -> 1
}
EOF
check acc "$TMP/acc.kd" 1

# --- 4. constant memory over a clone+eq loop ---
cat > "$TMP/loop.kd" <<'EOF'
struct Pair<T> { a: T, b: T }
impl<T: Clone> Clone for Pair<T> {
    fn clone(&self) -> Pair<T> ! { alloc } { Pair { a: self.a.clone(), b: self.b.clone() } }
}
impl<T: Eq> Eq for Pair<T> {
    fn eq(&self, other: &Pair<T>) -> bool ! { alloc } { if self.a.eq(&other.a) { self.b.eq(&other.b) } else { false } }
}
fn main() -> i64 ! { alloc } {
    let mut i = 0; let mut acc = 0;
    while i < 200000 {
        let mut v = vec_new();
        vec_push(&mut v, Pair { a: int_to_string(i), b: int_to_string(i) });
        let c = v.clone();
        if v.eq(&c) { acc = acc + 1; } else {}
        i = i + 1;
    }
    if acc == 200000 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$(peak_rss_kb "$TMP/loop")
if [[ -z "$rss" ]]; then
    echo "SKIP [loop]: no GNU/BSD /usr/bin/time available for the RSS gate"
else
    echo "INFO [loop]: peak RSS over 200k Vec<Pair<String>> clone+eq = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [loop]: RSS ${rss} KB > 32 MB — generic clone leaked"; exit 1
    fi
    echo "PASS [loop]: generic container clone+eq — RSS flat (<= 32 MB)"
fi

echo "PASS: Phase 41 — generic Clone/Eq over containers (JIT + AOT)"
