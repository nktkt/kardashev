# Kardashev — The Road to 1.0 and Beyond

*Sequencing every dimension from its current 2–3/5 to 6/6, and making the language
genuinely shippable for products.*

This document continues from `v0.36.0` (the numbered roadmap v1–v36 is released).
It is the lead-architect sequencing of the **nine design dimensions** into versioned
releases v37 → 1.0 → 1.x.

> **A note on dimension counting.** The underlying gap survey
> (`docs/roadmap-enumeration.md`) is organized as **seven source dimensions plus a
> completeness-critic pass** (174 items total). For *roadmap sequencing* we track
> **nine dimensions** — the seven source dimensions split into the axes that move
> independently and gate each other: type-system `[TS]`, memory-safety `[MS]`,
> concurrency `[CC]`, async-effects `[AE]`, metaprogramming `[MP]`, stdlib `[SL]`,
> tooling `[TL]`, backends-perf `[BP]`, maturity-ecosystem `[EC]`. "Every dimension
> to 6/6" therefore means **all nine** below. (The source survey's seven correspond
> 1:1 except that survey-dimension "Memory, ownership, effects & concurrency" is
> tracked here as the three axes MS/CC/AE, and "Backends & performance" plus
> "Self-hosting → bootstrap" are folded into BP/EC.)

---

## 1. Goal & strategy

**Goal.** Take every one of the **nine dimensions** to **6/6** — meaning kardashev
first reaches full **Rust-class parity** on each axis (5/5), then ships, on top of
that, **one differentiating capability per dimension that no single production
language offers out of the box**, each one gated by a CI conformance test (not
prose). At the same time, cross a hard **PRODUCTION-READINESS GATE** (§4): a 1.0
spec + conformance suite, a working package ecosystem with a toolchain distribution
+ MSRV story, a platform matrix that includes Windows + WASM + a freestanding
`no_std` target with prebuilt-or-build-from-source std per target, mature C FFI
(struct-by-value, callbacks, pointer arithmetic, header import), a committed panic
strategy, an observability facade, **application-benchmark perf within 1.1× of C**, a
multi-threaded executor (TSan-gated), a stability policy, a debugger, self-verifying
incremental compilation, and a full security/supply-chain story.

