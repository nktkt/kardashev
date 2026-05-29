#!/usr/bin/env bash
# Phase 52 smoke test: generic associated functions — a STATIC trait method
# called on a BOUNDED type param, `T::method()`, monomorphized per instance.
#   1. `fn first<T: Default>() -> T { T::default() }` runs for i64 (-> 0) and
#      String (-> ""); `fn pair_of<T: Default>() -> (T, T)` likewise. The
#      `T::default()` call resolves via T's `Default` bound and dispatches to
#      the concrete impl chosen at monomorphization.
#   2. A user `Default` impl is reachable through the bound (T::default() picks
#      the user impl, not a builtin).
#   3. Negative: `T::default()` for an UNBOUNDED T is rejected.
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

# --- 1+2. T::default() for i64 / String / a user Default type ---
cat > "$TMP/gaf.kd" <<'EOF'
fn pair_of<T: Default>() -> (T, T) ! { alloc } { (T::default(), T::default()) }
fn first<T: Default>() -> T ! { alloc } { T::default() }

struct Tag { v: i64 }
impl Default for Tag { fn default() -> Tag ! { alloc } { Tag { v: 99 } } }

fn make_tag<T: Default>() -> T ! { alloc } { T::default() }

fn main() -> i64 ! { alloc } {
    let zi: i64 = first();               // 0
    let zs: String = first();            // ""
    let p: (i64, i64) = pair_of();       // (0, 0)
    let tg: Tag = make_tag();            // Tag { v: 99 } via the USER impl
    zi + str_len(&zs) + p.0 + p.1 + tg.v // 0 + 0 + 0 + 0 + 99 = 99
}
EOF
run gaf "$TMP/gaf.kd" 99

# --- 3. negative: T::default() with no Default bound on T ---
cat > "$TMP/neg.kd" <<'EOF'
fn bad<T>() -> T ! { alloc } { T::default() }
fn main() -> i64 { 0 }
EOF
if "$KARDC" "$TMP/neg.kd" >/dev/null 2>&1; then
    echo "FAIL [neg]: T::default() on an unbounded T should be rejected"; exit 1
fi
echo "PASS [neg]: unbounded-T static call rejected"

echo "PASS: Phase 52 — generic associated functions T::method() (JIT + AOT)"
