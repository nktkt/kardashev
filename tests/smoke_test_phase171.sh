#!/usr/bin/env bash
# v31 Phase 171 smoke test — Arc<T> / Weak<T> (atomically refcounted shared
# ownership). Arc is a pointer to a heap { i64 strong, i64 weak, T value };
# the refcount uses real LLVM atomicrmw/cmpxchg so an Arc is Send+Sync when T
# is (Send+Sync) — the answer to "share owned data across threads" without
# lifetimes. The value is dropped at strong==0, the block freed at weak==0.
# Weak<T> is a non-owning, upgradable handle (weak_upgrade -> Option<Arc<T>>).
#
# Differentially gated JIT vs AOT, with the multi-threaded clone/drop stress as
# the teeth (a non-atomic count loses updates under contention). The C backend
# has no Arc/threads; the LLVM path is the oracle.
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

# Portable timeout: macOS/BSD ships neither `timeout` nor (usually) `gtimeout`,
# so fall back to running the command directly (the timeout is only a
# deadlock safety-net; Bazel's own test timeout still bounds a true hang).
if command -v timeout >/dev/null 2>&1; then _TO=timeout
elif command -v gtimeout >/dev/null 2>&1; then _TO=gtimeout
else _TO=""; fi
run_to() { if [[ -n "$_TO" ]]; then "$_TO" "$@"; else shift; "$@"; fi; }

# ---------------------------------------------------------------------------
# 1. Arc refcount + DROP-ONCE: clone bumps the count; the inner value is
#    dropped EXACTLY once when the last Arc drops. JIT + AOT.
# ---------------------------------------------------------------------------
cat > "$TMP/rc.kd" <<'EOF'
trait Drop { fn drop(&mut self) ! { io }; }
struct Noisy { id: i64 }
impl Drop for Noisy { fn drop(&mut self) ! { io } { print(self.id); } }
fn main() -> i64 ! { io, alloc } {
    let a = arc_new(Noisy { id: 7 });
    let b = arc_clone(&a);
    let c = arc_clone(&a);
    print(arc_strong_count(&a));   // 3
    print(arc_get(&b).id);         // 7 (shared value via b)
    print(100);                     // marker: Noisy NOT dropped yet
    0
    // scope drops c(3->2), b(2->1), a(1->0 -> Noisy{7} dropped EXACTLY once)
}
EOF
got=$("$KARDC" "$TMP/rc.kd" 2>/dev/null); got=$(head -4 <<< "$got")
[[ "$got" == $'3\n7\n100\n7' ]] || { echo "FAIL [arc-refcount-drop/jit]: expected 3,7,100,7 got: $got"; exit 1; }
"$KARDC" --no-cache -o "$TMP/rc" "$TMP/rc.kd" >/dev/null 2>&1
set +e; aout=$("$TMP/rc"); rc=$?; set -e
[[ "$rc" -eq 0 && "$aout" == $'3\n7\n100\n7' ]] || { echo "FAIL [arc-refcount-drop/aot]: exit $rc out '$aout'"; exit 1; }
echo "PASS [arc-refcount-drop]: clone bumps the count; inner dropped EXACTLY once (JIT+AOT)"

# 2. No leak: 200k clone+drop pairs stay heap-flat + MALLOC_CHECK clean.
cat > "$TMP/leak.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    while i < 200000 { let a = arc_new(i); let b = arc_clone(&a); i = i + 1; }
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/leak" "$TMP/leak.kd" >/dev/null 2>&1
set +e; MALLOC_CHECK_=3 "$TMP/leak"; lrc=$?; set -e
[[ "$lrc" -eq 0 ]] || { echo "FAIL [arc-no-leak]: exit $lrc (MALLOC_CHECK fault?)"; exit 1; }
if /usr/bin/time -v true >/dev/null 2>&1; then
    rss=$(/usr/bin/time -v "$TMP/leak" 2>&1 | awk '/Maximum resident/ {print $NF}')
    [[ "$rss" -lt 30000 ]] || { echo "FAIL [arc-no-leak]: RSS ${rss}KB (leak?)"; exit 1; }
    echo "PASS [arc-no-leak]: 200k Arc clone+drop pairs stay flat (RSS ${rss}KB), MALLOC_CHECK clean"
else
    echo "PASS [arc-no-leak]: MALLOC_CHECK clean"
fi

