#!/usr/bin/env bash
# Phase 10b smoke test: capturing closures end to end through JIT + AOT.
#
#   - A closure `|x| x + n` captures `n` by value and is invoked through the
#     uniform fat-pointer fn-value dispatch.
#   - Multi-capture and a closure passed to an effect-polymorphic higher-
#     order fn both round-trip.
#   - Effect propagation: a closure that calls `print` is `{io}`. Calling it
#     from an `io`-declaring context is accepted (and actually prints); the
#     same closure called from a PURE context is a typecheck error — proving
#     the closure's body effects flow to its call site (Phase 10a + 10b).
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- Value program: single capture, multi-capture, and a closure passed to
# a higher-order fn. main prints each result then returns 0.
cat > "$TMP/closures.kd" <<'EOF'
fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }
fn main() -> i64 ! {io} {
    let n = 5;
    let add_n = |x| x + n;
    print(add_n(10));        // 15

    let a = 3;
    let b = 4;
    let g = |x| x + a + b;
    print(g(10));            // 17

    let k = 7;
    print(apply(|x| x + k)); // 17
    0
}
EOF

JIT_OUT=$("$KARDC" "$TMP/closures.kd")
JIT_PRINTS=$(head -n 3 <<< "$JIT_OUT")
EXPECTED=$'15\n17\n17'
if [[ "$JIT_PRINTS" != "$EXPECTED" ]]; then
    echo "FAIL: JIT closure output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$JIT_PRINTS"
    exit 1
fi
echo "JIT: add_n(10)=15, g(10)=17, apply(|x| x+k)=17"

# AOT: the native binary writes exactly the three print lines.
"$KARDC" -o "$TMP/prog" "$TMP/closures.kd"
AOT_OUT=$("$TMP/prog")
if [[ "$AOT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: AOT closure output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$AOT_OUT"
    exit 1
fi
echo "AOT: matches"

# --- Effect propagation, positive: an io closure called from an io context.
cat > "$TMP/eff_ok.kd" <<'EOF'
fn main() -> i64 ! {io} {
    let n = 100;
    let p = |x| print(x + n);
    p(5);
    0
}
EOF
EFF_JIT=$("$KARDC" "$TMP/eff_ok.kd"); EFF_JIT=$(head -n 1 <<< "$EFF_JIT")
if [[ "$EFF_JIT" != "105" ]]; then
    echo "FAIL: io closure (positive) JIT expected 105, got: $EFF_JIT"
    exit 1
fi
"$KARDC" -o "$TMP/eff_ok_prog" "$TMP/eff_ok.kd"
EFF_AOT=$("$TMP/eff_ok_prog")
if [[ "$EFF_AOT" != "105" ]]; then
    echo "FAIL: io closure (positive) AOT expected 105, got: $EFF_AOT"
    exit 1
fi
echo "Positive: io closure called from io context prints 105 (JIT + AOT)"

# --- Effect propagation, negative: the same io closure called from a PURE
# context must be a typecheck error mentioning the io effect.
cat > "$TMP/eff_bad.kd" <<'EOF'
fn main() -> i64 {
    let p = |x| print(x);
    p(5)
}
EOF
set +e
ERR_OUT=$("$KARDC" "$TMP/eff_bad.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: pure main calling an io closure should be rejected"
    exit 1
fi
if ! grep -q 'effect `io`' <<< "$ERR_OUT"; then
    echo 'FAIL: expected an `io` effect-undeclared diagnostic, got:'
    echo "$ERR_OUT"
    exit 1
fi
echo "Negative: pure main calling an io closure is correctly rejected"

echo "PASS: capturing closures work in JIT + AOT; effects propagate from body to call site"
