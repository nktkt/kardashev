#!/usr/bin/env bash
# v34 Phase 186 smoke test — `#[cfg(...)]` conditional compilation.
#
# An item annotated `#[cfg(predicate)]` is included in the build only when the
# predicate is satisfied by the active flag set, which the driver populates from
# `--cfg NAME` / `--cfg key=value` options. Predicates: a bare flag, `not(...)`,
# `all(...)`, `any(...)`, and `key = "value"`. A disabled item is dropped during
# PARSING — before type checking — so it may even reference undefined types.
# Differentially gated JIT vs AOT, across multiple flag sets.
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

# diff_run <name> <expected> <src> [cfg flags...]
diff_run() {
    local name="$1" expect="$2" src="$3"; shift 3
    local cfg=("$@")
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    # NOTE: `"${cfg[@]+"${cfg[@]}"}"` (not a bare `"${cfg[@]}"`) so that an EMPTY
    # array expands to nothing under `set -u` on macOS's bash 3.2 — a bare
    # expansion there is an "unbound variable" error (bash 5 on Linux tolerates
    # it), which is exactly what broke this test on the macOS CI runner.
    local jit; jit=$("$KARDC" "${cfg[@]+"${cfg[@]}"}" "$TMP/$name.kd" 2>/dev/null | head -n "$n") || true
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache "${cfg[@]+"${cfg[@]}"}" -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name (cfg: ${cfg[*]:-none})"
}

# A program that exercises bare / not / all / any / feature predicates. The
# SAME source is compiled under different flag sets and must select different
# function bodies — true conditional compilation.
PROG='
#[cfg(fast)]
fn pick() -> i64 { 111 }
#[cfg(not(fast))]
fn pick() -> i64 { 222 }

#[cfg(all(unix, posix))]
fn plat() -> i64 { 7 }
#[cfg(any(unix, windows))]
fn plat2() -> i64 { 1 }
#[cfg(not(any(unix, windows)))]
fn plat2() -> i64 { 0 }

#[cfg(feature = "logging")]
fn extra() -> i64 { 99 }
#[cfg(not(feature = "logging"))]
fn extra() -> i64 { 0 }

fn main() -> i64 ! { io } {
    print(pick());
    print(plat());
    print(plat2());
    print(extra());
    0
}
'

# 1. --cfg fast --cfg unix --cfg posix: pick=111, plat=7, plat2=1, extra=0.
diff_run cfg_set_a $'111\n7\n1\n0' "$PROG" --cfg fast --cfg unix --cfg posix

# 2. A different flag set selects different bodies: default pick=222; plat
#    needs unix+posix so enable both; plat2 via windows; extra via feature.
diff_run cfg_set_b $'222\n7\n1\n99' "$PROG" --cfg unix --cfg posix --cfg windows --cfg feature=logging

# 3. A disabled item is dropped during PARSING, before type checking — so it
#    may reference an undefined type without breaking the build. `never` is not
#    set, so `dead` (returning the nonexistent `Ghost`) simply vanishes.
diff_run cfg_drops_before_typecheck $'5' '
#[cfg(never)]
fn dead() -> Ghost { conjure_a_ghost() }
fn main() -> i64 ! { io } { print(5); 0 }
'

# 4. Vacuous predicates: `all()` is true (item kept), `any()` is false (dropped).
diff_run cfg_vacuous $'1\n2' '
#[cfg(all())]
fn kept() -> i64 { 1 }
#[cfg(any())]
fn gone() -> i64 { 1 }
fn gone() -> i64 { 2 }
fn main() -> i64 ! { io } { print(kept()); print(gone()); 0 }
'

echo "ALL PHASE 186 SMOKE TESTS PASSED"
