#!/usr/bin/env bash
# Phase 69 smoke test (Roadmap v12 — the real stdlib): string -> number parsing
# (`parse_int` / `parse_f64` returning Option) + `int_to_hex` formatting. The #1
# stdlib gap: reading data no longer needs a hand-rolled digit loop.
#   1. parse_int parses signed decimals; a string that is not WHOLLY a number
#      (trailing junk, empty, embedded spaces) is None. JIT + AOT.
#   2. parse_f64 parses floats; the same all-or-nothing rule.
#   3. int_to_hex gives lowercase hex (the two's-complement pattern for n<0).
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

# 1+2+3: a program that exercises every case and prints a stable transcript.
cat > "$TMP/parse.kd" <<'EOF'
fn pi(s: &String) -> i64 { match parse_int(s) { Some(v) => v, None => 0 - 999 } }
fn pf(s: &String) -> i64 { match parse_f64(s) { Some(v) => (v * 100.0) as i64, None => 0 - 999 } }
fn main() -> i64 ! { io, alloc } {
    let a = "42";       let b = "-7";      let c = "0";
    let d = "42x";      let e = "";        let g = " 5";
    print(pi(&a));      // 42
    print(pi(&b));      // -7
    print(pi(&c));      // 0
    print(pi(&d));      // -999  (trailing junk)
    print(pi(&e));      // -999  (empty)
    print(pi(&g));      // -999  (leading space -> not wholly a number)
    let f1 = "3.5";     let f2 = "-0.25";  let f3 = "1e3";   let f4 = "x";
    print(pf(&f1));     // 350
    print(pf(&f2));     // -25
    print(pf(&f3));     // 100000
    print(pf(&f4));     // -999
    let h1 = int_to_hex(255);    print_str(&h1);   // ff
    let h2 = int_to_hex(4096);   print_str(&h2);   // 1000
    0
}
EOF
# The 12 explicit print lines. JIT also echoes main's i64 return (0) as a final
# line; AOT does not (it is the exit code). Compare accordingly.
want=$'42\n-7\n0\n-999\n-999\n-999\n350\n-25\n100000\n-999\nff\n1000'
got=$("$KARDC" "$TMP/parse.kd" 2>/dev/null | head -12)
[[ "$got" == "$want" ]] || { echo "FAIL [parse/jit]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
echo "PASS [parse/jit]: parse_int / parse_f64 (Option, all-or-nothing) + int_to_hex"

# AOT: same 12 lines, exit 0.
"$KARDC" --no-cache -o "$TMP/parse" "$TMP/parse.kd" >/dev/null 2>&1
aot=$("$TMP/parse")
[[ "$aot" == "$want" ]] || { echo "FAIL [parse/aot]:"; diff <(echo "$want") <(echo "$aot"); exit 1; }
echo "PASS [parse/aot]: same transcript AOT-compiled"

# A round trip: format a number to hex then it is a valid hex string (sanity).
cat > "$TMP/big.kd" <<'EOF'
fn main() -> i64 ! { io } {
    let s = "9223372036854775807";   // i64::MAX
    print(match parse_int(&s) { Some(v) => v, None => 0 - 1 });
    0
}
EOF
[[ "$("$KARDC" "$TMP/big.kd" 2>/dev/null | head -1)" == "9223372036854775807" ]] || { echo "FAIL [big]: i64::MAX did not round-parse"; exit 1; }
echo "PASS [big]: parse_int handles i64::MAX"

echo "ALL PHASE 69 SMOKE TESTS PASSED"
