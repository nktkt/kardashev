# Roadmap

Where kardashev honestly stands, and the work that would move it forward.

kardashev is a **research / portfolio-grade** systems language: a real,
well-tested, ~32 K-LOC LLVM-backed compiler with a genuinely uncommon feature —
zero-runtime-cost, row-polymorphic **effect labels** in the type system — plus
ownership + NLL borrow checking, deterministic `Drop`/RAII, generics/traits, a
full numeric tower, async, and threads. It is well above the median hobby/student
compiler in breadth and test discipline. It is **not** a production language: it
is pre-ecosystem, pre-performance-proven, and MVP-shaped in places.

**Shipped: v1–v19** (Phases 0–114, through `v0.19.0`). The per-version themes are
in the [README roadmap table](README.md#roadmap); every phase's detail is in
[CHANGELOG.md](CHANGELOG.md). v15–v19 built a self-hosted *mini* compiler and a
differential-fuzzing test surface.

## The honest gaps

These separate kardashev from a production language. They are stated plainly so
the roadmap below can close them in priority order.

- **"Self-hosting" is a mini compiler, not a bootstrap.** `examples/selfhost/compile.kd`
  type-checks and runs a tiny 2-type (`i64`/`bool`) expression + function
  language, lowering it to an 8-opcode in-process stack VM. It emits no LLVM IR
  or native code and handles none of kardashev's own features (no
  structs/enums/traits/generics/borrow-check/effects/`Drop`). It proves the
  language can express a compiler-shaped program; it is **not** kardashev
  compiling kardashev.
- **No performance numbers.** There are zero benchmarks. "Zero-cost effects" is
  true but type-system-only; the compiler's actual codegen/runtime speed vs a C
  or Rust reference is simply unmeasured.
- **MVP / leaky stdlib.** *(v21 closed the biggest items here.)* `HashMap`/
  `HashSet` now have `remove` (v21 Phase 122, backward-shift deletion); the
  `spawn` + `join` frame leak is fixed (v21 Phase 121, per-handle release); and
  `Mutex` is now generic over its cell type (v21 Phase 123). What remains MVP:
  the **const-eval scalar set** (`i64`/`bool` only) and the **OS-thread return
  value** (`fn() -> i64` only — async/await is the generic path) are still
  `i64`-shaped; a fully type-safe named `Mutex<T>` (vs the current type-erased
  i64 handle) is also deferred. *(Earlier drafts also listed HashMap interior-K/V
  drop and async `Future`-frame reclaim as leaks — measurement in v21 showed both
  are already clean.)*
- **A few real ergonomic gaps** (verified against the current compiler, *not* the
  stale docs): no `||` logical-or (it collides with closure `||` syntax); no `&`
  of a temporary/rvalue (`&A(10)` errors — bind to a `let` first), plus a related
  miscompile where a ref to an enum literal passes the wrong scalar. *(Note:
  `%`, `&&`, and enum-typed struct fields **do** work — the language reference is
  out of date and claims otherwise.)*
- **No ecosystem / single backend / thin platform story.** Local-path
  dependencies only (a third-party registry is deferred); LLVM is the only
  backend; CI covers Linux + macOS, where one arm64 JIT-teardown flake is papered
  over with retries rather than root-caused; no Windows/WASM; no normative spec;
  pre-1.0 with no stability policy.

## Planned

> Planned, not done. Each roadmap follows the established cadence: implement each
> phase green (JIT **and** AOT) → adversarial review → fix findings →
> consolidating PR → tag/release.

### v20 — toward a real bootstrap (the north star) — *in progress*

Move the self-hosted compiler from a toy toward real kardashev. Full
kardashev-compiles-kardashev is several roadmaps out; v20 is the first concrete
step past "toy":

- ✅ **Phase 115 (done)** — emit a **real artifact** instead of running an
  in-process stack VM. `examples/selfhost/llvmgen.kd` now lowers each `Expr` to
  SSA-form **textual LLVM IR** (`add`/`mul`/`icmp`+`zext`/branch-free `select`)
  and prints a complete module (`define i64 @f(...)` + a `main` calling it), so
  `clang out.ll -o prog && ./prog` runs **natively** and the exit code is the
  function's result. **Differential-gated**: the self-hosted compiler's result
  must equal the host compiler's on the same function (pinned by
  `tests/smoke_test_phase115.sh`). This is the step past "toy" — the self-hosted
  compiler produces a real, compilable native artifact.
- ✅ **Phase 116 (done)** — broaden the differential gate into a **fuzzer**:
  for many random valid functions (over `+ * < ==` and parenthesized `if/else`)
  with random args, the self-hosted-emitted LLVM IR (clang → native) must equal
  the host compiler's result. 75 functions across 3 seeds agree
  (`tests/smoke_test_phase116.sh`) — the self-hosted codegen matches the host.
