#!/usr/bin/env bash
# Soundness regression guard — locks in the fixes for the 6 issues a pre-merge
# multi-agent review of Roadmaps v6–v9 confirmed (5 memory-safety / type-
# soundness blockers + 1 effect-soundness major). Each case must behave exactly
# as asserted; a regression that re-opens any hole fails here.
#   1. hashmap_get / vec_get of a heap-owning value deep-CLONE (no double-free).
#   2. `dyn Trait<T>` coercion checks the trait arg (Producer<i64> !-> dyn
#      Producer<String>); a matching arg is accepted.
#   3. `*r` cannot MOVE a non-Copy pointee out of a `&` borrow.
#   4. a `&mut` binding cannot be reborrowed twice as `&mut` in one call
#      (`f(r, r)`); multiple SHARED reborrows stay legal (sort works).
#   5. a value moved in one `if` branch + reassigned in the other is
#      "maybe moved" — a later use is rejected.
#   6. a `dyn`/generic call attributes the trait method's declared effects (a
#      pure fn cannot call an `io` trait method through a trait object).
# JIT + AOT for the positive cases.
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

ok_run() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f" | tail -1)
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT exit $r (clean, no abort)"
}
rejects() { # name file needle
    local n=$1 f=$2 needle=$3 out
    set +e; out=$("$KARDC" "$f" 2>&1); local rc=$?; set -e
    if "$KARDC" "$f" >/dev/null 2>&1; then
        echo "FAIL [$n]: expected REJECTION, but it compiled"; exit 1
    fi
    if [[ -n "$needle" ]] && ! echo "$out" | grep -qi "$needle"; then
        echo "FAIL [$n]: rejected but diagnostic missing '$needle'; got: $out"; exit 1
    fi
    echo "PASS [$n]: rejected as expected"
}

# 1. hashmap_get / vec_get of a heap value — clone, no double-free.
cat > "$TMP/getclone.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, 1, int_to_string(98765));
    let g = match hashmap_get(&m, 1) { Some(v) => v, None => string_new() };
    let mut v = vec_new();
    vec_push(&mut v, int_to_string(42));
    let e = vec_get(&v, 0);
    str_len(&g) + str_len(&e)        // 5 + 2 = 7
}
EOF
ok_run getclone "$TMP/getclone.kd" 7

# 2a. dyn trait-arg mismatch rejected.
cat > "$TMP/dynmismatch.kd" <<'EOF'
trait Producer<T> { fn produce(&self) -> T; }
struct IntGen { base: i64 }
impl Producer<i64> for IntGen { fn produce(&self) -> i64 { self.base } }
fn run(p: &dyn Producer<String>) -> i64 ! { alloc } { let s = p.produce(); str_len(&s) }
fn main() -> i64 ! { alloc } { let g = IntGen { base: 5 }; run(&g) }
EOF
rejects dynmismatch "$TMP/dynmismatch.kd" ""
# 2b. matching dyn arg accepted.
cat > "$TMP/dynmatch.kd" <<'EOF'
trait Producer<T> { fn produce(&self) -> T; }
struct IntGen { base: i64 }
impl Producer<i64> for IntGen { fn produce(&self) -> i64 { self.base } }
fn run(p: &dyn Producer<i64>) -> i64 { p.produce() }
fn main() -> i64 { let g = IntGen { base: 5 }; run(&g) }
EOF
ok_run dynmatch "$TMP/dynmatch.kd" 5

# 3. move non-Copy out of a `&` via `*r` rejected.
cat > "$TMP/derefmove.kd" <<'EOF'
struct S { name: String }
fn take(x: S) -> i64 ! { alloc } { str_len(&x.name) }
fn steal(r: &S) -> i64 ! { alloc } { let stolen = *r; take(stolen) }
fn main() -> i64 ! { alloc } { let s = S { name: int_to_string(1) }; steal(&s) }
EOF
rejects derefmove "$TMP/derefmove.kd" "out of a borrowed reference"

# 4a. f(r, r) two &mut rejected.
cat > "$TMP/mutalias.kd" <<'EOF'
struct P { v: i64 }
fn two(a: &mut P, b: &mut P) -> i64 { a.v = 1; b.v = 2; a.v }
fn relay(r: &mut P) -> i64 { two(r, r) }
fn main() -> i64 { let mut p = P { v: 0 }; relay(&mut p) }
EOF
rejects mutalias "$TMP/mutalias.kd" "more than once"
# 4b. multiple shared reborrows (sort) still legal.
cat > "$TMP/sortok.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 30); vec_push(&mut v, 10); vec_push(&mut v, 20);
    sort(&mut v);
    vec_get(&v, 0) * 100 + vec_get(&v, 1) * 10 + vec_get(&v, 2)   // 1230
}
EOF
ok_run sortok "$TMP/sortok.kd" 1230

# 5. move-in-then / reassign-in-else / use rejected (branch join).
cat > "$TMP/branchmove.kd" <<'EOF'
struct S { name: String }
fn take(s: S) -> i64 ! { alloc } { str_len(&s.name) }
fn main() -> i64 ! { alloc } {
    let mut s = S { name: int_to_string(1) };
    let c = true;
    if c { take(s); } else { s = S { name: int_to_string(2) }; }
    str_len(&s.name)
}
EOF
rejects branchmove "$TMP/branchmove.kd" "moved"

# 6. pure fn calling an io trait method through &dyn rejected (effect attribution).
cat > "$TMP/dyneffect.kd" <<'EOF'
trait Speaker { fn speak(&self) -> i64 ! { io }; }
struct Dog { n: i64 }
impl Speaker for Dog { fn speak(&self) -> i64 ! { io } { let m = "woof"; println(&m); self.n } }
fn run(s: &dyn Speaker) -> i64 { s.speak() }
fn main() -> i64 ! { io } { let d = Dog { n: 3 }; run(&d) }
EOF
rejects dyneffect "$TMP/dyneffect.kd" "io"

echo "PASS: soundness regression guard (review blockers #1-#5 + effect-attribution) JIT + AOT"
