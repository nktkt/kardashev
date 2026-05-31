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
- **MVP / leaky stdlib.** `HashMap`/`HashSet` have no `remove` (deferred —
  open-addressing deletion needs tombstone-aware probing) and do **not** drop
  interior keys/values (a documented leak, no use-after-free); a completed async
  `Future`'s heap frame is never reclaimed (long-running async leaks frames);
  the const-eval scalar set and some library surfaces are still `i64`/`bool`-MVP.
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

### v21 — prove it, and close the leaks

Turn anecdotes into numbers and fix the documented soundness/footprint gaps:

- A **benchmark suite**: compile-time + runtime of the capstones (`json`,
  `wordfreq`, `matrix`, …) vs a C/Rust reference, with **committed numbers** —
  replacing the "-O2 default" / "flat RSS" anecdotes with reproducible data.
- Fix the **documented leaks**: drop `HashMap`/`HashSet` interior keys/values;
  add `remove` via tombstone-aware probing; reclaim completed async `Future`
  frames in the executor.
- Generalize the remaining `i64`/`bool`-MVP surfaces (library element types,
  const-eval scalars) toward arbitrary types.

### v22 — ergonomics, docs, and platform

- **`||` logical-or** (resolve the closure-syntax collision); **`&<temporary>`**
  (materialize an rvalue into a statement-scoped, dropped slot) and the related
  `&A(10)` ref-to-enum-literal miscompile.
- **Reconcile the docs with reality** — the language reference still claims `%`,
  `&&`, and enum-typed struct fields are unsupported when they work; bring the
  reference in line with the implementation and the test suite.
- The macOS `codegen_test` arm64 JIT-teardown **flake**: root-cause it (needs a
  macOS-arm64 environment) or raise `--flaky_test_attempts` to cut the residual.
- Explore a **second platform or backend** (Windows, or a WASM/C portability
  backend) to break the LLVM/Linux-leaning monoculture.

## Deferred (honest — documented, not stubbed)

- **Third-party package registry** (resolution via the Bazel module registry) —
  Bazel can't run in this build environment, so a real registry integration
  isn't verifiable here; only `mod foo;` + `kard.toml` local-path deps ship.
- **A normative language spec + conformance suite**, and a **stability / semver
  policy** — appropriate once the language stops changing core semantics (it is
  still pre-1.0).
