#!/usr/bin/env bash
# Phase 42 smoke test: #[derive(Clone, Eq, Display)].
#   1. #[derive(Clone, Eq)] on a recursive enum (Json) clones + compares deeply
#      with NO hand-written impl.
#   2. #[derive(Display)] synthesizes the canonical text for a struct
#      ("P { x: 3, y: 4 }") and an enum ("C(7, true)").
#   3. #[derive(...)] on a GENERIC struct Pair<T> bounds each param by the
#      derived trait (Pair<String> clones/compares/formats).
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

# --- 1. derive Clone + Eq on a recursive enum (no hand-written impl) ---
cat > "$TMP/enum.kd" <<'EOF'
#[derive(Clone, Eq)]
enum Json { JNull, JInt(i64), JArr(Vec<Json>) }
fn main() -> i64 ! { alloc } {
    let mut a = vec_new();
    vec_push(&mut a, JInt(1));
    vec_push(&mut a, JNull);
    let inner = JArr(a);
    let mut b = vec_new();
    vec_push(&mut b, inner);
    vec_push(&mut b, JInt(9));
    let j = JArr(b);             // nested: [[1,null],9]
    let c = j.clone();           // derived deep clone
    if j.eq(&c) { 1 } else { 0 } // derived deep eq -> 1
}
EOF
check enum "$TMP/enum.kd" 1

# --- 2. derive Display on a struct + an enum (exact text) ---
cat > "$TMP/disp.kd" <<'EOF'
#[derive(Display)]
struct P { x: i64, y: i64 }
#[derive(Display)]
enum E { A, B(i64), C(i64, bool) }
fn main() -> i64 ! { io, alloc } {
    let p = P { x: 3, y: 4 };
    let s = p.to_string();
    println(&s);                 // "P { x: 3, y: 4 }"
    let e = C(7, true);
    let es = e.to_string();
    println(&es);                // "C(7, true)"
    str_len(&s) + str_len(&es)   // 16 + 10 = 26
}
EOF
out=$("$KARDC" "$TMP/disp.kd")
echo "$out" | grep -qx "P { x: 3, y: 4 }" || { echo "FAIL [disp]: struct text"; echo "$out"; exit 1; }
echo "$out" | grep -qx "C(7, true)" || { echo "FAIL [disp]: enum text"; echo "$out"; exit 1; }
echo "$out" | tail -1 | grep -qx "26" || { echo "FAIL [disp]: signal not 26"; exit 1; }
"$KARDC" --no-cache -o "$TMP/disp" "$TMP/disp.kd" >/dev/null
set +e; "$TMP/disp" >/dev/null; dr=$?; set -e
[[ "$dr" -ne 26 ]] && { echo "FAIL [disp/aot]: exit $dr expected 26"; exit 1; }
echo "PASS [disp]: derived Display text (struct + enum), JIT + AOT"

# --- 3. derive on a GENERIC struct ---
cat > "$TMP/gen.kd" <<'EOF'
#[derive(Clone, Eq, Display)]
struct Pair<T> { a: T, b: T }
fn main() -> i64 ! { alloc } {
    let p = Pair { a: int_to_string(7), b: int_to_string(88) };
    let c = p.clone();                       // Pair<String> derived clone
    let eq = if p.eq(&c) { 1 } else { 0 };   // derived eq -> 1
    let s = p.to_string();                    // "Pair { a: 7, b: 88 }"
    eq * 100 + str_len(&s)                    // 100 + 20 = 120
}
EOF
check gen "$TMP/gen.kd" 120

echo "PASS: Phase 42 — #[derive(Clone, Eq, Display)] (JIT + AOT)"