**Memory-model thesis (stated up front, because gate #11 depends on it).**
Kardashev is **RAII + ownership + `Arc`/`Rc` for shared ownership — there is no
tracing garbage collector**, by design. Deterministic destruction (`Drop`) is the
memory model. The one consequence we name explicitly: **reference cycles of `Arc`
(or `Rc`) are a *safe-subset leak*** — exactly as in Rust — broken by using `Weak`
for back-edges. This is the only class of leak the safe subset permits, it is *not*
undefined behavior, and §4 gate #11 is written to account for it (a curated,
documented, justified leak-allowlist of cycle fixtures, not "zero leaks of any
kind"; see gate #11 and the LSan-vs-Arc-cycle reconciliation there).

**Strategy — parity first, then beyond, leverage-ordered.**

1. **Unblock everything first.** A few items are prerequisites for whole
   dimensions: **named lifetimes + region inference (NLL)** unblocks the rest of
   the type system, all of memory-safety, and borrow-capturing concurrency; **mature
   FFI + `no_std` + the allocator trait** unblock the entire systems/embedded use
   case (enumeration HIGH-leverage #4 and #5); the **package registry +
   manifest/lockfile + toolchain manager** unblock the ecosystem; the **normative
   spec** unblocks conformance, stability, and the mechanized-spec beyond-work; the
   **query engine** unblocks incremental compile and the unified IDE server; the
   **benchmark-regression harness** gates "shippable"; the **TSan CI job** gates the
   work-stealing executor. These land early.
2. **Parity before beyond, per dimension.** Every `[beyond/*]` phase depends on its
   dimension's `[parity/*]` phases; we never build a differentiator on an
   incomplete base. The 6/6 capabilities (totality effect, Miri-gate, static
   deadlock-freedom, multi-shot effects, proc-macros, fusing iterators,
   time-travel debug, dual-bound perf gate, mechanized spec, reproducible registry,
   real bootstrap, mechanized soundness) are sequenced **after** their parity floor.
3. **Mega-arcs run as their own milestones** (§3), each with explicit entry/exit
   criteria, woven into the versions where their dependencies are satisfied.
4. **Every promise is a CI gate.** Each phase below carries its objective
   acceptance criterion verbatim from the design tracks. A version is "done" only
   when all its gates are green on both CI platforms (and, where stated, the TSan
   and freestanding-target jobs).

**Dimension shorthand:** `[TS]` type-system, `[MS]` memory-safety, `[CC]`
concurrency, `[AE]` async-effects, `[MP]` metaprogramming, `[SL]` stdlib, `[TL]`
tooling, `[BP]` backends-perf, `[EC]` maturity-ecosystem. Each phase is tagged
`parity|beyond` and `S/M/L/XL`.

---

## 2. The versioned sequence (v37 → 1.0 → 1.x)

> Ordering rule: a phase appears in the earliest version where all its deps are
> already shipped. Early versions are biased to **unblock** (lifetimes, FFI, no_std,
> registry, toolchain manager, spec, query engine, perf harness, sanitizer gate
> incl. TSan); late versions deliver the 6/6 differentiators and the formal/proof
> work.

### v37 — "Foundations & unblockers" — moves: TS, MS, CC, BP, TL, MP

The cheap, dependency-free wins that other phases stand on, **plus the two CI jobs
that gate later concurrency and the panic policy that affects the ABI from here on.**
No XL work; everything here is `parity` and independently testable.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Turbofish + explicit generic args (`f::<T>`, `Vec::<i64>::new()`) | TS | parity/M | `smoke_test_turbofish.sh`: ≥15 inference-insufficient programs JIT==AOT; ≥6 negatives (wrong arity, conflict) stable error code |
| Global sanitizer gate (ASan+LSan+UBSan, **both** backends) | MS | parity/M | `ci.yml` required `sanitizers` job ubuntu+macos; **all ≥186 smoke tests + memsafety fuzzer** clean; merge-blocking |
| **ThreadSanitizer CI job** (the executor's prerequisite gate) | CC | parity/M | `ci.yml` required `tsan` job ubuntu+macos builds the runtime + all concurrency smoke tests under `-fsanitize=thread`; **currently-green** v31/v32 concurrency corpus runs TSan-clean; merge-blocking; a seeded data-race fixture turns it red |
| **Panic strategy: `panic=unwind` default + `panic=abort` flag + `catch_unwind` + FFI unwind-boundary contract** | MS | parity/L | `smoke_test_panic_policy.sh`: a panic across an `extern "C"` boundary without an `abort` shim is a **compile error** (FFI unwind-boundary lint); `catch_unwind` catches a panic and runs every pending `Drop` exactly once (ASan + drop-count clean); `-Cpanic=abort` produces a binary with no landing pads (FileCheck-on-IR) and `catch_unwind` is a compile error under it; the chosen default (`unwind`) is documented in the spec ABI clause |
| **Release-build integer-overflow policy re-affirmed + selectable** | MS | parity/S | `smoke_test_overflow_policy.sh`: default debug build **traps** on overflow (UBSan-clean = intentional trap, not UB); `-Coverflow=wrap` gives two's-complement `-fwrapv` semantics; `-Coverflow=trap` traps in release too; the default for `--release` is documented and pinned by a test (a `i64::MAX + 1` fixture aborts in debug, wraps under `-Coverflow=wrap`) |
| Complete the borrow checker (two-phase mut-2nd-arg, field reborrows) | MS | parity/L | borrowck conformance corpus ≥120 (≥60 accept / ≥60 reject w/ exact code), 100% classify; the two deferred shapes flip to accept |
| Full operator-trait surface (Index/Deref/Neg/Not/Rem/bitops, custom Output) | MP | parity/M | `smoke_test_operator_overload2.sh`: ≥14 programs LLVM-exit==C-exit; missing-impl still diagnoses |
| Benchmark-regression CI harness (kill hand-committed ratios) | BP | parity/M | `bench_regression` green; ratio-to-C within baseline±tol (loop 2.2×, others 1.1×); fails on injected 20% slowdown |
| Real test framework (assert!/eq!/ne!, filter, parallel, `--format=json`) | TL | parity/L | `smoke_test_testframework.sh`: pass/fail/should_panic, `--filter`, JSON schema-checked, exit 0 iff all pass |

**Unblocks:** turbofish → where-clauses/GATs; sanitizer gate → leak-closure +
unsafe surface; **TSan gate → the v40 work-stealing executor's "race-free" claim is
now verifiable**; **panic policy → FFI safety (v41) + the no-double-drop story +
the ABI clause of the spec (v45)**; perf harness → app-scale benches + dual-bound
gate; operator surface → metaprogramming parity.

---

### v38 — "The type system, completed I (the lifetime spine)" — moves: TS → 4

The headline parity gap and its prerequisites. This is the load-bearing version
for memory-safety and concurrency downstream.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| where-clauses on associated types + projected bounds | TS | parity/L | `smoke_test_where_assoc.sh`: ≥12 projected-bound programs JIT==AOT; ≥8 negatives stable code |
| Full GATs: bounded-Self / generic-param projection (`C::Out<i64>`) | TS | parity/L | `smoke_test_gat_bounded.sh`: generic `fn f<C: Container>->C::Out<i64>` + LendingIterator, ≥3 instances each; previously-deferred case passes |
| Object safety + complete `dyn`-trait dispatch (+ supertrait upcast) | TS | parity/L | `smoke_test_object_safety.sh`: ≥10 object-safe dispatch all methods; ≥8 non-object-safe rejected naming the member; ≥3 upcast |
| Variance inference for type parameters | TS | parity/M | `--variance-report` matches a hand-checked oracle on ≥20 fixtures; ≥6 unsound coercions rejected |
| **Named lifetimes + region inference (NLL)** | TS | parity/XL | `tests/conformance/regions/` ≥120 programs, accept/reject vs oracle ≥99% (100% on frozen subset); 52 borrow units stay green; lifetimes erased (IR-diff confirms no codegen change) |

**Dim move:** type-system reaches **5/5 parity floor on the region story** (the
remaining 5/5 piece — let-generalization/HRTB — lands in v39). **Unblocks:** real
borrow-check (MS), borrow-capturing scoped threads (CC), drop-check.

---

### v39 — "FFI maturity, no_std & async parity I" — moves: SL/EC (FFI+no_std), CC, AE, TS

With lifetimes in place we (a) finish the **systems-language unblockers the v33
roadmap deferred** — mature C FFI and `no_std`/freestanding/allocator — and (b)
finish the parallel runtime primitives. FFI + no_std are sequenced here because they
gate the platform matrix (v44 freestanding target) and the FFI unwind contract
(panic policy, v37) is already in place.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Let-generalization + higher-rank (`for<'a>`) where it pays | TS | parity/L | `smoke_test_hrtb.sh`: ≥10 HRTB programs JIT==AOT; ≥6 need rank-2; ≥4 ill-typed rejected; ≥200-program principal-types fuzzer |
| **FFI maturity I: `repr(C)` struct-by-value across `extern "C"`, C callbacks (fn-ptr ABI), raw-pointer arithmetic in FFI** | EC | parity/L | `smoke_test_ffi_struct.sh`: a `repr(C)` struct passed/returned by value to a C function (compiled by `cc`) round-trips field-for-field; `smoke_test_ffi_callback.sh`: a kardashev `fn` passed as a C callback (`qsort`-style) is invoked with correct ABI and results match; `smoke_test_ffi_ptrarith.sh`: pointer arithmetic over a C array (`*const T` + offset) reads correct elements; **all three ASan+UBSan-clean**; ABI pinned by an `llvm-objdump` signature check; out-of-FFI-subset constructs refused |
| **FFI maturity II: `bindgen`-equivalent header importer (`kard bindgen foo.h`)** | EC | parity/L | `smoke_test_bindgen.sh`: importing a real `<math.h>`/`<string.h>` subset header generates `extern "C"` decls + `repr(C)` structs that compile and call libc correctly (`strlen`/`memcpy`/`pow` results match C); generated bindings are deterministic (byte-identical re-run); a malformed header fails cleanly with a diagnostic, never a crash |
| **`no_std` / freestanding mode + pluggable `GlobalAlloc` trait** | SL | parity/L | `smoke_test_no_std.sh`: a `#![no_std]` program builds and runs against a **user-supplied** `GlobalAlloc` impl (a bump allocator fixture) with **zero libc-malloc symbols** in the binary (`nm` check); core (`Option`/`Result`/slices/`Iterator`/arithmetic) works without std; using a std-only API under `no_std` is a **compile error** naming the item; an `extern` panic handler + `#[no_std] fn main`-equivalent entry is supported; built **as a CI matrix leg** |
| Generic `thread_join<T>` — retire i64-only OS-thread return | CC | parity/L | typed `JoinHandle<T>` over i64/struct/String/Vec returns exact value JIT==AOT; 1000-iter loop RSS-flat LSan-clean; non-Send result = compile error |
| Cross-platform reactor: kqueue (macOS/BSD) + poll() fallback | AE | parity/L | `smoke_test_reactor.sh`: epoll/kqueue identical stdout+exit on both CI; forced `KD_REACTOR=poll` passes on ubuntu; sleep(500ms) <5ms CPU all three |
| Blocking multi-wait select + async Mutex/RwLock/channel | CC/AE | parity/L | 4-prod/1-cons select consumes 1M msgs at <5% CPU when idle (getrusage); 8 tasks×100k on async-Mutex == 800,000, deterministic over 50 runs |
| Recursive Future-drop: sound structured cancellation | AE/MS | parity/L | `smoke_test_cancel.sh` under MALLOC_CHECK_+ASan: 50k tasks holding heap, cancel half mid-flight, **zero** leaked bytes RSS-flat; timeout releases a held Mutex guard |

**Dim move:** the systems-language unblockers (mature FFI + no_std + allocator
trait) land — **the embedded/kernel/C-interop use case is now reachable**, retiring
the v33 deferrals (Phases 179/180); async-effects nears 5/5 (multi-thread executor
in v40 completes it); concurrency advances on join + async-aware primitives.
**Unblocks:** the freestanding platform leg (v44 gate #5), the OpenSSL/libc-class
real-package use case (arc B), the FFI unwind-boundary contract (already enforced
since v37) now has real cross-language fixtures.

---

### v40 — "The parallel executor & structured concurrency" — moves: CC, AE → 5/5

The deferred Phase 174 and the structured-concurrency capstone. **Every "race-free"
/ "deterministic" claim below runs under the v37 TSan job in addition to ASan.**

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Multi-threaded work-stealing executor + cross-platform reactor | CC | parity/XL | 100k CPU-bound tasks scale near-linear (4-worker within 1.4× of single/4); 4×100k steal-stress exact total deterministic over 200 runs **and TSan-clean (v37 `tsan` job)**; macOS kqueue I/O green |
| Multi-threaded work-stealing executor (the deferred Phase 174) | AE | parity/XL | `smoke_test_executor_mt.sh`: divide-and-conquer over 10M elems ≥2.5× speedup vs single-thread; 100k fan-out leak-free deterministic over 50 runs **and TSan-clean**; non-Send closure = compile error; ubuntu+macos |
| Blocking multi-channel select via shared waker (retire poll-backoff) | AE | parity/M | `smoke_test_select.sh`: selecting consumer CPU <10ms vs >100ms backoff; select2/3/4 still JIT==AOT; 4-prod fairness within 10%; TSan-clean |
| Structured concurrency: borrow-capturing scoped threads + async scope + recursive cancel-drop | CC | parity/XL | scoped thread borrows stack `Vec` by `&mut` across 4 workers; removing scope = borrow error; cancel of 1000-child parent frees every frame RSS-flat over 100 cycles; TSan-clean |
| Structured concurrency: async `scope` + cancellation tokens | AE | parity/L | `smoke_test_async_scope.sh`: 1000 children, one fails → all cancelled within one tick (AtomicI64 reaches 0); child can't escape scope (compile error); leak-free; task count returns to baseline; TSan-clean |

**Dim move:** **concurrency 5/5** (Send/Sync + work-stealing + scoped + lock-free
+ async-aware sync, all TSan-gated) and **async-effects 5/5** (parallel executor +
sound structured cancellation). *Note on the async-effects parity floor:* the
**5/5 floor is fully met at v40** — typed/untyped algebraic handlers already shipped
in v32, and v40 completes the runtime (parallel executor + structured cancellation).
The **typed-rows-end-to-end + multi-shot** work is **6/6 (beyond)** and is scheduled
in v43; it is *not* part of the 5/5 floor and the §6 trajectory marks AE 5/5 at v40,
AE-6-begins at v43. (This corrects the earlier forward-dependency.)

---

### v41 — "Memory safety, parity complete + unsafe surface" — moves: MS → 5/5

Close the documented leaks, add real region inference to the borrow checker, and
finish the unsafe escape hatch with a written aliasing contract. (The FFI raw-write
+ pointer-arith *across language boundaries* already landed in v39; this phase is
the **safe-unsafe in-language surface** and the aliasing model that both share.)

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Close documented leaks: recursive Future-drop + escaping/early-return heap locals | MS | parity/L | `smoke_test_futuredrop.sh` RSS-flat over 1M iters ASan-clean; nested-block + early-return cases in `smoke_test_dropleaks.sh` ASan-clean both backends; **the intentional-leak allowlist is reduced to *only* the documented `Arc`/`Rc`-cycle fixtures (see gate #11), each with a justification comment; all other entries == 0** |
| Lifetime params + real region inference (replace position-counting) | MS | parity/XL | borrowck conformance corpus → ≥250 cases incl. lifetime-parametric APIs + NLL classics, 100% classify; scope_spawn borrow-captures a stack local with verified-sound bound (ASan-clean) |
| Finish unsafe (in-language): raw write (`*p=v`), pointer arithmetic, aliasing model | MS | parity/L | `smoke_test_rawptr.sh` (`*p=v`, ptr arith round-trip, `copy_nonoverlapping`) LLVM==C ASan+UBSan-clean; `docs/aliasing-model.md` ≥15 worked examples tagged sound/UB; **the model is consistent with the v39 FFI pointer rules (one aliasing contract for both)** |

**Dim move:** **memory-safety 5/5** — safe subset is sanitizer+fuzzer-gated end to
end, unsafe surface is complete with a normative contract, FFI shares one aliasing
model (the Miri-gate and proof are the 6/6 work in v47/v50).

---

### v42 — "Stdlib depth I: collections, iterators, I/O, time, observability" — moves: SL → 5 (in progress)

Real data structures, a deterministic-testable runtime surface, **and the
observability facade** (`log`/`tracing`-class structured logging + spans + metrics),
which "ship a product on it" requires. All parity-level; the beyond-stdlib work
(fusing iterators, serde, contracts, replay) follows once these land.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Balanced ordered collections (real BTreeMap/BTreeSet) | SL | parity/L | `smoke_test_btree_perf.sh`: comparison-count <3·log₂(n) at n∈{1k,10k,100k}; JIT==AOT; ASan-clean over 200k churn; old sorted-Vec marker grep-rejected |
| Leak-free HashMap/HashSet (interior K/V Drop) | SL | parity/M | `smoke_test_hashmap_drop.sh`: 500k insert/overwrite/remove on `<String,String>` + `<i64,Vec<i64>>` zero leaked bytes (ASan), MALLOC_CHECK_=3 clean, JIT==AOT |
| Lazy iterator core trait + fundamental adaptors | SL | parity/L | `smoke_test_iter_lazy.sh`: `range(0,1M).map.filter.take(10).collect()` ≤2 heap collection allocs regardless of length; results == eager; JIT==AOT |
| Buffered I/O, files, stdin/stdout, env/process (Phase 189) | SL | parity/L | `smoke_test_io.sh`: 1MB temp-file round-trip byte-for-byte; N lines from fixed stdin; in-memory sink == fd sink; ASan-clean; JIT==AOT |
| Time, Duration & monotonic clock | SL | parity/M | `smoke_test_time.sh`: fake-clock elapsed deterministic JIT==AOT; Duration arithmetic table-matches; real-clock monotonic over 1000 samples |
| Networking: TCP/UDP + minimal HTTP client | SL | parity/L | `smoke_test_net.sh`: loopback echo 64KB byte-for-byte; HTTP GET parses status+headers+body; JIT==AOT; ASan-clean; fully offline |
| **Observability facade: structured logging + tracing spans + metrics counters** | SL | parity/M | `smoke_test_observability.sh`: a pluggable `Subscriber` captures structured key/value log records and span enter/exit events in order; level filtering (`KD_LOG=info`) drops debug records; a `counter!`/`histogram!` metric reads back the expected aggregate; the facade is **no-op zero-cost when no subscriber is installed** (FileCheck-on-IR: a disabled `trace!` lowers to nothing); JIT==AOT; works under `no_std` with a user subscriber |

**Dim move:** stdlib advances toward 5/5 (regex in v43 completes the parity floor;
observability is now a first-class stdlib API distinct from the v48 *determinism*
replay surface).

---

### v43 — "Metaprogramming parity + stdlib regex + typed/multishot effects (AE 6 begins)" — moves: MP → 5, SL, AE → 6 (begins)

Hygiene, nested repetition, span diagnostics, built-in helper macros, comptime
depth — the Rust-grade macro floor. Plus the **first async-effects 6/6 work** (typed
rows end-to-end + multi-shot), which the v40 parity executor now supports. *This is
where the AE differentiator work begins; the AE 5/5 parity floor was already
complete at v40.*

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Macro hygiene via gensym + syntax contexts | MP | parity/L | `smoke_test_hygiene.sh`: ≥15 adversarial capture programs hygienically correct, LLVM-exit==C-exit; ≥8 baseline-fails flip to pass |
| Nested repetition + metavar-after-repetition | MP | parity/L | `smoke_test_macro_repetition.sh`: 2-D `mat!`, `hashmap!`, `$(…)* $tail:expr`; ≥12 LLVM==C; mismatched-count = span error not crash |
| Span-accurate macro diagnostics | MP | parity/M | `smoke_test_macro_diag.sh`: ≥10 programs, caret lands on invocation line; `--explain-expansion` backtrace; zero negative/out-of-file lines |
| Built-in helper macros (stringify!/concat!/concat_ident!/count!/cfg!) | MP | parity/M | `smoke_test_builtin_macros.sh`: ≥2 cases each; `concat_ident!` generates callable getters; `cfg!` correct under two `--cfg` configs |
| comptime over full value/type space (const generics, const trait dispatch, const collections) | MP | parity/L | `smoke_test_comptime_full.sh`: const lookup table, const String length as array length, const trait-method drives const-generic; ≥12 const==runtime + LLVM==C; non-terminating const loop fails budget |
| Regex engine (linear-time NFA/DFA) | SL | parity/XL | `smoke_test_regex.sh`: 100-case conformance table 100% pass; pathological `a?ⁿaⁿ` <50ms at n=50; JIT==AOT |
| Effect rows in signatures, machine-checked end-to-end | AE | beyond/L | ≥60 effect-typing programs: undeclared/unhandled effect fails to compile (no runtime Unhandled); well-typed handlers JIT==AOT; fuzzer: compiles iff every perform is discharged |
| Multi-shot resumptions, memory-safe under the borrow checker | AE | beyond/XL | `smoke_test_multishot.sh`: `choose` resumed twice == reference enumeration; generator yields N lazily; resuming a captured-moved-non-Copy local = compile error; N-queens N=10 == 724 solutions, zero leaks over 100k resumptions |

**Dim move:** metaprogramming reaches **5/5 parity** (hygiene + full matcher +
spans + helpers + comptime). async-effects **begins its 6/6 ascent** (typed rows +
multi-shot, both beyond OCaml 5 / Koka); the AE 5/5 parity floor was completed at
v40.

---

### v44 — "Backends & platforms: perf, cross-compile, WASM, Windows, freestanding" — moves: BP → 5/5

The "shippable everywhere" version. Closes the perf gap to **parity-level on
applications**, adds cross-compilation **with a real per-target std story**, and
lands the WASM + Windows + freestanding targets (Mega-arc C, §3).

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Application-scale benchmark suite (beyond micro) | BP | parity/L | 6–10 workloads output-identical to C/Rust refs; committed baselines; BENCHMARKS.md regenerated from harness (CI fails on hand-edit divergence) |
| **Close the gap to parity: ≤1.3× on the tight loop AND ≤1.1× geomean on the application suite** | BP | parity/L | `loop` ratio-to-C ≤1.3× (from 2.2×) via alloca-free counters + signed-div strength reduction (FileCheck-on-IR: zero alloca/store for induction var, division strength-reduced); **AND the 6–10-workload application suite geomean ratio-to-clang-`-O2` ≤1.1× (committed `bench/app_baseline.json`)** — this is the **application-level 1.1× parity gate that §4 #6 requires *before* 1.0** (the fusing-pipeline 1.1× is a separate, additional v48 *beyond* result on a synthetic pipeline, not the 1.0 gate) |
| First-class LTO + verified `-Oprofile` (PGO) | BP | parity/L | CI builds an app-scale bench with `-Oprofile` + `--lto`; PGO ≥5% faster on branch-heavy OR not slower; byte-identical output to -O2 |
| Cross-compilation infrastructure (host/target triples) + **per-target std/sysroot story** | BP | parity/L | cross-compile smoke corpus x86_64→aarch64-linux, correct datalayout (llvm-objdump), exit codes match native under qemu-user; bogus triple fails cleanly; **a documented `target → {sysroot, libc}` table; the std cross-build matrix builds std for {x86_64-linux, aarch64-linux, wasm32, x86_64-windows, the freestanding `no_std` target}; the policy "which targets ship a prebuilt std vs build-from-source" is committed (`docs/targets.md`) and the prebuilt-std targets are produced by CI and checksummed** |
| **WASM backend**, differentially gated under a committed CI runtime | BP | parity/XL | `smoke_test_wasm.sh`: i64/bool/struct/enum corpus under wasmtime/node == LLVM-AOT exit+stdout; hard CI gate; out-of-subset refused; **builds the wasm32 std leg from the cross-build matrix above** |
| **Windows/PE target** (clang-cl / MinGW ABI) | BP | parity/XL | scalar+struct+string corpus on windows-runner or wine == Linux LLVM-AOT; FFI round-trip pins ABI; **the x86_64-windows std leg builds in the matrix** |
| **Freestanding (`no_std`) target leg in the platform matrix** | BP | parity/L | the v39 `no_std`+`GlobalAlloc` program cross-compiles to the freestanding target and runs under qemu-system (or a CI emulator) producing the expected exit code with **zero libc dependency** (`nm`/`ldd` check); a std-only API for this target is a compile error |

**Dim move:** **backends-perf 5/5** — multi-target (x86/ARM/WASM/Windows/freestanding),
cross-compile with a real per-target std/sysroot story, LTO+PGO, **application perf
at parity (≤1.1× geomean)**. (The dual-bound gate, codegen contracts, and the
universal-C-reach 6/6 work are in v48.)

---

### v45 — "The ecosystem foundation: registry, toolchain manager, spec, bootstrap begins" — moves: EC → 4

The big unblockers for maturity. Two Mega-arcs (B registry, D spec) open here; the
bootstrap (A) begins its **frozen-target-subset** staged corpus (see §3 arc A —
the fixpoint target is a frozen subset, not the moving full language).

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| Package manifest, resolver, lockfile (Cargo.lock parity) + **MSRV/toolchain-range in the manifest, enforced by the resolver** | EC | parity/L | `smoke_test_resolver.sh`: diamond resolves to one version byte-identically; re-resolve deterministic; corrupt hash → non-zero; unsatisfiable range reported; **a package declaring `kardc = ">=1.0, <1.3"` is rejected by a `kardc 0.x` and accepted by an in-range `kardc`; a dependency whose MSRV exceeds the active toolchain is a resolver error naming the package; the manifest can pin a toolchain (`toolchain = "1.1.0"`)** |
| **Toolchain distribution + version manager (`rustup`-equivalent: `kardup`)** | EC | parity/L | `smoke_test_kardup.sh`: `kardup install 1.0.0` + `kardup default 1.0.0` selects a versioned `kardc`; `kardup override` pins a per-directory toolchain (read from the manifest `toolchain =`); installed toolchains are checksum-verified against the registry's signed manifest; offline `kardup` from a vendored bundle reproduces the same binaries (byte-identical) |
| Hosted package registry + `kard publish` + offline vendoring | EC | parity/L | `smoke_test_registry.sh`: publish→fresh-project resolve+build+run; `kard vendor` + `--offline` byte-identical to online; yank refused for new / kept for pinned; republish rejected |
| Normative language spec (EBNF + operational semantics + **ABI/panic/overflow clauses**) | EC | parity/XL | `smoke_test_grammar_conformance.sh`: 2000 EBNF-generated programs parse; ill-formed corpus rejected; every clause ID unique; grammar round-trips; coverage ≥95%; **the spec contains normative clauses for the panic strategy (v37), the integer-overflow release default (v37), and the C ABI/`repr(C)`/FFI-unwind contract (v37/v39), each with a stable clause ID linked to its gate** |
| Query-engine rearchitecture: salsa-style demand-driven memoized core | TL | beyond/XL | `query_engine_test`: byte-identical input → zero leaf-query recompute; one-body change re-runs <25% of queries; full 700+ unit + 186 smoke stay green |

**Dim move:** ecosystem reaches 4/5 (real dependency model + registry + toolchain
manager + MSRV + spec exists); tooling lays its incremental/IDE foundation.
**Mega-arc A (bootstrap)** formally opens its staged differential corpus here,
**against a frozen target-language subset** (§3).

---

### v46 — "Tooling depth + conformance + stability + security-response → the 1.0 surface" — moves: TL → 5/5, EC → 5

The IDE/test/debug surface and the conformance + stability + **security-response**
machinery that 1.0 requires.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| LSP feature completeness: semantic tokens, inlay hints, code actions | TL | parity/L | `smoke_test_lsp_rich2.sh`: ≥20-token semanticTokens match golden; ≥5 inlay hints; ≥1 code action/diag class applies → recompiles clean |
| Workspace-wide rename + cross-file references + file watching | TL | parity/L | `smoke_test_lsp_workspace.sh`: 3-file rename WorkspaceEdit touches all, recompiles to identical behavior; references == golden; prepareRename rejects keywords |
| **Debugger integration (LLVM backend): DWARF -g + gdb/lldb pretty-printers + backtraces** | TL | parity/XL | `smoke_test_debug.sh`: **on the LLVM backend**, breakpoint by line, print String == source, Vec elements, backtrace ≥3 named frames; DWARF line-table entry per executable line. **Scope is explicit: full source-level debugging is an LLVM-backend guarantee.** |
| **C-backend debug-info floor (`--emit-c -g`)** | TL | parity/M | `smoke_test_debug_cbackend.sh`: `--emit-c` emits `#line` directives mapping every generated C statement back to its kardashev source line, so a stock `gdb` on the cc-compiled binary breaks on and prints the **kardashev** source line/file (verified: a breakpoint set by kardashev `file:line` stops at the right place; a backtrace shows kardashev file:line). This is the **stated C-backend debug story** that reconciles "C fallback is a first-class platform" (§4 #5) with the debugger gate — line-level on the C platform, full DWARF pretty-printing on the LLVM platform. |
| Doctests + hosted docs site | TL | parity/M | `smoke_test_doctest.sh`: 1 pass / 1 fail doctest reported w/ file:line; HTML site page-count == public-symbol count; search-index schema-checked |
| Benchmark regression harness as a CI gate (perf gate v2) | TL | parity/M | `bench/regress.sh`: ratio worsens >10% vs `bench/baseline.json` → non-zero; CI red on a pessimized branch (demonstrated) |
| Conformance test suite + published pass-rate gate | EC | parity/L | `tests/conformance` 100% on LLVM (JIT==AOT); per-clause coverage committed per release; `--emit-c` runs in-subset, out-of-subset marked refused; CI fails if pass-rate <100% or coverage regresses |
| Stability policy: editions, SemVer enforcement, MSRV policy, 1.0 criteria | EC | parity/L | `smoke_test_semver.sh`: added-pub-fn → minor accepted; removed/renamed → breaking, patch bump rejected; edition-gated feature fails old / compiles new; **raising a published package's MSRV is classified as a breaking change by `kard semver-check`**; checklist links to green gates |
| RFC/governance + dependency audit & licensing + **security-response policy** | EC | parity/M | doclint: every accepted RFC links to its implementation; `smoke_test_audit.sh`: advisory dep → `kard audit` non-zero; GPL-under-MIT → `kard license` non-zero; clean graph passes both; **`SECURITY.md` exists with a CVE-disclosure + coordinated-response process; a published `kard audit` advisory schema; a documented embargo/patch SLA** (doclint-checked the file exists and links the advisory DB the resolver/`kard audit` consume) |
| **Compiler-hardening fuzzing (parser/typechecker/borrow-checker crash-resistance, i.e. DoS surface)** | TL | parity/L | `fuzz_compiler` (nightly cron + a per-PR smoke budget): ≥100000 malformed/adversarial **source inputs per run** through `kardc` front-end; **zero crashes/aborts/UBSan-or-ASan findings in the compiler itself** (a compiler crash on bad input fails CI); any crash auto-files a minimized repro; this is **distinct from** the memory-safety fuzzing of *generated programs* (v37/v47) |

**Dim move:** **tooling 5/5** (LSP depth + test framework + debugger on both
backends + doctests + bench gate + compiler-hardening fuzz); ecosystem 5/5
(registry + toolchain manager + spec + conformance + stability/MSRV + audit +
security-response).

---

### 🎯 v1.0 — "Production-ready" — the readiness gate closes

**v1.0 is declared only when every item in §4 (PRODUCTION-READINESS GATE) is
green** — including the **application-benchmark ≤1.1× parity gate** (#6, delivered
v44), the **TSan-gated multi-threaded executor** (#7), the **mature-FFI** (#13),
**no_std/freestanding** (#14), **panic/overflow policy** (#15), **observability**
(#16), and **toolchain/MSRV distribution** (#17). No new features land in 1.0 itself
beyond what's needed to close the gate; it is the stabilization + the formal 1.0
conformance/stability commitment. Editions freeze at `edition = "2026"`. The
conformance pass-rate, the platform matrix, the perf ratios, the SemVer/MSRV policy,
and the security-response process become the published, gated contract.

---

### v47 — "6/6 BEYOND I: verified safety + totality" — moves: MS → 6, TS → 6 (begins)

Now that parity + 1.0 are shipped, the differentiators begin. Safety first: the
Miri-style UB gate and the totality effect.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| **BEYOND: Miri-style UB interpreter that gates `unsafe` in CI** | MS | beyond/XL | `kardc --interp-ub`: ≥40 UB cases detected (100% TP), ≥40 sound cases zero FP; runs on full unsafe+FFI corpus as merge-blocking job; catches ≥3 seeded UB mutations |
| **BEYOND: continuous adversarial memory-safety fuzzing** | MS | beyond/L | nightly cron ≥10000 programs/run across ≥6 categories through triple oracle (ASan/LSan/UBSan + UB-interp + drop-count); any divergence fails + auto-files minimized repro; rolling-30-day zero unexplained safe-subset UB (**`Arc`-cycle leaks excluded via the gate-#11 allowlist, never counted as UB**) |
| **BEYOND: totality/termination as a checked effect (`! { div }`)** | TS | beyond/XL | `smoke_test_totality.sh`: ≥15 total accepted w/o `div`, ≥15 partial required to declare it; soundness oracle: certified-total halts on ≥500 fuzzed inputs within step cap; `--totality-report`; ≥80% of prelude pure fns certify total |

**Dim move:** **memory-safety 6/6** (both safe **and** unsafe code continuously
verified UB-free by tooling wired into the release gate). Type-system begins 6/6.

---

### v48 — "6/6 BEYOND II: effect unification, fusing stdlib, perf contracts, deadlock-freedom" — moves: AE → 6/6, SL → 6, BP → 6/6, CC → 6 (begins)

The cross-dimension differentiators that depend on v47's safety base and v44's
backend base.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| **BEYOND: async = an effect handler (unify runtime + effect system)** | AE | beyond/XL | all existing async smoke tests pass unchanged after rewrite; a user `Retry` + `Trace` effect compose with the executor (no special-casing); state-machine fast path within 1.1× of pre-unification ns/suspend over 10M suspends |
| **BEYOND: deterministic replayable executor (`--sim`)** | AE | beyond/L | `smoke_test_sim.sh`: racy program over 1000 seeds reproducible per-seed (byte-identical trace); buggy ordering replays 100/100; 24h-logical-time timeout completes <1s wall |
| **BEYOND: zero-allocation fusing iterator pipelines (the *additional* 1.1× result)** | SL | beyond/XL | `smoke_test_iter_fuse.sh`: 5-stage pipeline over 10M elems zero post-source heap allocs; within 1.1× of hand-written C loop (BENCHMARKS.md); `Vec<String>` map/filter clones zero strings. *(This is a beyond/synthetic-pipeline result; the 1.0 application-perf parity gate was already met at v44 §4 #6.)* |
| **BEYOND: derive-based serde + machine-checked schema-evolution** | SL | beyond/XL | `smoke_test_serde.sh`: 50 random typed values round-trip JSON+binary `deser(ser(x))==x`; drop-required-field rejected, add-Option accepted (exit-code gates); JIT==AOT |
| **BEYOND: machine-verified collection contracts (Big-O + leak-free)** | SL | beyond/L | `smoke_test_collection_contracts.sh`: for 6 collections, regression-fit exponent within ±0.15 of claimed class across n∈{1e3..1e6}; 1M-op fuzz ASan+double-free-clean; CONTRACTS.md generated, CI fails on drift |
| **BEYOND: deterministic replayable runtime surface (capability-injected)** | SL | beyond/L | `smoke_test_replay.sh`: program (stdin+net+time+random) records a trace then replays byte-identical offline; JIT==AOT. *(Determinism/replay — complements, does not replace, the v42 observability facade.)* |
| **BEYOND: always-on dual-bound perf-regression gate (self vs live C)** | BP | beyond/L | dual-bound report each CI run; gate fires only on statistically-significant + over-threshold regression (FP rate <5% over 20 unchanged runs); ratio-to-C recomputed live; flamegraph auto-uploaded on regression |
| **BEYOND: per-function codegen-quality contracts (`#[codegen(no_alloc, vectorized, …)]`)** | BP | beyond/XL | annotated fn violating its contract FAILS compilation (no_alloc w/ malloc, vectorized w/o vector IR); negative test proves each bites; runs in normal `kardc`, CI-gated |
| **BEYOND: portable C fallback proven equal to LLVM on every platform** | BP | beyond/XL | randomized oracle (structs/enums/Vec/String/closures/generics/Drop) ≥10000 programs/platform on Linux/macOS/Windows + one non-LLVM-via-C arch, `--emit-c`==LLVM (exit+stdout) zero divergence |
| **BEYOND: static deadlock-freedom (lock-order + channel-cycle analysis)** | CC | beyond/XL | ≥30 deadlock-prone programs all rejected with cycle diagnostic; ≥30 correct concurrent programs (incl. whole v31/v32 suite) all compile; zero false-positives = hard gate |

**Dim move:** **async-effects 6/6** (async-as-effect-handler + deterministic
replay, the four-property unification no production language ships); **stdlib 6/6**
(fusing iterators + checked serde + verified contracts + replayable surface, on top
of the v42 observability facade); **backends-perf 6/6** (dual-bound gate + codegen
contracts + universal C reach); concurrency begins 6/6.

---

### v49 — "6/6 BEYOND III: concurrency verification, proc-macros, refinement types" — moves: CC → 6/6, MP → 6/6, TS, EC

The model-checking/verification differentiators and the unifying metaprogramming
primitives.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| **BEYOND: deterministic record-replay + exhaustive interleaving model-checker** | CC | beyond/XL | racy program replays its emitted seed 1000/1000; `--concurrency-check` proves a correct lock-free stack has no violation and FLAGS a broken relaxed-ordering variant (green/red CI) |
| **BEYOND: machine-checked memory model + verified scheduler/atomics core** | CC | beyond/XL | proof checker (TLA+/Coq/CBMC) green every push; checks Chase-Lev lock-free progress, no-lost-task/no-double-run injector, Ordering↔fence correspondence; weakening an ordering breaks the gate |
| **BEYOND: typed AST-reflection API (the unifying primitive)** | MP | beyond/XL | `smoke_test_reflection.sh`: const fn iterates fields via TypeInfo; reflected field count/names/types == declaration for struct/tuple-struct/enum (≥10 cases); identical LLVM/`--emit-c`; trait-impl query correct at compile time |
| **BEYOND: procedural macros as in-language `meta fn`s (quote/unquote)** | MP | beyond/XL | `smoke_test_procmacro.sh`: derive `to_json` via reflection, attribute timing-instrument, function-like AST builder; ≥12 end-to-end; generated impls typecheck+borrow-check+LLVM==C; hygiene suite re-run; ill-typed AST → span diagnostic not crash |
| **BEYOND: differential + soundness gate for ALL expansions (`--meta-audit`)** | MP | beyond/L | `meta-audit` CI job over ≥60 programs (incl. ≥500 fuzzed/run): every program LLVM==C exit, zero hygiene violations, zero macro-introduced borrow/effect errors; any divergence fails |
| **BEYOND: refinement / dependent-lite types via bundled SMT** | TS | beyond/XL | `tests/conformance/refinements/` ≥80 programs accept/reject vs oracle (100% frozen subset); rejected shows SMT counterexample; check ≤1.5× type-check wall time; elides ≥90% bounds checks (IR count); SMT vendored + deterministic |

**Dim move:** **concurrency 6/6** (static deadlock-freedom + record-replay +
model-checker + verified scheduler/memory model); **metaprogramming 6/6** (one typed
AST-reflection substrate unifying declarative + comptime + proc-macros, every
expansion dual-backend-identical + soundness-gated). Type-system nears 6/6.

---

### v50 / 1.x — "6/6 BEYOND IV: the formal capstone" — moves: TS → 6/6, EC → 6/6, TL → 6/6

The mechanized-soundness and provable-tooling capstones that complete every
dimension and the maturity arc.

| Phase | Dim | Tag | Acceptance gate |
|---|---|---|---|
| **BEYOND: statically-verified exhaustive effect handling** | TS | beyond/L | `smoke_test_effect_exhaustive.sh`: ≥12 fully-handled programs JIT==AOT; ≥10 unhandled/partial rejected pinpointing the operation + perform site; ≥200-nesting fuzzer zero false accepts (runtime assertion confirms no effect reaches main) |
| **BEYOND: mechanized soundness proof of core type relations in CI** | TS | beyond/XL | `proofs/` builds with zero `admit`/`sorry`; differential fuzzer ≥10000 programs/run through formal model + real typechecker, 100% verdict agreement; `SOUNDNESS.md` maps each lemma to its code site |
| **BEYOND: mechanized soundness core (progress/preservation, drop-soundness)** | MS | beyond/XL | proof artifact builds in CI (zero `admit`/`sorry`/extra axioms); preservation+progress+drop-soundness stated; ≥200 core-calculus programs through formal interpreter + `--interp-ub`, 100% UB/no-UB agreement |
| **BEYOND: sub-100ms self-verifying incremental compilation** | TL | beyond/XL | `smoke_test_incremental.sh`: touch 1 of 50 fns → rebuild <100ms, <10% queries re-run; `--verify-incremental` over ≥50 random edit sequences: every incremental object-hash == from-clean, else CI fails |
| **BEYOND: unified IDE server (one query engine: LSP+DAP+format+lint), conformance-gated** | TL | beyond/XL | `smoke_test_ide_conformance.sh`: LSP hover type-string == batch compiler type over ≥100 bindings (zero mismatch); LSP diagnostics == batch (codes/ranges); DAP breakpoint+variable read; LSP path hits persisted memo cache |
| **BEYOND: in-tree deterministic record-replay time-travel debugging** | TL | beyond/XL | `smoke_test_record_replay.sh`: replay twice byte-identical (hash); DAP reverse-step lands on prior source line; "last write to x" returns correct line; multithreaded fixture replays deterministically over ≥20 runs |
| **BEYOND: mechanized spec — every clause linked to an executable test** | EC | beyond/XL | `smoke_test_mechanized_spec.sh` fails if any clause has zero linked tests, any test references a missing clause, or spec EBNF ≠ kardc grammar fingerprint; metric 100% clauses test-linked; rendered spec shows per-clause pass/fail badge |
| **BEYOND: real self-hosting bootstrap, differentially verified (on the frozen target subset)** | EC | beyond/XL | bootstrap CI: ≥5000 fuzzed + real-feature programs **drawn from the frozen `kard-boot` target subset (§3 arc A)**, kardc-in-kardashev == C++ host (exit/output); stage2 + stage3 produced, stage2→stage3 fixpoint self-compiles to functionally-equal compiler on the full **frozen-subset** corpus |
| **BEYOND: reproducible builds + mandatory SLSA-level provenance** | EC | beyond/XL | `smoke_test_reproducible.sh`: same source twice (different paths/timestamps) → byte-identical kardc; registry rebuilds each upload, rejects non-reproducing/tampered-signature; metric: 100% packages verified-reproducible (mandatory) |
| **BEYOND: machine-checked soundness of core invariants, re-checked in CI** | EC | beyond/XL | proof/model checker non-interactive, fails on any unproven obligation (zero `admit`/`sorry`/`Axiom`, grep-gated); each theorem carries a spec clause ID + impl reference (doclint-verified); ≥4 invariants checked (progress/preservation, HM, NLL, effect-row subtyping) |

**Dim move:** **type-system 6/6** (totality + exhaustive effect handling +
refinement types + mechanized soundness — stronger static guarantees than any
single production language, each gated); **tooling 6/6** (self-verifying
incremental + unified query-backed IDE + in-tree time-travel debug);
**maturity-ecosystem 6/6** (mechanized test-linked spec + differentially-verified
reproducible bootstrap on the frozen subset + mandatory reproducible provenance +
machine-checked soundness). **All nine dimensions at 6/6.**

---

## 3. The four XL Mega-arcs (entry/exit criteria)

Each mega-arc is too large for one version; it runs as a dedicated track woven into
the versions whose dependencies it satisfies.

### Mega-arc A — Real self-hosting bootstrap

- **Frozen target subset (`kard-boot`).** The bootstrap fixpoint is defined over a
  **frozen language subset, not the moving full language.** `kard-boot` is the set
  of features the self-hosted compiler must *implement and be written in*; it is
  **frozen at v45** to exactly the parity features that have shipped and stabilized
  by then: HM + generics + traits + GATs/object-safety/variance/NLL (v38),
  Send/Sync + the work-stealing executor + structured concurrency (v40),
  Drop/RAII + complete borrow-check + the unsafe surface (v41), mature FFI +
  no_std (v39), the v32 algebraic-effect handlers, declarative macros + comptime
  (v43), and the v42 stdlib core. **It explicitly EXCLUDES every still-evolving
  6/6/beyond semantics** — totality (v47), async-as-effect unification (v48),
  multi-shot/typed-row beyond-effects (v43 beyond), refinement types (v49),
  proc-macros/reflection (v49), and the model-checked concurrency semantics (v49).
  The self-hosted compiler may *target* the full language, but the **fixpoint
  corpus and the compiler's own source are restricted to `kard-boot`**, so the
  fixpoint is over a stable language even as the host language keeps gaining 6/6
  semantics. (This resolves the "cannot be a fixpoint over a language still gaining
  semantics" issue: the fixpoint language is frozen at v45; later beyond-semantics
  are out of the bootstrap corpus.)
- **Entry (v45):** the normative spec exists (target grammar frozen enough to
  parse against), the conformance suite gives a differential corpus, and
  **`kard-boot` is frozen**. Stage the kardashev-in-kardashev compiler:
  lexer/parser → HM+generics+traits+effects(handlers) → borrow check → full codegen
  (structs/enums/match/Drop/closures/async over the `kard-boot` subset).
- **Milestones (v45→v50):** each milestone differentially gated — kardc-in-kardashev
  and the C++ host produce identical results on a growing `kard-boot` corpus.
- **Exit (v50/1.x):** stage2 (host compiles the kardashev compiler) and stage3
  (stage2 compiles it) are produced; stage2 output == stage3 output on the full
  ≥5000-program **`kard-boot`** corpus (the diverse-double-compilation fixpoint over
  the frozen subset). Because `kard-boot` was frozen at v45 and excludes all
  post-v45 beyond-semantics, the fixpoint is **stable by construction** — no
  semantics the corpus exercises changes after the freeze.

### Mega-arc B — Package registry & ecosystem (incl. toolchain distribution)

- **Entry (v45):** manifest + SemVer resolver + lockfile + **MSRV/toolchain-range**
  (`smoke_test_resolver.sh` green), the **toolchain manager `kardup`**
  (`smoke_test_kardup.sh` green), and the local-fixture registry
  (`smoke_test_registry.sh` green).
- **Milestones:** `kard publish`/`add`/`update`/`vendor`/`audit`/`license`;
  `kardup install`/`default`/`override`; sparse-index format; yank/immutability;
  the security-advisory schema + `SECURITY.md` response process (v46).
- **Exit (v50/1.x):** registry re-builds each upload from source and accepts only
  on hash match; mandatory SLSA-level provenance; **100% of packages
  verified-reproducible** (`smoke_test_reproducible.sh` green); the toolchain itself
  is distributed reproducibly + checksum-verified by `kardup`. N≥(production
  threshold, §4) real packages — **including at least one that links a real C
  library through the v39 FFI/bindgen path (e.g. an OpenSSL- or zlib-class binding)**
  — resolvable + buildable offline.

### Mega-arc C — More backends & platforms (WASM + Windows + freestanding)

- **Entry (v44):** cross-compilation infra (host/target triples, datalayout
  decoupled from host) **plus the per-target std/sysroot build matrix and policy**
  lands.
- **Milestones:** WASM target under a committed CI runtime (wasmtime/node),
  Windows/PE target (clang-cl/MinGW), the **freestanding `no_std` target**, each
  differentially gated like `--emit-c`; a **per-target std build** (prebuilt for the
  hosted-platform targets, build-from-source for freestanding) checksummed in CI.
- **Exit (v44, extended in v48):** platform matrix Linux+macOS+Windows+WASM
  **+freestanding** all in the differential corpus with identical exit+stdout (and,
  for freestanding, zero-libc); each non-freestanding target has a CI-built,
  checksummed std; the portable C fallback is proven equal to LLVM on every platform
  incl. one non-LLVM-via-C arch (≥10000 programs/platform zero divergence), and the
  C-backend has a stated `-g` line-level debug story (v46).

### Mega-arc D — Spec, stability & governance → 1.0

- **Entry (v45):** normative spec (EBNF + operational semantics + the ABI/panic/
  overflow/FFI clauses, clause IDs, coverage ≥95%) authored.
- **Milestones (v46):** conformance suite (100% pass, per-clause coverage
  committed/release), stability policy (editions + SemVer enforcement + **MSRV
  policy** + 1.0 checklist), RFC/governance + audit/license + **security-response
  policy + compiler-hardening fuzzing**.
- **Exit (v1.0, then v50/1.x):** §4 gate closed → **1.0 declared**; then the spec
  becomes **mechanized** (100% of clauses test-linked, per-clause pass/fail badge)
  and core soundness invariants are machine-checked in CI (≥4 invariants, zero
  `admit`/`sorry`).

---

## 4. PRODUCTION-READINESS GATE — "you can ship products on this"

**1.0 is declared only when ALL of the following are green CI gates** (not prose,
not aspiration). Each maps to the phase(s) that deliver it. Every criterion below is
objective and verifiable by a named CI job/test.

| # | Gate | Objective criterion | Delivered by |
|---|---|---|---|
| 1 | **Normative spec** | EBNF + operational semantics + ABI/panic/overflow/FFI clauses, every clause a stable ID, coverage ≥95%, grammar round-trips against the parser | v45 (EC spec) |
| 2 | **Conformance suite + pass-rate** | `tests/conformance` 100% on LLVM (JIT==AOT), per-clause coverage committed per release, CI fails if pass-rate <100% or coverage regresses | v46 (EC conformance) |
| 3 | **Stability + MSRV policy** | editions model + `kard semver-check` (too-small bump for a breaking change = non-zero; **MSRV raise classified as breaking**) + numbered 1.0 checklist, each line linked to a green gate | v46 (EC stability) |
| 4 | **Package ecosystem** | manifest/resolver/lockfile (**with MSRV/toolchain ranges**) + hosted registry + `kard publish`/`vendor`/`audit`/`license`; offline build byte-identical to online; N production packages (incl. ≥1 real C-library binding) resolvable+buildable offline | v45 (EC registry) + v39 (FFI) + arc B |
| 5 | **Platform matrix (incl. per-target std)** | Linux + macOS + **Windows** + **WASM** + **freestanding `no_std`** all in the differential corpus with identical exit+stdout (freestanding: zero-libc); cross-compile verified under qemu; **each non-freestanding target has a CI-built, checksummed std; the prebuilt-vs-from-source policy is committed** (`docs/targets.md`) | v44 (BP WASM+Windows+freestanding+cross+std-matrix) + arc C |
| 6 | **Perf within 1.1× C (application-level)** | **6–10-workload application suite geomean ratio-to-clang-`-O2` ≤1.1×** (committed `bench/app_baseline.json`) **AND** tight-loop ratio ≤1.3×; the bench-regression gate is live each CI run. *(The 1.0 gate is met by the application-suite 1.1× at v44. The synthetic fusing-pipeline 1.1× in v48 is an additional beyond result, not the 1.0 prerequisite.)* | **v44 (app-suite ≤1.1× + loop ≤1.3×)** + v37/v46 (regression gate) |
| 7 | **Multi-threaded executor (race-free, verified)** | N-worker work-stealing scheduler, ≥2.5× speedup on divide-and-conquer over single-thread, leak-free + deterministic over 200 runs, **and TSan-clean under the required `tsan` CI job**, ubuntu+macos | v37 (`tsan` job) + v40 (CC/AE Phase 174) |
| 8 | **Debugger (both backends)** | **LLVM backend:** DWARF -g per-line, gdb/lldb pretty-printers for String/Vec/Option/Result/enums, breakpoint+print+backtrace verified. **C backend:** `#line`-mapped source so `gdb` breaks on and backtraces the kardashev source line. Each backend's debug story is its own green test. | v46 (TL debugger LLVM + C-backend `-g`) |
| 9 | **Incremental compile** | sub-100ms single-fn rebuild, `--verify-incremental` proves incremental output == from-clean over ≥50 random edit sequences | v50 (TL incremental) |
| 10 | **Security / audit / supply-chain / response** | `kard audit` (advisory DB) + `kard license` (SPDX allowlist) non-zero on violations; reproducible builds + signed provenance the registry re-verifies; **`SECURITY.md` CVE-disclosure + coordinated-response process; compiler-hardening fuzzer (≥100000 malformed inputs/run, zero compiler crashes) as a CI gate** | v46 (EC audit + security-response + compiler-fuzz) + v50 (EC reproducible) |
| 11 | **Memory-safety floor (incl. the GC-free leak policy)** | every smoke test + memsafety fuzzer ASan+LSan+UBSan clean on both backends as a merge-blocking job. **Leak policy (RAII/no-GC):** the LSan-allowlist contains *only* the documented `Arc`/`Rc`-reference-cycle fixtures (the one safe-subset leak class, identical to Rust), each with a `// SAFETY-LEAK: Arc cycle, use Weak` justification + a doclint check that every allowlist entry is exactly such a cycle; **all non-cycle leaks == 0**. (This reconciles "continuously verified UB-free" with the unavoidable safe-`Arc`-cycle: cycles are a *documented, justified leak*, never UB, and never counted by the fuzzer.) | v37 (sanitizer gate) + v41 (leak closure + allowlist reduction) |
| 12 | **Borrow-check soundness** | borrowck conformance corpus ≥250 cases (incl. lifetime-parametric + NLL classics) 100% classify | v41 (region inference) |
| 13 | **FFI maturity (C interop)** | `repr(C)` struct-by-value across `extern "C"`, C callbacks (fn-ptr ABI), pointer arithmetic in FFI, and a `kard bindgen` header importer — all ASan+UBSan-clean, ABI-pinned, calling real libc; an FFI-unwind boundary without an abort shim is a compile error | v39 (FFI I+II) + v37 (panic/FFI-unwind contract) |
| 14 | **no_std / freestanding / pluggable allocator** | `#![no_std]` builds against a user `GlobalAlloc`, zero libc-malloc symbols, core works without std, std-only API under no_std is a compile error; builds + runs as a CI matrix leg incl. the freestanding cross-target | v39 (no_std + GlobalAlloc) + v44 (freestanding target leg) |
| 15 | **Panic + overflow policy** | committed default `panic=unwind` (with `panic=abort` flag), `catch_unwind` runs all `Drop`s once, FFI-unwind boundary contract enforced; release integer-overflow default committed + selectable (`-Coverflow=trap/wrap`); both clauses in the spec | v37 (panic + overflow policy) + v45 (spec clauses) |
| 16 | **Observability** | structured logging + tracing spans + metrics facade, pluggable `Subscriber`, level filtering, zero-cost when disabled, works under no_std | v42 (observability facade) |
| 17 | **Toolchain distribution + version manager** | `kardup install/default/override` selects + pins versioned `kardc`; per-directory toolchain from the manifest; checksum-verified against signed registry manifests; offline bundle reproduces byte-identical toolchains | v45 (kardup) + arc B |

> **1.0 is the moment all seventeen are simultaneously green on both CI platforms**
> (plus the `tsan` and freestanding-target jobs). The 6/6 beyond-parity capabilities
> (§5) land **after** 1.0 (v47→v50) and are not required to ship products — they are
> what makes kardashev *exceed* every single production language on every dimension.
>
> **On gate #6 specifically:** the headline "within 1.1× of C" is satisfied **at the
> instant 1.0 is declared** by the v44 **application-benchmark suite geomean ≤1.1×**,
> not by the v48 fusing-pipeline. The 1.0 perf contract is therefore provably met
> when 1.0 ships; the v48 result is an additional, synthetic-pipeline differentiator
> on top.

---

## 5. 6/6 BEYOND-PARITY — the one differentiator per dimension

For each of the **nine dimensions**: the single capability that puts it **above any
single production language**, and the gate that measures it.

| Dim | The differentiator (above any one production language) | How it's measured (the 6/6 evidence) | Version |
|---|---|---|---|
| **type-system** | **Totality + checked-exhaustive effects + refinement types + a mechanized soundness proof** — Rust-class ownership *and* a checked verification surface (a fn provably total carries a machine-checkable purity+termination guarantee; every `perform` provably discharged; `i64 where x>0` discharged by a bundled SMT check; HM/NLL/effect-row subtyping proven in a proof assistant). | `--totality-report` + ≥500-input halting oracle; effect-exhaustive fuzzer (≥200 nestings, zero false accepts); `tests/conformance/refinements/` ≥80 vs oracle + ≥90% bounds-check elision; `proofs/` zero `admit`/`sorry` + ≥10000-program model-vs-typechecker agreement. | v47, v49, v50 |
| **memory-safety** | **Both safe AND unsafe code continuously verified UB-free by tooling in the release gate, backed by a formal core, with a stated no-GC RAII memory model** — a Miri-style provenance/Stacked-Borrows UB interpreter that *fails the build* on UB (Rust ships Miri but doesn't gate releases on it), plus a mechanized progress/preservation/drop-soundness proof; the one permitted safe-subset leak (Arc cycle) is documented and gate-allowlisted, never UB. | `--interp-ub`: ≥40 UB (100% TP) / ≥40 sound (0 FP), merge-blocking on full unsafe+FFI corpus, catches seeded mutations; nightly ≥10000-program triple-oracle fuzzer (Arc-cycles excluded via allowlist); proof artifact zero `admit`/`sorry` + ≥200-program formal-interp vs `--interp-ub` 100% agreement. | v47, v50 |
| **concurrency** | **Compiler-verified static deadlock-freedom + deterministic record-replay + an exhaustive interleaving model-checker + a machine-checked memory model/scheduler, all on a TSan-gated executor** — beyond Rust (data-race-only), Go/Java/Rust (replay/model-check need external tools), and every production scheduler (none mechanically verified). | ≥30 deadlock-prone programs rejected / ≥30 correct compile (zero FP hard gate); replay 1000/1000 per-seed; `--concurrency-check` proves a correct lock-free stack + flags a broken one; proof checker green (Chase-Lev progress, no-lost/no-double-run, Ordering↔fence); the whole executor TSan-clean. | v48, v49 |
| **async-effects** | **The first language to UNIFY a production async runtime with full algebraic effects in one zero-cost typed model** — async/await *is* an effect handler (one control-flow mechanism, user handlers compose with the scheduler), effect rows typed+checked end-to-end (> OCaml 5's untyped effects), multi-shot resumptions memory-safe under the borrow checker, and a deterministically-replayable executor (a guarantee tokio can't give). | all async smoke tests pass after the async=effect rewrite + `Retry`/`Trace` compose, fast path ≤1.1× ns/suspend; ≥60 effect-typing programs (no runtime Unhandled possible); N-queens N=10==724 multi-shot zero-leak; `--sim` 1000-seed reproducibility. | v43, v48 |
| **metaprogramming** | **One typed AST-reflection substrate unifying hygienic declarative macros + Zig-grade comptime + D-grade reflection + in-language proc-macros (no separate crate/ABI/build.rs), with every expansion dual-backend-identical and soundness-gated** — span-accurate to the call site, zero macro-induced UB. | reflection field count/names/types == declaration (≥10 cases); proc-macro derive/attr/fn-like ≥12 end-to-end (typecheck+borrow-check+LLVM==C, hygiene re-run); `--meta-audit` ≥60 programs + ≥500 fuzzed/run, zero hygiene/soundness violations. | v49 |
| **stdlib** | **Every stdlib promise is an executable CI gate** — a provably zero-allocation fusing iterator tower, derive-based serde with machine-checked schema-evolution, collections whose Big-O + leak-freedom are machine-verified, a structured observability facade, and a deterministically-replayable I/O/time/random surface (capabilities Rust/Go bolt on via third-party fakes). | 5-stage pipeline over 10M elems zero post-source allocs + ≤1.1× a C loop; 50 random values round-trip + schema-incompat rejected; 6-collection exponent-fit within ±0.15 + 1M-op leak-clean; observability subscriber captures spans+metrics, zero-cost disabled; record==replay byte-identical offline. | v42 (observability), v48 |
| **tooling** | **Provably-correct tooling, not just feature count** — sub-100ms incremental rebuilds whose result is CI-verified byte-identical to a clean rebuild (rustc can't prove this), in-tree deterministic record-replay time-travel debugging (rr/Pernosco are external), one query-engine backing LSP+DAP+format+lint+compiler with a conformance gate proving IDE answers == compiler ground truth, and a compiler hardened by crash-fuzzing (no IDE/CLI crash on adversarial input). | `--verify-incremental` ≥50 edit sequences object-hash==from-clean; reverse-step lands on prior line + multithreaded replay deterministic over ≥20 runs; LSP hover/diagnostics == batch compiler over ≥100 bindings (zero mismatch); `fuzz_compiler` ≥100000 inputs/run zero crashes. | v46 (compiler-fuzz), v50 |
| **backends-perf** | **Performance as a regression-gated, machine-checked contract with universal reach** — an always-on dual-bound perf gate (self-baseline AND live clang -O2, recomputed each run, no third-party tool), per-function `#[codegen(no_alloc, vectorized, …)]` contracts enforced in CI, a portable C fallback proven semantically equal to LLVM on every platform incl. non-LLVM arches (with a stated `-g` story), and a freestanding/no_std target in the same matrix. | dual-bound report each run, FP rate <5% over 20 unchanged runs, flamegraph on regression; annotated-fn contract violation FAILS compilation (negative test bites); ≥10000 programs/platform `--emit-c`==LLVM zero divergence on Linux/macOS/Windows + a non-LLVM arch; application-suite geomean ≤1.1× (v44). | v44 (app ≤1.1× + freestanding), v48 |
| **maturity-ecosystem** | **A verified, reproducible, machine-checked ecosystem end-to-end** — a normative spec where every clause is test-linked with a live pass/fail badge (Rust's reference is prose-only), a registry that re-verifies reproducible-build + SLSA provenance before accepting (crates.io has neither mandatory), a reproducibly-distributed toolchain with MSRV-aware resolution + a `rustup`-class version manager, a differentially+reproducibly-verified self-hosting bootstrap over a frozen subset (the diverse-double-compilation north star), and machine-checked core soundness re-run in CI. | `smoke_test_mechanized_spec.sh` 100% clauses test-linked; `smoke_test_reproducible.sh` byte-identical + registry rejects non-reproducing; `kardup` reproduces byte-identical toolchains + MSRV resolver gate; stage2==stage3 fixpoint on ≥5000 frozen-subset programs; proof checker zero unproven obligations, ≥4 invariants green. | v45 (toolchain/MSRV), v50 |

---

## 6. At a glance — version → dimension trajectory

| Version | Theme | Dimensions it advances (target after) |
|---|---|---|
| v37 | Foundations & unblockers (incl. TSan job, panic/overflow policy) | TS·MS·CC·BP·TL·MP (parity groundwork) |
| v38 | Type system, completed I (lifetime spine) | **TS → region 5** |
| v39 | FFI maturity, no_std & async parity I | EC/SL (FFI+no_std unblockers), CC·AE·TS (HRTB) |
| v40 | Parallel executor & structured concurrency | **CC → 5/5, AE → 5/5** (AE parity floor complete here) |
| v41 | Memory safety parity + unsafe surface | **MS → 5/5** |
| v42 | Stdlib depth I (collections/IO/time/net/observability) | SL → 5 (in progress) |
| v43 | Metaprogramming parity + regex + typed/multishot effects | **MP → 5/5**, SL, **AE → 6 (begins)** |
| v44 | Backends & platforms (perf/cross/WASM/Windows/freestanding) | **BP → 5/5 + app-perf ≤1.1×** + arc C |
| v45 | Ecosystem foundation (registry/kardup/MSRV/spec/bootstrap) | EC → 4 + arcs A·B·D open, TL query engine |
| v46 | Tooling depth + conformance + stability + security-response | **TL → 5/5, EC → 5** |
| **🎯 v1.0** | **Production-ready** | **§4 (17-gate) closes — shippable** |
| v47 | Beyond I: verified safety + totality | **MS → 6/6**, TS → 6 begins |
| v48 | Beyond II: effect unification, fusing stdlib, perf contracts, deadlock-free | **AE → 6/6, SL → 6/6, BP → 6/6**, CC → 6 begins |
| v49 | Beyond III: concurrency verification, proc-macros, refinement types | **CC → 6/6, MP → 6/6**, TS nears 6 |
| v50/1.x | Beyond IV: the formal capstone | **TS → 6/6, TL → 6/6, EC → 6/6** — all nine at 6/6 |

---

*Sequencing principle restated: unblock (lifetimes, FFI+no_std, registry+toolchain
manager, spec, query engine, perf harness, sanitizer+TSan gates, panic/overflow
policy) → reach Rust-class parity on every axis, including application-perf ≤1.1× C →
close the seventeen-gate production-readiness checklist and ship 1.0 → then layer the
nine 6/6 differentiators, each a green CI gate, until kardashev provably exceeds
every single production language on every dimension.*
