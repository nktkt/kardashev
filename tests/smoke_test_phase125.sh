#!/usr/bin/env bash
# Phase 125 smoke test: `&<temporary>` — taking a reference to an rvalue.
#   - `&Val(42)` (enum literal), `&5` (int literal), `&Nil` (nullary variant),
#     `&P { .. }` (struct literal), `&(arith)` all materialize a slot and borrow
#     it (previously a hard codegen error / a wrong scalar);
#   - DROP SAFETY: a droppable temporary `&Text(int_to_string(i))` (an enum that
#     owns a heap String), referenced in a 500k loop, frees its String exactly
#     once at scope exit — RSS stays flat (no leak) and MALLOC_CHECK_=3 finds no
#     double-free;
#   - `&()` (a unit value has no storage to borrow) is still a clean codegen
#     error, not a crash.
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

# --- 1. the rvalue-ref forms, each summed into one value (expect 1+2+4+8+16=31) ---
cat > "$TMP/refs.kd" <<'EOF'
enum W { Val(i64), Nil }
struct P { x: i64, y: i64 }
fn unwrap(w: &W) -> i64 { match w { Val(x) => *x, Nil => 0, } }
fn tag(w: &W) -> i64 { match w { Val(x) => 1, Nil => 2, } }
fn deref(x: &i64) -> i64 { *x }
fn sum(p: &P) -> i64 { p.x + p.y }
fn main() -> i64 {
    let r1 = if unwrap(&Val(42)) == 42 { 1 } else { 0 };   // enum literal w/ payload
    let r2 = if tag(&Nil) == 2 { 2 } else { 0 };           // nullary variant
    let r3 = if deref(&5) == 5 { 4 } else { 0 };           // int literal
    let r4 = if deref(&(3 * 4 + 1)) == 13 { 8 } else { 0 };// arithmetic temporary
    let r5 = if sum(&P { x: 10, y: 5 }) == 15 { 16 } else { 0 }; // struct literal
    r1 + r2 + r3 + r4 + r5                                  // 31
}
EOF
jit=$("$KARDC" "$TMP/refs.kd")
[[ "$jit" != "31" ]] && { echo "FAIL [refs/jit]: expected 31, got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/refs" "$TMP/refs.kd" >/dev/null
set +e; "$TMP/refs" >/dev/null; rc=$?; set -e
[[ "$rc" -ne 31 ]] && { echo "FAIL [refs/aot]: exit $rc (expected 31)"; exit 1; }
echo "PASS [refs]: &enum-literal / &nullary / &int / &arith / &struct-literal — JIT + AOT"

# --- 2. droppable temporary drops exactly once: RSS flat + no double-free ---
cat > "$TMP/drop.kd" <<'EOF'
enum Msg { Text(String), Empty }
fn len_of(m: &Msg) -> i64 {
    match m {
        Text(s) => str_len(s),
        Empty => 0,
    }
}
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    let mut acc = 0;
    while i < 500000 {
        acc = acc + len_of(&Text(int_to_string(i)));   // &<temporary> owns a heap String
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/drop" "$TMP/drop.kd" >/dev/null
set +e; MALLOC_CHECK_=3 "$TMP/drop" >/dev/null; rc=$?; set -e
[[ "$rc" -ne 0 ]] && { echo "FAIL [drop]: exit $rc under MALLOC_CHECK_=3 — temporary double-freed or not dropped"; exit 1; }
rss=$(peak_rss_kb "$TMP/drop")
if [[ -z "$rss" ]]; then
    echo "SKIP [drop]: no GNU/BSD /usr/bin/time for the RSS gate (double-free check passed)"
else
    echo "INFO [drop]: peak RSS over 500k &Text(String) temporaries = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [drop]: RSS ${rss} KB > 32 MB — the ref-temporary's String is leaking (not dropped)"
        exit 1
    fi
    echo "PASS [drop]: &<droppable temporary> drops exactly once — RSS flat, no double-free"
fi

# --- 3. `&()` (unit has no storage) is a clean codegen error, not a crash ---
cat > "$TMP/unit.kd" <<'EOF'
fn main() -> i64 { let r = &(); 0 }
EOF
set +e; out=$("$KARDC" "$TMP/unit.kd" 2>&1); rc=$?; set -e
[[ "$rc" -eq 0 ]] && { echo "FAIL [unit]: &() should not compile"; exit 1; }
[[ "$rc" -ge 128 ]] && { echo "FAIL [unit]: &() crashed (signal $((rc-128))) instead of a clean error"; exit 1; }
grep -q "operand must be a binding" <<< "$out" || { echo "FAIL [unit]: missing real error; got: $out"; exit 1; }
echo "PASS [unit]: &() reports a clean codegen error (no crash)"

echo "PASS: Phase 125 — &<temporary> materializes a dropped slot; unit-ref stays a clean error"
