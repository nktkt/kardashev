# Roadmap

Where kardashev honestly stands, and the work that would move it forward.

kardashev is a **research / portfolio-grade** systems language: a real,
well-tested, ~32 K-LOC LLVM-backed compiler with a genuinely uncommon feature —
zero-runtime-cost, row-polymorphic **effect labels** in the type system — plus
ownership + NLL borrow checking, deterministic `Drop`/RAII, generics/traits, a
full numeric tower, async, and threads. It is well above the median hobby/student
compiler in breadth and test discipline. It is **not** a production language: it
is pre-ecosystem, pre-performance-proven, and MVP-shaped in places.

**Shipped: v1–v23** (Phases 0–129, through `v0.23.0`). The per-version themes are
in the [README roadmap table](README.md#roadmap); every phase's detail is in
[CHANGELOG.md](CHANGELOG.md). v15–v19 built a self-hosted *mini* compiler and a
differential-fuzzing test surface; v20 took it to real LLVM IR; v21 added a
benchmark suite and closed the measured leaks; v22 landed `||` and `&<temporary>`
and reconciled the docs; v23 added a second backend (`kardc --emit-c`).

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
- **Performance: measured on micro-benchmarks, unproven at scale.** *(v21 added
  `bench/` + `BENCHMARKS.md`: kardashev is C-competitive on `fib`/`collatz`
  (≈1.0×) and ≈2.2× C on a tight integer loop.)* What remains: the ~2.2× gap is
  an open codegen-opt target, and there are no application-scale benchmarks,
  no LTO/PGO, and no regression tracking.
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
- **Language completeness is MVP in places.** *(v22 closed the two most-cited
  ergonomic gaps — `||` logical-or and `&<temporary>` ref-to-rvalue — and
  reconciled the stale docs around `%`/`&&`/enum-typed struct fields.)* What
  remains is breadth: no default trait methods / supertraits / blanket impls /
  coherence / associated consts; no match guards / or-patterns / slice patterns;
  no `char` type or UTF-8 codec; no `Fn`/`FnMut`/`FnOnce` hierarchy; no macros /
  operator overloading; no raw pointers / `unsafe` / `no_std`. The full list and
  its sequencing are in *The road to production* below.
- **No ecosystem / thin platform story.** *(v23 added a second backend — a C
  subset generator — so LLVM is no longer the only target.)* Local-path
  dependencies only (a third-party registry is deferred); the C backend is still
  a subset; no WASM/Windows; CI covers Linux + macOS, where one arm64
  JIT-teardown flake is retried rather than root-caused; no normative spec;
  pre-1.0 with no stability policy.

## The road to production (v24 →)

> Planned, not done. Each roadmap follows the established cadence: implement each
> phase green (JIT **and** AOT, differentially gated where a backend is
> involved) → adversarial review → fix findings → consolidating PR → tag/release.
> Phase numbers continue from 129. The list below is the **complete** set of
> work that separates kardashev from a production systems language, derived from
> a grounded seven-dimension survey of the codebase (language/type-system,
> memory/effects/concurrency, stdlib/runtime, backends/performance, self-hosting,
> tooling/ecosystem, correctness/spec/stability) plus a completeness critic. The
> exhaustive item-by-item list (174 items, each with codebase evidence + a size)
> is in **[docs/roadmap-enumeration.md](docs/roadmap-enumeration.md)**; the
> sections below sequence those items into roadmaps.

**Sequencing rationale.** Order is by *leverage × dependency*, not by area:
(1) developer-experience foundations first (diagnostics pay off on day one);
(2) the language-completeness items that unblock everything downstream (traits,
patterns, the standard-trait vocabulary, the string/char story — these lock in
early); (3) finish the second backend while the momentum and the differential
oracle are fresh (and it is fully testable in this environment); (4) mature the
two differentiators — concurrency and the effect system; (5) the systems-grade
escape hatches (FFI / `unsafe` / `no_std`); (6) metaprogramming; (7) stdlib
depth and tooling; (8) the long XL arcs run as dedicated tracks (see *Mega-arcs*).

### v24 — diagnostics & the developer surface — *done (v0.24.0)*
The highest-ROI gap: error quality. *(Survey: critic "error message quality" =
top priority; `tooling-ecosystem` LSP items.)* All five phases shipped (130
snippet+caret with user-relative lines, 131 parser panic-mode recovery, 132 the
`-W` lint, 133 error codes + `--explain`, 134 `///` doc comments).
- **130** rich diagnostics — source snippets, multi-line spans with carets,
  `expected … / found …`, secondary notes + hints (today errors are one-line
  `line:col: message`).
- **131** parser + typecheck **error recovery** — collect and report many
  independent errors per run instead of cascading off the first.
- **132** a **lint / warning** pass (`-W`) — unused `let`/param/import,
  unreachable code, shadowing, non-snake-case; warnings are non-fatal.
- **133** stable **error codes** + `kardc --explain Exxxx` + a diagnostics index.
- **134** `///` **doc comments** captured in the AST and surfaced in LSP hover —
  groundwork for doc-generation (v36).

### v25 — the trait system, finished — *done (v0.25.0)*
*(Survey `lang-typesystem`: default methods, supertraits, blanket impls,
coherence, associated consts — all "not implemented / monomorphic-only MVP".)*
- **135** **default trait methods** (with impl override).
- **136** **supertraits** / trait inheritance (`trait Ord: Eq`).
- **137** **blanket + where-bounded impls** (`impl<T: Bound> Tr for T`).
- **138** **coherence + overlap** checking (orphan rule, overlap rejection).
- **139** **associated consts** on traits/impls.
- **140** unify the **standard-trait vocabulary** — `From`/`Into`/`Deref`/
  `Index`/`Iterator`-as-a-trait, consistently derivable and overridable.

### v26 — patterns, types & borrow-check completeness — *done (v0.26.0)*
*(Survey: pattern gaps, type aliases, generic enum payloads, `Fn`/`FnMut`/
`FnOnce`, module visibility, `&mut` reborrow / two-phase / NLL completeness.)*
*Shipped: 141 guards + or-patterns; 142 struct/tuple patterns; 143 slice
patterns + `&mut [T]`; 144 type aliases; 145 the `Fn`/`FnMut`/`FnOnce` hierarchy
+ capture classification; 146 two-phase borrows + `pub(crate)`/`pub(super)`/
`pub(self)` + `use`/`pub use`. Documented follow-ons: full NLL region inference,
implicit `&T`-field reborrows, cross-crate visibility, owned by-move closure
captures (a true runtime `FnOnce` needs a closure-env drop vtable).*
- **141** match **guards** (`x if c =>`) + **or-patterns** (`A | B =>`).
- **142** **struct / tuple patterns** in `match` and fn params; nested
  destructuring (today tuple-in-match is unsupported).
- **143** **slice patterns** (`[x, rest @ ..]`) + **`&mut [T]`** mutable slices.
- **144** **type aliases** (`type X = …`) + **generic enum payloads**.
- **145** the **`Fn`/`FnMut`/`FnOnce`** closure-trait hierarchy + precise
  move/ref capture classification.
- **146** **borrow-check completeness** — `&mut` reborrow through calls/recursion,
  two-phase borrows, implicit reborrows — and **module visibility** scoping
  (`pub(crate)`/`pub(super)`) + re-exports (`pub use`).

### v27 — strings, text & formatting (lock-in early) — *done (v0.27.0)*
*Shipped: 147 a real `char` type (Unicode scalar) + UTF-8 char<->string bridges; 148 UTF-8 correctness (char iteration/indexing/validation); 149 `format!`/`print!`/`println!` (parser-desugared, no macro system yet); 150 the `Debug` trait + `{:?}` + `#[derive(Debug)]`; 151 char classification + string encode helpers (str_join/replace/lines). Documented follow-ons: a distinct borrowed `&str` type (folded into 148; `&String` serves the role today), grapheme-cluster segmentation (UAX #29) + full Unicode case folding (need the Unicode character database), `{:width}`/alignment format specs.*
*(Survey + critic: "no `char` type; strings are byte arrays, no UTF-8 codec";
"no `format!`".)*
- **147** a real **`char`** type (Unicode scalar) + a `String` / `&str` split.
- **148** **UTF-8 correctness** — char iteration, byte/char indexing, validation.
- **149** **`format!` / `println!`** format-args (built-in, or the first macro).
- **150** **`Display` / `Debug`** formatting traits wired to format-args.
- **151** string encode/parse helpers + a grapheme-awareness story.

### v28 — const-eval & generics, finished — *done (v0.28.0)*
*Shipped: 152 const-eval for array/tuple/struct/enum aggregates (+ projection); 153 const-generics beyond i64 (bool/char) + expected-type propagation; 154 bidirectional inference (struct-field coercion + a fixed generic-enum-struct-field mutual-resolution bug); 155 GATs (`type Out<T>;` -> `Self::Out<i64>`, concrete-Self case); 156 monomorphization dedup + specialization (concrete beats blanket) + `--mono-report`. Documented follow-ons: char/f64 SCALAR consts, turbofish + bounded-generic-param GAT projection (`C::Out<i64>`), enum const-generic params.*
*(Survey: const-eval is i64/bool only (`typecheck.cpp:4206`); const-generics
i64-only; inference incomplete; no GATs.)*
- **152** **const-eval beyond i64/bool** — struct / enum / array const values.
- **153** **const-generics beyond i64** (bool/char/enum const params) + bounds.
- **154** **bidirectional type inference** completeness (closures, match arms,
  deep context propagation).
- **155** **generic associated types** (GATs).
- **156** **monomorphization control** / specialization + code-bloat mitigation.

### v29 — the C backend, finished I (aggregates + control)
Continue v23 (`--emit-c`), each phase differentially gated vs LLVM.
*(Survey `backends-performance`: the explicit subset list in `emit_c.hpp`.)*
- **157** C backend: **structs** (layout + field access).
- **158** C backend: **enums + `match`** (tagged unions + decision trees).
- **159** C backend: **references / borrows** + `&<temporary>`.
- **160** C backend: **`for` / `loop`-with-value** + **multi-file modules**.
- **161** wire the arith / control / memsafety **fuzzers as a randomized
  C-vs-LLVM oracle** (`emit-c` == LLVM == reference).

### v30 — the C backend, finished II (heap + RAII + generics)
- **162** C backend: **`String`** + heap strings.
- **163** C backend: **`Vec` / `HashMap` / `HashSet`** (the runtime, in C).
- **164** C backend: **`Drop` / RAII** (deterministic destructors + drop flags).
- **165** C backend: **closures + fn-pointers**.
- **166** C backend: **generics / monomorphization** — after which `--emit-c` is
  a near-complete second backend (async excepted; see Mega-arcs).

### v31 — concurrency, hardened (differentiator I)
*(Survey `memory-effects-concurrency`: type-erased `Mutex`, structural `Send`,
no atomics/select/scoped-threads, OS-thread i64-only, no `Rc`/`Arc` weak.)*
- **167** real **`Send`/`Sync`** marker traits (declarable + auto-derived),
  backing/replacing the structural rule with hard enforcement.
- **168** a **type-safe named `Mutex<T>`** (retire the type-erased i64 handle) +
  `RwLock`.
- **169** **atomics** + compare-and-swap + memory orderings.
- **170** channel **`select`** / multiplexed wait + **scoped threads**
  (lifetime-bounded spawning).
- **171** generic **OS-thread return** (`join<T>`, non-i64 results) + `Arc`/weak.

### v32 — async & effects, matured (differentiator II)
*(Survey: no Future combinators, single-threaded executor, Linux-only async I/O,
effect system "tracking only — no handlers/subtyping".)*
- **172** **Future combinators** (`map`/`and_then`/`join`/`select`) + a task API.
- **173** async **cancellation** + timeouts + structured concurrency.
- **174** a **multi-threaded / configurable executor** (work-stealing) + macOS
  async I/O (`kqueue`).
- **175** **effect inference** completeness + **effect subtyping**.
- **176** **user-defined effects + effect handlers** (algebraic effects — the
  research frontier that most extends kardashev's signature feature).

### v33 — systems-grade: FFI, `unsafe` & `no_std`
*(Critic: "incomplete FFI; no raw pointers; no inline asm/SIMD; no `no_std`".)*
- **177** **raw pointers** + **`unsafe`** blocks (the systems escape hatch).
- **178** **FFI maturity** — structs-by-value, callbacks, C-header **binding
  generation** (`bindgen`-equivalent).
- **179** **`no_std`** / freestanding + a pluggable **allocator** trait.
- **180** **inline asm** + **SIMD** intrinsics (portable + target-specific).
- **181** **overflow-checked** arithmetic + a documented integer-overflow policy.

### v34 — metaprogramming
*(Critic: "no macro system; no operator overloading; comptime limited".)*
- **182** a **declarative macro** system (`macro_rules!`-style).
- **183** **user-defined `#[derive]`** on top of macros.
- **184** **operator overloading** (`Add`/`Index`/`Deref`/…) via traits.
- **185** generalized **`comptime` / const-fn** (richer compile-time execution).
- **186** **conditional compilation** / features (`#[cfg(…)]` + `kard.toml`).

### v35 — stdlib depth & runtime
*(Survey `stdlib-runtime`: BTree/VecDeque, iterator adaptors, buffered I/O,
error trait, time/random, serde.)*
- **187** ordered collections (**`BTreeMap`/`BTreeSet`**) + **`VecDeque`**.
- **188** **iterator-adaptor** completeness (lazy, double-ended, zip/chain/
  enumerate/collect).
- **189** **buffered I/O**, stdin streams, file seek, full process/env API.
- **190** an **error-trait** ecosystem (`Error` trait, source chains, `?`-with-
  `From` conversion).
- **191** **time/duration**, random, OS APIs; (de)**serialization** (serde-like).

### v36 — tooling & compiler performance
*(Survey `tooling-ecosystem` + `backends-performance`.)*
- **192** **LSP completeness** — live diagnostics, semantic tokens, code actions,
  inlay hints, cross-file rename, workspace symbols; editor plugins; watch mode.
- **193** **debugger** story — validated gdb/lldb + pretty-printers + backtraces
  with line info; split-DWARF.
- **194** **doc-gen** (`///` → an API-docs site + doctests) + a **test framework**
  (property-based + isolation) + REPL/CI-release polish.
- **195** **incremental compilation** — query caching (unify / trait-resolve /
  per-module) + compile-time-perf work.
- **196** **codegen optimization** — close the committed ~2.2× tight-loop gap
  (register allocation, inlining cost model, bounds-check elision, LICM,
  datalayout) + LTO; expand the benchmark suite with regression detection.

## Mega-arcs (each is XL — a dedicated multi-roadmap track)

These are too large for a single roadmap and run as parallel long-horizon tracks.

- **A real bootstrap (the north star).** Grow the self-hosted compiler from
  today's toy (a 2-type, 8-opcode mini language) to one that compiles kardashev
  *itself*: full lexer/parser for the real grammar → HM inference + generics +
  traits + effects in kardashev → the borrow checker in kardashev → full codegen
  (structs/enums/`match`/`Drop`/closures/async/traits) → the milestone where
  kardc-in-kardashev compiles kardc-in-kardashev. *(Survey `selfhost-bootstrap`:
  13 staged L/XL items.)*
- **The ecosystem.** A third-party **package registry**, dependency versioning +
  lockfiles, `kard publish`, a dependency-tree/audit story. *(Deferred to date
  because Bazel can't run in the dev sandbox — needs a hosted registry.)*
- **More backends & platforms.** A **WASM** backend (with a committed CI runtime
  — wasmtime or node — so it is differentially gated like `--emit-c`); a
  **Windows/PE** target (`clang-cl` / MSVC ABI); cross-compilation infra (host /
  target triples); calling-convention/ABI stability; PGO.
- **Spec, stability & governance → 1.0.** A **normative language spec** (EBNF +
  semantics) + a **conformance suite**; a **stability / SemVer policy** + an
  **editions** model + explicit **1.0 criteria**; an **RFC / governance**
  process; a **UB / soundness audit** + formal verification of core invariants
  (HM unification, NLL soundness, effect-row subtyping); reproducible / signed
  builds + supply-chain provenance; and finally the **root-cause + permanent
  fix** for the macOS-arm64 JIT-teardown flake (currently retried, not cured).

## Shipped — recent detail (v20–v23)

### v20 — toward a real bootstrap (the north star) — *done (v0.20.0)*

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

### v23 — a second backend — *the C backend lands (v0.23.0)*

The v22 "second platform/backend" item is its own roadmap: a full second code
generator is comparable in scope to the LLVM backend it parallels, so it is
grown by subset rather than rushed. v0.23.0 ships the foundation (the i64/bool
subset, differentially gated); the remaining bullets continue in later phases.

- ✅ **Phase 129 (the C-source backend, done)** — `kardc --emit-c` walks the
  same typechecked AST that `codegen.cpp` lowers and emits portable C (compiled
  by the system C compiler), the most *verifiable* second backend in this
  environment. `compiler/src/emit_c.cpp` maps the SUBSET — i64/bool, the full
  operator set (arithmetic / comparison / `&&`/`||` / bitwise / unary), `let`
  (incl. `mut` + assignment), `if`/`else` as a value, `while`, blocks, direct
  calls + recursion (forward prototypes for mutual recursion), and top-level
  `const` — to `int64_t` C, using GNU statement-expressions for the
  expression-oriented constructs. Anything outside the subset is REFUSED with a
  clear error (never miscompiled). **Differentially gated** against the LLVM
  backend by `tests/smoke_test_phase129.sh`: for each program the LLVM AOT exit
  code must equal the emitted-C (`cc -fwrapv`) exit code — 12 programs across
  recursion / loops / logic / bitwise / signed-mod / const all match, and a
  struct program is cleanly refused.
- **Next subset growth** — extend `--emit-c` phase by phase: structs →
  enums + `match` → strings / `Vec` → `Drop`, each differentially green, plus
  wiring the existing arithmetic / control-flow fuzzers (Phases 110–111) as a
  randomized oracle (`emit-c` == LLVM == reference), before claiming portability.
- **WASM** and a **Windows target** are the follow-on reach once the C backend
  proves the AST→target seam. WASM needs a committed runtime in CI (wasmtime or
  node) so it can be differentially tested the same way; the Windows port is
  mostly toolchain/ABI work (PE objects, the `clang-cl`/MSVC driver, path
  handling) layered on the existing LLVM backend rather than a new code generator.

## Deferred (honest — documented, not stubbed)

These are now tracked under *Mega-arcs* above; the notes here record *why* they
are deferred rather than merely unscheduled:

- **Third-party package registry** — resolution would go through the Bazel module
  registry, but Bazel can't run in this build environment, so a real registry
  integration isn't verifiable here; only `mod foo;` + `kard.toml` local-path
  deps ship today.
- **A normative language spec + conformance suite** and a **stability / SemVer
  policy** — appropriate once the language stops changing core semantics (it is
  still pre-1.0, and the v24–v36 plan changes the surface substantially).
- **WASM / Windows backends** — each needs a committed CI runtime / toolchain
  (wasmtime or node for WASM; `clang-cl` / MSVC for Windows) to be differentially
  gated the way `--emit-c` is; they wait on that infrastructure.