- ✅ **Phase 117 (structs, done)** — the self-hosted compiler now accepts
  `struct NAME { f: i64, ... }`, builds struct literals, reads fields, and lowers
  them to first-class LLVM aggregates (`insertvalue`/`extractvalue`); every value
  carries its type so the emitter prints the right LLVM type. Differential-gated
  vs the host on several struct programs (`tests/smoke_test_phase117.sh`).
- ✅ **Phase 118 (enums + match, done)** — the self-hosted compiler now accepts
  `enum NAME { V(i64), ... }`, constructs variants `V(e)`, and `match`es them.
  An enum is a tagged pair `{ i64 tag, i64 payload }`; construction →
  `insertvalue`; an enum-typed `if` → `select` over the aggregate; `match` →
  `extractvalue` tag/payload + a branch-free **select-chain** on the tag (sound
  because the language is pure — no phi/blocks needed). Differential-gated vs the
  host on two- and three-variant programs across all branches
  (`tests/smoke_test_phase118.sh`).

- ✅ **Phase 119 (adversarial review + fixes, done)** — a 3-way review (~80
  valid programs vs the host, IR validity via clang/llc, test honesty) found one
  real bug: a `match` whose ARMS return enum values lowered its select-chain as
  `i64` instead of the aggregate type (clang-rejected; the host compiled it). Fixed
  to use the arm result type (mirroring the `if` lowering), plus a latent
  aggregate-return `main` fix (extract field 0 as the exit code). Both pinned by
  new regression cases. IR validity + test honesty came back clean.

**v20 is functionally complete:** the self-hosted compiler emits real native code
(115) that provably matches the host (116) for the i64/bool language, plus
**structs** (117) and **enums + match** (118) — the shapes kardashev itself is
built from, adversarially reviewed (119). Full kardashev-compiles-kardashev
remains several roadmaps out, but this is well past "toy".

### v21 — prove it, and close the leaks — *done (v0.21.0)*

Turn anecdotes into numbers and fix the real footprint gaps:

- ✅ **Phase 120 (benchmarks, done)** — `bench/` + `BENCHMARKS.md`: each workload
  written identically in kardashev and C, AOT-compiled (`kardc -O2` / `clang
  -O2`), run best-of-3 with output checked equal. Result: kardashev is
  **C-competitive** — `fib` ≈ 1.0×, `collatz` ≈ 1.0×, a tight integer `loop` ≈
  2.2× C. Correctness pinned by `tests/smoke_test_bench.sh`; perf ratios committed
  in `BENCHMARKS.md`. (Replaces the "-O2 default"/"flat RSS" anecdote with data;
  the ~2.2× tight-loop gap is a concrete codegen-opt target.)
- ✅ **Phase 121 (spawn/join frame leak, done)** — the one real leak measurement
  found: `spawn` + `join` leaked a heap frame per spawned task (the executor task
  array grew unbounded), because `join` drove + read the result but never
  reclaimed the task (unlike `block_on`, which reaps). A naive reap-after-join is
  *wrong* — driving one handle also completes sibling tasks (the executor
  interleaves), so an all-done reap frees a sibling's result before its own
  `join` reads it. Fixed with a **per-handle release** (`__kd_exec_release(h)`):
  free only task `h`'s frame+slot, resetting the executor only once every task is
  released. Now a spawn+join loop is RSS-flat and multi-handle joins return the
  right distinct results; pinned by `tests/smoke_test_spawnleak.sh`. *(HashMap
  interior-K/V drop and block_on/await frame reclaim were measured clean.)*
- ✅ **Phase 122 (HashMap/HashSet `remove`, done)** — the one genuinely-missing
  stdlib operation. Open-addressing deletion is done by **backward-shift**
  (Knuth Algorithm R) rather than tombstones, so `get`/`insert`/`grow` stay
  untouched: the rest of the probe chain is shifted into the hole, keeping the
  table tombstone-free (every live key stays reachable from its home by a
  contiguous run, so there is no load-factor or infinite-probe regression).
  `hashmap_remove<K,V>` returns `Option<V>` with the value **moved out** (the
  stored key + lookup key dropped); `hashset_remove<T>` returns a `bool`. Pinned
  by `tests/smoke_test_hashremove.sh`: head/middle/tail + wrap-around chain
  preservation, a 50-key oracle, and heap-clean String-map remove + 200k churn
  under `MALLOC_CHECK_=3` (RSS-flat).
- ✅ **Phase 123 (generic `Mutex<T>`, done)** — the headline `i64`/`bool`-MVP
  surface lifted: the `Mutex` guarded cell was `i64`-only and is now an
  arbitrary `T` (guard a struct, `String`, `bool`, `Vec`, … — including shared
  across threads). `mutex_new`/`get`/`set` are specialized per cell type over a
  `{ pthread_mutex_t, T }` block; the i64 handle stays Copy + shareable, so it is
  fully backward compatible (`mutex_new(0)` infers `T=i64`). `get` clones the
  cell and `set` drops the old value (a `Mutex<String>` over 100k sets is
  RSS-flat). Follows the handle-based `join<T>` idiom — `T` is type-erased
  through the i64 handle, so `mutex_get<T>` (T return-only) is pinned by context
  or an explicit annotation. Pinned by `tests/smoke_test_mutex_generic.sh`
  (bool/struct/i64 cells, heap-clean `Mutex<String>`, and a `Mutex<struct>` across
  two threads → exact total). *(A fully type-safe named `Mutex<T>` with `T`
  inferred from the handle is a larger Send/Copy/capture change — deferred
  honestly; the other handle-based surfaces, OS-thread return value and the
  const-eval scalar set, remain `i64`/`bool`-MVP and are documented as such.)*