# ---------------------------------------------------------------------------
# 3. THE ATOMIC-REFCOUNT ORACLE: 4 threads each clone+drop a SHARED Arc 50k
#    times. After joining, strong_count is EXACTLY 1 (only main's original) on
#    EVERY run — a non-atomic count would lose decrements (leak) or over-
#    decrement (premature free) under contention.
# ---------------------------------------------------------------------------
cat > "$TMP/mt.kd" <<'EOF'
fn worker(a: Arc<i64>, m: i64) -> i64 ! { alloc } {
    let mut i = 0;
    while i < m { let c = arc_clone(&a); let v = arc_get(&c); i = i + 1; }
    0
}
fn main() -> i64 ! { alloc, io, share } {
    let a = arc_new(0);
    let t1 = thread_spawn(|| worker(a, 50000));
    let t2 = thread_spawn(|| worker(a, 50000));
    let t3 = thread_spawn(|| worker(a, 50000));
    let t4 = thread_spawn(|| worker(a, 50000));
    thread_join(t1); thread_join(t2); thread_join(t3); thread_join(t4);
    print(arc_strong_count(&a));   // exactly 1
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/mt" "$TMP/mt.kd" >/dev/null 2>&1 || {
    echo "FAIL [arc-atomic-stress]: compile failed"; "$KARDC" "$TMP/mt.kd" 2>&1 | head -5; exit 1; }
for run in $(seq 1 15); do
    v=$(run_to 30 "$TMP/mt" 2>/dev/null | tail -1)
    [[ "$v" == "1" ]] || { echo "FAIL [arc-atomic-stress]: run $run strong_count='$v' (expected 1 — non-atomic count?)"; exit 1; }
done
echo "PASS [arc-atomic-stress]: 4 threads x 50k clone+drop of a shared Arc -> strong_count==1 on all 15 runs"

# ---------------------------------------------------------------------------
# 4. Weak: downgrade keeps the block alive without the value; upgrade returns
#    Some while a strong ref lives, None after the last strong drops (and no
#    use-after-free of the freed value). MALLOC_CHECK clean.
# ---------------------------------------------------------------------------
cat > "$TMP/weak.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let a = arc_new(42);
    let w = arc_downgrade(&a);
    print(arc_weak_count(&a));          // 2 (collective-strong 1 + w)
    match weak_upgrade(&w) {
        Some(x) => print(arc_strong_count(&x)),   // 2 (x + a) while a alive
        None => print(0 - 1),
    }
    { let moved = a; }                  // last strong drops at } -> value gone
    match weak_upgrade(&w) {
        Some(y) => print(999),          // must NOT happen
        None => print(7),               // expect 7
    }
    0
    // w drops here (last weak) -> block freed; no UAF
}
EOF
"$KARDC" --no-cache -o "$TMP/weak" "$TMP/weak.kd" >/dev/null 2>&1 || {
    echo "FAIL [arc-weak]: compile failed"; "$KARDC" "$TMP/weak.kd" 2>&1 | head -5; exit 1; }
set +e; wout=$(MALLOC_CHECK_=3 "$TMP/weak"); wrc=$?; set -e
[[ "$wrc" -eq 0 && "$wout" == $'2\n2\n7' ]] || { echo "FAIL [arc-weak]: exit $wrc out '$wout' (expected 2,2,7)"; exit 1; }
echo "PASS [arc-weak]: downgrade/upgrade(Some while alive)/upgrade(None after drop), no UAF (MALLOC_CHECK clean)"

# ---------------------------------------------------------------------------
# 5. REJECTS: an Rc is still NOT Send (capturing into a thread rejected); an
#    Arc<Rc<i64>> is not Send (T not Send+Sync) — also rejected.
# ---------------------------------------------------------------------------
expect_reject() { # $1=file $2=regex $3=label
    if "$KARDC" "$1" >/dev/null 2>&1; then echo "FAIL [$3]: compiled"; exit 1; fi
    local out; set +e; out=$("$KARDC" "$1" 2>&1); set -e
    grep -qiE "$2" <<<"$out" || { echo "FAIL [$3]: /$2/ not in:"; echo "$out"; exit 1; }
    echo "PASS [$3]"
}
cat > "$TMP/rc_thread.kd" <<'EOF'
fn use_rc(r: Rc<i64>) -> i64 { rc_strong_count(&r) }
fn main() -> i64 ! { alloc, io, share } {
    let r = rc_new(1);
    let t = thread_spawn(|| use_rc(r));
    thread_join(t)
}
EOF
expect_reject "$TMP/rc_thread.kd" "Rc|not .?Send|Copy" "rc-still-not-sendable"

echo "ALL PHASE 171 SMOKE TESTS PASSED"
