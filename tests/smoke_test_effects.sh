#!/usr/bin/env bash
# Phase 10a smoke test: effect-carrying function types + effect-row
# polymorphism, end to end through both pipeline paths.
#
#   - A higher-order `apply` takes an effect-polymorphic fn `fn(i64)->i64 !
#     {e}` and is itself `! {e}`. Given the effectful `ioInc` (which
#     declares `io`), `apply(ioInc)` performs `io`, so an `io`-declaring
#     `main` type-checks and the call actually prints at runtime.
#   - We verify JIT and AOT produce identical output.
#   - We also confirm the typechecker REJECTS a pure `main` that leaks `io`
#     through the same `apply(ioInc)` — proving effects are tracked through
#     the first-class fn value, not lost.
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

# Positive program: effect-polymorphic higher-order fn instantiated at an
# effectful argument, called from an io-declaring main.
cat > "$TMP/effects.kd" <<'EOF'
fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }
fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }
fn main() -> i64 ! {io} {
    let r = apply(ioInc);
    print(r);
    0
}
EOF

# JIT: apply(ioInc) prints 10 (inside ioInc), returns 11; main prints 11;
# kardc then prints main's return value (0) on a final line.
JIT_OUT=$("$KARDC" "$TMP/effects.kd")
JIT_PRINT_OUT=$(echo "$JIT_OUT" | head -n 2)
EXPECTED=$'10\n11'
if [[ "$JIT_PRINT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: JIT effect-poly output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$JIT_PRINT_OUT"
    exit 1
fi
echo "JIT: apply(ioInc) printed 10 then 11"

# AOT: the native binary writes exactly the two print lines.
"$KARDC" -o "$TMP/prog" "$TMP/effects.kd"
AOT_OUT=$("$TMP/prog")
if [[ "$AOT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: AOT effect-poly output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$AOT_OUT"
    exit 1
fi
echo "AOT: matches"

# Negative program: pure main leaks io through apply(ioInc) -> must be a
# typecheck error mentioning the io effect.
cat > "$TMP/leak.kd" <<'EOF'
fn apply(f: fn(i64) -> i64 ! {e}) -> i64 ! {e} { f(10) }
fn ioInc(x: i64) -> i64 ! {io} { print(x); x + 1 }
fn main() -> i64 { apply(ioInc) }
EOF
set +e
ERR_OUT=$("$KARDC" "$TMP/leak.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: pure main leaking io through apply(ioInc) should be rejected"
    exit 1
fi
if ! echo "$ERR_OUT" | grep -q 'effect `io`'; then
    echo 'FAIL: expected an `io` effect-undeclared diagnostic, got:'
    echo "$ERR_OUT"
    exit 1
fi
echo "Negative: pure main leaking io is correctly rejected"

echo "PASS: effect-carrying fn types + row polymorphism work in JIT + AOT"
