#!/usr/bin/env bash
# Phase 40 smoke test: generic `impl<T: Bound>` blocks.
#   1. `impl<T: Display> Display for Pair<T>` formats Pair<i64> AND Pair<String>
#      — two DISTINCT monomorphizations (T's own Display dispatched per instance).
#   2. A generic impl on a BUILT-IN container (`impl<T> .. for Vec<T>`) is
#      callable.
#   3. The bound is enforced at typecheck (a body calling `.to_string()` on an
#      unbounded T is rejected).
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

# --- 1. impl<T: Display> Display for Pair<T> — Pair<i64> + Pair<String> ---
cat > "$TMP/pair.kd" <<'EOF'
struct Pair<T> { a: T, b: T }
impl<T: Display> Display for Pair<T> {
    fn to_string(&self) -> String ! { alloc } {
        let mut out = "(";
        string_push_str(&mut out, self.a.to_string());
        string_push_str(&mut out, ",");
        string_push_str(&mut out, self.b.to_string());
        string_push_str(&mut out, ")");
        out
    }
}
fn main() -> i64 ! { alloc } {
    let pi = Pair { a: 11, b: 2222 };                       // "(11,2222)" = 9
    let ps = Pair { a: int_to_string(7), b: int_to_string(999) }; // "(7,999)" = 7
    let si = pi.to_string();
    let ss = ps.to_string();
    str_len(&si) + str_len(&ss)                              // 16
}
EOF
check pair "$TMP/pair.kd" 16

# --- 2. generic impl on a built-in container ---
cat > "$TMP/vec.kd" <<'EOF'
trait Count { fn count(&self) -> i64; }
impl<T> Count for Vec<T> { fn count(&self) -> i64 { vec_len(self) } }
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 10); vec_push(&mut v, 20); vec_push(&mut v, 30);
    let mut w = vec_new();
    vec_push(&mut w, int_to_string(1));
    v.count() * 10 + w.count()                               // 31
}
EOF
check vec "$TMP/vec.kd" 31

# (A generic-impl method dispatching THROUGH a bounded-generic receiver `T` to
# ANOTHER generic-impl method — e.g. a `Wrap<Wrap<i64>>` whose to_string calls
# the inner Wrap<i64>'s to_string — is deferred: that nested
# generic-impl-to-generic-impl monomorphization is Phase-41 container territory.)

# --- 3. the bound is enforced (negative: unbounded T calling a trait method) ---
cat > "$TMP/neg.kd" <<'EOF'
struct Pair<T> { a: T, b: T }
impl<T> Display for Pair<T> {
    fn to_string(&self) -> String ! { alloc } { self.a.to_string() }
}
fn main() -> i64 { 0 }
EOF
if "$KARDC" "$TMP/neg.kd" >/dev/null 2>&1; then
    echo "FAIL [neg]: unbounded-T trait-method call should be rejected"; exit 1
fi
echo "PASS [neg]: unbounded-generic trait-method call rejected"

echo "PASS: Phase 40 — generic impl<T: Bound> blocks (JIT + AOT)"
