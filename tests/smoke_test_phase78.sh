#!/usr/bin/env bash
# Phase 78 smoke test (Roadmap v13 — concurrency): `Rc<T>` — a non-atomic
# reference-counted shared owner, and the LEGIBLE non-`Send` witness for the
# concurrency Send rule. rc_new / rc_clone / rc_get / rc_strong_count.
#   1. The strong count tracks clones (1 -> 2 -> 1 -> 0); the shared value is
#      readable through any handle; the heap block + inner value are dropped
#      EXACTLY once when the last Rc drops (no leak, no double-free). JIT + AOT.
#   2. THE DIFFERENTIATOR: an `Rc` is not `Send` (its refcount is non-atomic) —
#      sending one on a channel is a compile error that names Rc, vs sharing
#      mutable state safely across threads via a Mutex.
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

# 1. refcount + shared read + drop-EXACTLY-once over a Drop-counted inner.
cat > "$TMP/rc.kd" <<'EOF'
trait Drop { fn drop(&mut self) ! { io }; }
struct Noisy { id: i64 }
impl Drop for Noisy { fn drop(&mut self) ! { io } { print(self.id); } }
fn main() -> i64 ! { io, alloc } {
    let a = rc_new(Noisy { id: 7 });
    let b = rc_clone(&a);
    let c = rc_clone(&a);
    print(rc_strong_count(&a));   // 3
    print(rc_get(&b).id);         // 7 (shared value read via b)
    print(100);                   // marker: Noisy must NOT have dropped yet
    0
    // scope drops c(3->2), b(2->1), a(1->0 -> drop Noisy{7} EXACTLY once)
}
EOF
got=$("$KARDC" "$TMP/rc.kd" 2>/dev/null | head -4)
[[ "$got" == $'3\n7\n100\n7' ]] || { echo "FAIL [rc/jit]: expected 3,7,100,7 got: $got"; exit 1; }
"$KARDC" --no-cache -o "$TMP/rc" "$TMP/rc.kd" >/dev/null 2>&1
set +e; aout=$("$TMP/rc"); rc=$?; set -e
[[ "$rc" -eq 0 && "$aout" == $'3\n7\n100\n7' ]] || { echo "FAIL [rc/aot]: exit $rc out '$aout'"; exit 1; }
echo "PASS [rc-refcount-drop]: clone bumps the count; inner dropped EXACTLY once at zero (JIT+AOT)"

# 1b. no leak at scale (200k clone+drop pairs stay flat — guarded on GNU time).
cat > "$TMP/leak.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    while i < 200000 { let a = rc_new(i); let b = rc_clone(&a); i = i + 1; }
    0
}
EOF
if /usr/bin/time -v true >/dev/null 2>&1; then
    "$KARDC" --no-cache -o "$TMP/leak" "$TMP/leak.kd" >/dev/null 2>&1
    rss=$(/usr/bin/time -v "$TMP/leak" 2>&1 | awk '/Maximum resident/ {print $NF}')
    [[ "$rss" -lt 30000 ]] || { echo "FAIL [rc-no-leak]: RSS ${rss}KB (leak?)"; exit 1; }
    echo "PASS [rc-no-leak]: 200k clone+drop pairs stay flat (RSS ${rss}KB)"
else
    echo "SKIP [rc-no-leak]: GNU /usr/bin/time -v unavailable"
fi

# 2. Rc is not Send — sending one on a channel is a compile error.
cat > "$TMP/send.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    let a = rc_new(7);
    chan_send(tx, a);
    0
}
EOF
set +e; out=$("$KARDC" "$TMP/send.kd" 2>&1); set -e
if "$KARDC" "$TMP/send.kd" >/dev/null 2>&1; then echo "FAIL [rc-not-send]: compiled"; exit 1; fi
grep -qi "Rc" <<<"$out" && grep -qi "not .Send.\|not Send" <<<"$out" || { echo "FAIL [rc-not-send]: wrong message: $out"; exit 1; }
echo "PASS [rc-not-send]: sending an Rc on a channel is rejected (non-atomic refcount, not Send)"

echo "ALL PHASE 78 SMOKE TESTS PASSED"
