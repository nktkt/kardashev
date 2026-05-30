#!/usr/bin/env bash
# Roadmap v12 PRE-MERGE adversarial-review regressions. A multi-agent review of
# the real-stdlib phases found two MAJORs the green suite missed; both fixed and
# pinned here (same discipline as tests/smoke_test_v11_review.sh).
#
#   PARSE OVERFLOW: parse_int of a value past the i64 range returned a silently
#     clamped Some(i64::MAX/MIN) instead of None — strtoll's ERANGE was
#     unchecked. Fix: clear errno before strtoll and reject on ERANGE.
#   DISCARDED-TEMPORARY LEAK: an owned value moved out by `vec_remove(&mut v, 0);`
#     (or any call result like `int_to_string(n);`) used as an expression-
#     STATEMENT was never dropped — its heap leaked. Fix: codegen drops a
#     discarded droppable call-result (via an entry-block temp), dropping it
#     EXACTLY once (no double-free).
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

# 1. parse_int rejects out-of-i64-range values (ERANGE) but keeps the bounds.
cat > "$TMP/ovf.kd" <<'EOF'
fn p(s: &String) -> i64 { match parse_int(s) { Some(v) => v, None => 0 - 1 } }
fn main() -> i64 ! { io } {
    let over = "9223372036854775808";    // i64::MAX + 1   -> None
    let undr = "-9223372036854775809";   // i64::MIN - 1   -> None
    let huge = "99999999999999999999";   // 20 nines       -> None
    let maxv = "9223372036854775807";    // i64::MAX       -> Some
    let minv = "-9223372036854775808";   // i64::MIN       -> Some
    print(p(&over)); print(p(&undr)); print(p(&huge));
    print(p(&maxv)); print(p(&minv));
    0
}
EOF
got=$("$KARDC" "$TMP/ovf.kd" 2>/dev/null); got=$(head -5 <<< "$got")
want=$'-1\n-1\n-1\n9223372036854775807\n-9223372036854775808'
[[ "$got" == "$want" ]] || { echo "FAIL [overflow]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
echo "PASS [parse-overflow]: out-of-i64-range -> None; i64::MAX / i64::MIN still Some"

# 2. a discarded owned temporary (the String moved out by vec_remove used as a
#    statement) is dropped EXACTLY once — not leaked, not double-freed. A Drop
#    counter prints each id once.
cat > "$TMP/drop.kd" <<'EOF'
trait Drop { fn drop(&mut self) ! { io }; }
struct Noisy { id: i64 }
impl Drop for Noisy { fn drop(&mut self) ! { io } { print(self.id); } }
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    vec_push(&mut v, Noisy { id: 1 });
    vec_push(&mut v, Noisy { id: 2 });
    vec_push(&mut v, Noisy { id: 3 });
    vec_remove(&mut v, 0);   // discards Noisy{1}: dropped exactly once
    99
}
EOF
# JIT: discarded-drop (1), then v's scope drops (2,3), then the return (99).
got=$("$KARDC" "$TMP/drop.kd" 2>/dev/null)
[[ "$got" == $'1\n2\n3\n99' ]] || { echo "FAIL [discard-drop/jit]: expected 1,2,3,99 got: $got"; exit 1; }
# AOT: the three drop prints, exit 99. A double-free would crash; a leak would
# omit the '1'.
"$KARDC" --no-cache -o "$TMP/drop" "$TMP/drop.kd" >/dev/null 2>&1
set +e; aout=$("$TMP/drop"); rc=$?; set -e
[[ "$rc" -eq 99 && "$aout" == $'1\n2\n3' ]] || { echo "FAIL [discard-drop/aot]: exit $rc out '$aout'"; exit 1; }
echo "PASS [discard-drop]: discarded vec_remove result dropped exactly once (no leak, no double-free)"

# 3. and the leak is gone at scale: 200k discarded removes stay flat in RSS
#    (guarded on GNU time -v; skipped where unavailable, e.g. macOS BSD time).
cat > "$TMP/leak.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    while i < 200000 {
        let mut v = vec_new();
        vec_push(&mut v, int_to_string(1234567890));
        vec_remove(&mut v, 0);
        i = i + 1;
    }
    0
}
EOF
if /usr/bin/time -v true >/dev/null 2>&1; then
    "$KARDC" --no-cache -o "$TMP/leak" "$TMP/leak.kd" >/dev/null 2>&1
    rss=$(/usr/bin/time -v "$TMP/leak" 2>&1 | awk '/Maximum resident/ {print $NF}')
    [[ "$rss" -lt 30000 ]] || { echo "FAIL [no-leak]: RSS ${rss}KB too high (leak?)"; exit 1; }
    echo "PASS [no-leak]: 200k discarded removes stay flat (RSS ${rss}KB)"
else
    echo "SKIP [no-leak]: GNU /usr/bin/time -v unavailable"
fi

echo "ALL V12 REVIEW-REGRESSION TESTS PASSED"
