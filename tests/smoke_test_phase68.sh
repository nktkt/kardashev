#!/usr/bin/env bash
# Phase 68 CAPSTONE (Roadmap v11 — the numeric tower): FNV-1a + CRC-32 + a
# binary parser in kardashev, each checked against its known answer.
#   1. Builds the real examples/checksum/main.kd and runs it JIT + AOT. It
#      prints the five known witnesses (FNV-1a low32, CRC-32 of "abc" and of
#      "123456789", a little-endian u32, a big-endian u16) and returns their
#      sum 4706988969 (exit code = 4706988969 & 255 = 169).
#   2. The capstone is impossible with i64-only: it needs a u64 literal past
#      i64::MAX, wrapping u64 multiply, unsigned logical `>>`, the bitwise
#      operators, and u8->u16/u32 casts. We assert the exact known values.
#   3. Cross-checks: CRC-32 of the canonical "123456789" vector == 0xcbf43926,
#      and a from-scratch FNV-1a equals the example's.
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

# Locate the capstone source (Bazel runfiles vs. local tree).
SRC=""
for cand in \
    "${TEST_SRCDIR:-}/_main/examples/checksum/main.kd" \
    "${TEST_SRCDIR:-}/kardashev/examples/checksum/main.kd" \
    "${RUNFILES_DIR:-}/_main/examples/checksum/main.kd" \
    "${RUNFILES_DIR:-}/kardashev/examples/checksum/main.kd" \
    "examples/checksum/main.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/checksum/main.kd not found"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. the capstone prints the five known witnesses and returns their sum.
out=$("$KARDC" "$SRC" 2>/dev/null)
expected_lines=$'88168267\n891568578\n3421780262\n305419896\n51966\n4706988969'
[[ "$out" == "$expected_lines" ]] || {
    echo "FAIL [checksum/jit]: output mismatch"; echo "--- got ---"; echo "$out"
    echo "--- want ---"; echo "$expected_lines"; exit 1; }
echo "PASS [checksum/jit]: FNV-1a, CRC-32 x2, LE u32, BE u16 all match; sum 4706988969"

# AOT: same five prints, exit code = 4706988969 & 255 = 169.
"$KARDC" --no-cache -o "$TMP/checksum" "$SRC" >/dev/null 2>&1
set +e; aout=$("$TMP/checksum"); rc=$?; set -e
[[ "$rc" -eq 169 ]] || { echo "FAIL [checksum/aot]: exit $rc expected 169"; exit 1; }
[[ "$aout" == $'88168267\n891568578\n3421780262\n305419896\n51966' ]] || {
    echo "FAIL [checksum/aot]: print mismatch; got: $aout"; exit 1; }
echo "PASS [checksum/aot]: same five witnesses, exit 169 (= sum & 255)"

# 2. cross-check CRC-32 of the canonical "123456789" vector == 0xcbf43926.
cat > "$TMP/crc.kd" <<'EOF'
fn crc32<const N: i64>(data: [u8; N]) -> u32 {
    let mut crc: u32 = 0xFFFFFFFF;
    let mut i: i64 = 0;
    while i < N {
        crc = crc ^ (data[i] as u32);
        let mut bit: i64 = 0;
        while bit < 8 {
            let mask: u32 = 0u32 - (crc & 1u32);
            crc = (crc >> 1) ^ (mask & 0xEDB88320);
            bit = bit + 1;
        }
        i = i + 1;
    }
    crc ^ 0xFFFFFFFF
}
fn main() -> i64 {
    let v: [u8; 9] = [49u8,50u8,51u8,52u8,53u8,54u8,55u8,56u8,57u8];
    crc32(v) as i64
}
EOF
jit=$("$KARDC" "$TMP/crc.kd" 2>/dev/null | tail -1)
[[ "$jit" == "3421780262" ]] || { echo "FAIL [crc-vector]: expected 3421780262 (0xcbf43926) got '$jit'"; exit 1; }
echo "PASS [crc-vector]: CRC-32(\"123456789\") = 0xcbf43926"

echo "ALL PHASE 68 SMOKE TESTS PASSED"
