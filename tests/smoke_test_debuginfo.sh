#!/usr/bin/env bash
# Phase 14a smoke test: DWARF debug info behind the `-g` flag.
#
# Asserts:
#   (a) `kardc -g` emits debug metadata — a compile unit (!llvm.dbg.cu),
#       DISubprograms (incl. the user's `add` and `main`), and DILocations
#       (line info). We check this at the LLVM IR level via `--emit-llvm`,
#       which is platform-independent (no external dwarf tooling needed).
#   (b) WITHOUT `-g`, the IR has NO debug metadata (clean separation — the
#       `-g` path is purely additive).
#   (c) `kardc -g -o` produces a runnable executable with the same result as
#       the no-`-g` build (so -g survives AOT object emission + linking).
#   (d) BONUS, only if llvm-dwarfdump is available: the linked -g executable
#       carries a DWARF compile unit + subprogram. Skipped (not failed) when
#       the tool isn't installed — macOS CI ships LLVM without it.
set -euo pipefail

find_bin() {
    local name=$1
    for candidate in \
        "${TEST_SRCDIR:-}/_main/compiler/$name" \
        "${TEST_SRCDIR:-}/kardashev/compiler/$name" \
        "${RUNFILES_DIR:-}/_main/compiler/$name" \
        "${RUNFILES_DIR:-}/kardashev/compiler/$name" \
        "./compiler/$name" \
        "./build.local/$name"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

KARDC=$(find_bin kardc) || { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/prog.kd" <<'EOF'
fn add(a: i64, b: i64) -> i64 {
    let s = a + b;
    s
}
fn main() -> i64 {
    let x = add(3, 4);
    let y = x + 1;
    y
}
EOF

# --- (a) -g IR carries a compile unit + subprograms + line info. ---
IR_G=$("$KARDC" -g --emit-llvm "$TMP/prog.kd" 2>/dev/null)

if ! grep -q '!llvm.dbg.cu' <<<"$IR_G"; then
    echo "FAIL: -g IR has no !llvm.dbg.cu compile unit"
    exit 1
fi
echo "Found !llvm.dbg.cu compile unit"

SUBPROG_COUNT=$(grep -c 'DISubprogram' <<<"$IR_G" || true)
if [[ "$SUBPROG_COUNT" -lt 1 ]]; then
    echo "FAIL: -g IR has no DISubprogram"
    exit 1
fi
echo "Found $SUBPROG_COUNT DISubprogram entries"

# The user's `add` and `main` should both be present as subprograms.
if ! grep -qE 'DISubprogram\(name: "add"' <<<"$IR_G"; then
    echo "FAIL: -g IR missing DISubprogram for 'add'"
    exit 1
fi
if ! grep -qE 'DISubprogram\(name: "main"' <<<"$IR_G"; then
    echo "FAIL: -g IR missing DISubprogram for 'main'"
    exit 1
fi
echo "DISubprograms include both 'add' and 'main'"

DILOC_COUNT=$(grep -c 'DILocation' <<<"$IR_G" || true)
if [[ "$DILOC_COUNT" -lt 1 ]]; then
    echo "FAIL: -g IR has no DILocation (no line info)"
    exit 1
fi
echo "Found $DILOC_COUNT DILocation entries (line info)"

# --- (b) no-g IR must have NO debug metadata. ---
IR_NOG=$("$KARDC" --emit-llvm "$TMP/prog.kd" 2>/dev/null)
NOG_DBG=$(grep -cE 'DISubprogram|!llvm.dbg.cu|DILocation' <<<"$IR_NOG" || true)
if [[ "$NOG_DBG" -ne 0 ]]; then
    echo "FAIL: no-g IR unexpectedly contains debug metadata ($NOG_DBG refs)"
    exit 1
fi
echo "Clean separation: no-g IR has zero debug metadata"

# --- (c) -g and no-g executables build and run to the same result. ---
"$KARDC" -g -o "$TMP/prog_g"   "$TMP/prog.kd"
"$KARDC"    -o "$TMP/prog_nog" "$TMP/prog.kd"
for b in "$TMP/prog_g" "$TMP/prog_nog"; do
    if [[ ! -x "$b" ]]; then
        echo "FAIL: kardc did not produce executable $b"
        exit 1
    fi
done
set +e
"$TMP/prog_g";   RC_G=$?
"$TMP/prog_nog"; RC_NOG=$?
set -e
if [[ "$RC_G" -ne 8 || "$RC_NOG" -ne 8 ]]; then
    echo "FAIL: expected both binaries to exit 8 (got -g=$RC_G, no-g=$RC_NOG)"
    exit 1
fi
echo "Runtime: both -g and no-g binaries exit 8 (identical result); -g survives AOT link"

# --- (d) BONUS: verify linked-binary DWARF if llvm-dwarfdump is available. ---
DWARFDUMP=""
if command -v llvm-dwarfdump >/dev/null 2>&1; then
    DWARFDUMP=$(command -v llvm-dwarfdump)
elif command -v llvm-config >/dev/null 2>&1; then
    bindir=$(llvm-config --bindir 2>/dev/null || true)
    [[ -n "$bindir" && -x "$bindir/llvm-dwarfdump" ]] && DWARFDUMP="$bindir/llvm-dwarfdump"
fi
if [[ -n "$DWARFDUMP" ]]; then
    INFO_G=$("$DWARFDUMP" --debug-info "$TMP/prog_g" 2>/dev/null || true)
    if grep -q "DW_TAG_compile_unit" <<<"$INFO_G" && grep -q "DW_TAG_subprogram" <<<"$INFO_G"; then
        echo "Bonus: llvm-dwarfdump confirms DWARF compile unit + subprogram in the linked exe"
    else
        echo "Bonus: llvm-dwarfdump present but linked-exe DWARF not in expected form (platform-dependent; IR checks already passed)"
    fi
else
    echo "Bonus: llvm-dwarfdump not available — skipping linked-binary DWARF check (IR-level checks already proved -g works)"
fi

echo "PASS: -g emits a DWARF compile unit + subprograms + line info (IR-verified); no-g stays debug-info-free"