### v22 — ergonomics, docs, and platform hygiene — *done (v0.22.0)*

- ✅ **Phase 124 (`||` logical-or, done)** — short-circuit logical-or, resolving
  the collision with the zero-param closure `|| body`. Disambiguation is
  positional: `||` is the `PipePipe` token — logical-or in INFIX position (after
  an operand) and a closure in PRIMARY position — so the precedence loop and
  `parsePrimary` never alias. `||` binds looser than `&&` (the precedence ladder
  shifts up one tier); lowering mirrors `&&`'s short-circuit branch+phi, flipped
  (lhs true skips rhs and yields true). Pinned by `tests/smoke_test_phase124.sh`
  (truth table, dead-rhs `10/0` skipped, precedence both directions, closure
  intact) + parser (precedence) and codegen (short-circuit) unit cases.
- ✅ **Phase 125 (`&<temporary>`, done)** — `&<rvalue>` (`&A(10)`, `&5`, a struct
  literal, an arithmetic temp, a nullary variant `&Nil`) had no storage to point
  at and errored; now it materializes the value into an entry-block slot (one
  slot reused across loop iterations, like a `let`), registers a droppable
  temporary for scope-exit drop, and borrows the slot. Drop safety is the
  load-bearing part: a `&Text(int_to_string(i))` (an enum owning a heap String)
  in a 500k loop drops exactly once — RSS flat at ~2 MB, `MALLOC_CHECK_=3` clean.
  `&()` (a unit value has no storage) stays a clean codegen error, and the void
  guard fixes a crash a unit-returning call hit on the new path. Pinned by
  `tests/smoke_test_phase125.sh`; `smoke_test_diag` repoints to `&()`.
- ✅ **Phase 126 (docs reconciliation, done)** — the language reference and stdlib
  docs listed `%`, `&&`/`||`, `&` of a literal/temporary, and enum-typed struct
  fields as deliberate limitations; all four compile (Phases 33 / 36 / 124 / 125).
  The honesty note, the lexical-structure table, the enum-field section, the
  surface-limitations list, and the stale "Roadmap v5" version headers are
  brought in line with the implementation and the test suite. doclint stays green.
- ✅ **Phase 127 (macOS flake residual, done)** — the macOS-arm64 `codegen_test`
  ORC-JIT teardown abort (~50%/run, confirmed non-deterministic; root cause needs
  macOS-arm64 hardware this environment lacks) is raised from 3 to 5
  `--flaky_test_attempts`, scoped by regex to that one target so a real
  regression anywhere else is never retried/masked (~12.5% → ~3% residual). The
  test is deterministic on Linux, so a genuine regression still fails all 5
  attempts and reddens CI.

### v23 — a second backend (and the leftover platform reach)

The v22 "second platform/backend" item is its own roadmap: a full second code
generator is comparable in scope to the LLVM backend it parallels, so it is
broken out here rather than rushed into a single phase.

- **A C-source backend (`--emit-c`), recommended first.** Walk the same
  typechecked AST that `codegen.cpp` lowers and emit portable C, compiled by the
  system C compiler. It is the most *verifiable* option in this environment (a C
  toolchain is present), so it can be **differentially gated** against the LLVM
  backend exactly as the self-hosted compiler already is — the existing
  arithmetic / control-flow fuzzers (Phases 110–111) become the oracle
  (`emit-c` output == LLVM AOT output == reference). Scope it as a SUBSET first —
  i64/bool, arithmetic / comparison / bitwise, `&&`/`||`, `if` / `while`,
  functions + recursion — then grow it (structs → enums + match → strings / Vec →
  Drop) phase by phase, each differentially green, before claiming portability.
  This breaks the LLVM/Linux monoculture for a real subset without shipping a stub.
- **WASM** and a **Windows target** are the follow-on reach once the C backend
  proves the AST→target seam. WASM needs a committed runtime in CI (wasmtime or
  node) so it can be differentially tested the same way; the Windows port is
  mostly toolchain/ABI work (PE objects, the `clang-cl`/MSVC driver, path
  handling) layered on the existing LLVM backend rather than a new code generator.

## Deferred (honest — documented, not stubbed)

- **Third-party package registry** (resolution via the Bazel module registry) —
  Bazel can't run in this build environment, so a real registry integration
  isn't verifiable here; only `mod foo;` + `kard.toml` local-path deps ship.
- **A normative language spec + conformance suite**, and a **stability / semver
  policy** — appropriate once the language stops changing core semantics (it is
  still pre-1.0).
