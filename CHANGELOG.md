# Changelog

All notable changes to kardashev are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Versioning

kardashev uses [Semantic Versioning](https://semver.org). It is **pre-1.0**, so
each completed **roadmap** is a `MINOR` bump (Roadmap v9 → `0.9.0`, v10 →
`0.10.0`) and bug-fix releases bump `PATCH`. Per SemVer's 0.x rule anything may
change between minors until 1.0. `1.0.0` is reserved for a language-surface
**stability commitment**; after it the language evolves via opt-in **editions**
(the Rust model) rather than `MAJOR` bumps. The version lives in
`compiler/include/kardashev/version.hpp` (reported by `kardc --version`),
`MODULE.bazel`, and here.

`0.9.0` is the first tagged release; the entries below `0.9.0` document the
pre-tag roadmap history (Phases 0–56), each of which shipped fully green (6 unit
suites + the smoke aggregate, JIT **and** AOT).

## [0.34.0] — Roadmap v34 "metaprogramming: macros, derive & comptime" (Phases 182-186)

Theme: give the language the tools to abstract over syntax and shift work to
compile time — declarative macros, user-defined derives, operator overloading,
richer `const fn` evaluation, and conditional compilation. Every phase is
differentially gated (JIT vs AOT).

### Added
- **Declarative `macro_rules!` macros** (Phase 182) — a real token-level macro
  system. `macro_rules! name { (matcher) => { body }; … }` defines rules, and
  `name!( … )` / `name![ … ]` / `name!{ … }` invocations are rewritten into the
  first matching rule's body before parsing, so a macro can expand in
  expression, statement, OR item position. Supports multiple rules (selected by
  shape), fragment metavariables (`$x:expr | ident | literal | ty | pat | tt |
  …`), one level of repetition `$( … )sep* / + / ?` in both matcher and body,
  and recursion (a variadic `sum!` reduces one element per re-invocation). A new
  `$` token carries metavariables. The built-in format macros (`format!` /
  `println!` / `print!`) are untouched and compose with user macros.
- **User-defined `#[derive(...)]`** (Phase 183) — a library author writes a
  custom derive as a `macro_rules! derive_Foo` whose matcher destructures the
  item (e.g. `struct $name { $($f:ident : $t:ty),* }`) and whose body emits an
  `impl`; `#[derive(Foo)]` then synthesizes the expansion automatically. User
  and built-in derives (Clone/Eq/Debug/…) compose on the same attribute. The
  macro matcher is now recursive over delimiter groups, which also enables
  map-literal-style macros (`m!{ k => v, … }`).
- **Operator overloading** (Phase 184) — a user type opts into `+` / `-` / `*`
  / `/` by implementing the prelude `Add` / `Sub` / `Mul` / `Div` trait
  (`fn add(self, rhs: Self) -> Self`); the binary operator desugars to the
  method. Operator traits are pure (effect-free), so an `impl` body is pure too.
- **Richer comptime / `const fn`** (Phase 185) — a `const fn` can now use the
  imperative `let mut … ; while … { … }` style with variable reassignment and
  early `return`, all evaluated at compile time (iterative factorial /
  fibonacci, running sums — usable as `const` values and array lengths). A
  non-terminating const loop fails against the global step budget instead of
  hanging the compiler.
- **`#[cfg(...)]` conditional compilation** (Phase 186) — items can be gated on
  build flags set with `--cfg NAME` / `--cfg key=value`. Predicates: a bare
  flag, `not(…)`, `all(…)`, `any(…)`, and `key = "value"`. A disabled item is
  dropped during parsing (before type checking, so it may even reference
  undefined types). Active flags fold into the AOT cache key.

### Deferred / honest limitations
- **Macro hygiene** is not implemented — expansions are unhygienic (avoid
  capturing identifiers); nested repetitions and a metavariable in the matcher
  *after* a repetition are rejected with a clear error (never miscompiled).
- **`#[cfg]` from a `kard.toml` `[features]` table** — the `--cfg` mechanism is
  the engine; auto-feeding it from a manifest section is a thin follow-on.
- Operator overloading is homogeneous (`Self`-typed `rhs` and result); `Index` /
  `Deref` / `Neg` and heterogeneous / custom-`Output` operators are deferred.

## [0.33.0] — Roadmap v33 "systems-grade: FFI, `unsafe` & overflow control" (Phases 177-181)

Theme: the systems-programmer escape hatch. Raw pointers + `unsafe`, a more
mature C FFI surface, and explicit integer-overflow control. Every phase is
differentially gated (JIT vs AOT); the FFI phase is verified against real
libm/libc.

### Added
- **Raw pointers + `unsafe` blocks** (Phase 177) — `*const T` / `*mut T` raw
  pointers (NOT borrow-checked, nullable, lowering to the same opaque pointer as
  `&T`; a `&T` never unifies with a `*const T`) and `unsafe { … }` blocks. Create
  a raw pointer from a reference (`&x as *const T`, safe), dereference-READ it
  inside `unsafe` (a deref outside is a type error), and cast reference↔rawptr
  (no-op) / rawptr↔integer-address (`ptrtoint` / `inttoptr`). `effect` / `handle`
  / `with` / `perform` / `unsafe` are contextual keywords, so existing
  identifiers (a task `handle`, …) keep working.
- **FFI maturity — scalars + pointers** (Phase 178) — `extern "C"` signatures,
  which were limited to i32/i64/bool/&String, now also accept f64 / f32 (C
  double / float), the full integer width tower (i8..i64 / u8..u64), and (via
  Phase 177) `*const T` / `*mut T` as a C pointer. This covers the bulk of real C
  interop — libm math and the pointer-taking libc/buffer APIs — verified end to
  end against real `sqrt`/`pow`/`memset`/`memcpy`/`abs`.
- **Overflow-checked + wrapping arithmetic** (Phase 181) — the integer-overflow
  policy is documented (the DEFAULT is 2's-complement WRAP, `-fwrapv`) and joined
  by explicit opt-in operators: `checked_add/sub/mul/div(a, b) -> Option<i64>`
  (`None` on signed overflow / div-by-zero / `INT_MIN / -1`) and
  `wrapping_add/sub/mul(a, b) -> i64`. Overflow is detected with portable
  sign-bit identities / a 128-bit widen-and-compare (no version-fragile
  `*.with.overflow` intrinsics).

### Deferred / honest limitations
- **Phase 179 (`no_std` / freestanding + a pluggable allocator)** and **Phase
  180 (inline asm + SIMD intrinsics)** are NOT in this release. A pluggable
  global allocator means rerouting the core libc malloc/free/realloc path (a
  risky change to every heap type), full `no_std` conflicts with the
  libc-dependent prelude runtime (print/String/Vec), and inline asm / SIMD are
  platform-specific and not portably verifiable here. Tracked in ROADMAP.md as
  future systems work (alongside Phase 174's multi-threaded executor).
- Raw-pointer WRITE (`*p = v`) needs deref-assignment (unsupported
  language-wide); pointer ARITHMETIC; struct-by-value / callbacks / bindgen
  across `extern "C"` (the harder FFI-maturity pieces) — all deferred.

## [0.32.0] — Roadmap v32 "async & effects, matured (differentiator II)" (Phases 172-176)

Theme: take the two features that most distinguish kardashev — its async runtime
and its zero-cost effect system — from "they exist" to "they compose." Future
combinators + a type-safe task API, async cancellation/timeouts, effect
subtyping, and the research-frontier headline: user-defined **algebraic effects
with handlers**. Every phase is differentially gated (JIT vs AOT) and the
heap/leak-sensitive ones under `MALLOC_CHECK_`.

### Added
- **Future combinators + a type-safe task API** (Phase 172) — four
  compiler-synthesized combinator futures: `future_map<T,U>(Future<T>,
  fn(T)->U)` , `future_and_then<T,U>(Future<T>, fn(T)->Future<U>)` (monadic
  bind), `future_join2<A,B>(Future<A>, Future<B>) -> Future<(A,B)>` (wait-all),
  and `future_select<A,B>(…) -> Future<Either<A,B>>` (wait-any, drops the loser)
  — plus a new prelude `enum Either<A,B> { Left, Right }`. The combinators thread
  the continuation's effects to the call site via the existing effect-row var
  (`future_map` of a pure closure is pure; of an `io` closure is `io`). And
  **`JoinHandle<T>`**: `spawn` now returns a move-only, result-typed handle that
  `join` consumes — so double-joining (a double free) is a compile error.
- **A pre-existing codegen bug, fixed** (surfaced by Phase 172) — malloc sizes
  were baked with LLVM's default DataLayout (i64 under-aligned to 4) while
  StructGEP offsets lower against the host layout (i64 align 8), so a
  `Poll<multi-payload-enum>` slot was under-allocated by 8 bytes — an 8-byte heap
  overflow on `block_on` of such a future (also hit `block_on(async fn ->
  Result/Option)`). The host DataLayout is now pinned before the codegen walk.
- **async timeouts + cancellation** (Phase 173) — `timeout<T>(Future<T>, ms) ->
  Future<Option<T>>` races a future against an internal `sleep_ms` timer
  (`Some(v)` if it finishes first, `None` on timeout); `task_cancel<T>(
  JoinHandle<T>)` retires + releases a spawned task (and consumes the handle, so
  a cancelled task can't be joined). With `future_join2` these are the
  structured-concurrency primitives.
- **Effect subtyping** (Phase 175) — a function value that performs FEWER effects
  is now usable where one with MORE effects is expected (subsumption): a pure
  `fn()->R` coerces into a `fn()->R ! {io}` parameter. One-way and sound (an
  actual that does more than expected is still rejected); the `! {e}` effect-row
  threading of `vec_map`/`future_map` is unchanged.
- **User-defined effects + effect HANDLERS — algebraic effects** (Phase 176, the
  headline) — `effect E { fn op(a: A) -> R; … }` declares an effect and its
  operations; `perform E::op(args)` invokes the dynamically-current handler and
  RESUMES at the call site with its result; `handle { body } with E { op(p) =>
  hbody, … }` installs handlers for the body's dynamic extent and DISCHARGES `E`
  from the body's effect row (the way `catch` clears `panic`). Handler arms
  desugar to by-reference-capturing closures, so multiple arms share live
  handle-scope state — a `State` effect's `get`/`put` operate on one cell. This
  is the **tail-resumptive / dynamically-scoped subset** (reader, state, logging,
  dependency injection), implemented over a per-(effect,op) current-handler
  global with save/restore. `effect`/`handle`/`with`/`perform` are CONTEXTUAL
  keywords (a variable named `handle` still works).

### Deferred / honest limitations
- **Phase 174 (multi-threaded work-stealing executor + macOS `kqueue`)** is NOT
  in this release. The async executor remains single-threaded (cooperative) with
  an `epoll` reactor on Linux; a parallel work-stealing executor and a `kqueue`
  reactor are substantial, separately-verifiable future work and are tracked in
  ROADMAP.md.
- **Algebraic effects** ship as the tail-resumptive subset only: no non-tail
  resume, no multi-shot resume, no abort-without-unwind, and a `handle` body must
  not `return` through the handle. `future_select`/`timeout`/`task_cancel` drop a
  loser/cancelled task SHALLOWLY — a mid-flight async-fn loser leaks its nested
  in-flight sub-frame (memory-safe; a recursive `Future`-drop is future work).

## [0.31.0] — Roadmap v31 "concurrency, hardened (differentiator I)" (Phases 167-171)

Theme: take the concurrency story from "structural Send + a type-erased i64
`Mutex` + i64-only threads" to a hardened, modern surface — real `Send`/`Sync`
marker traits, RAII lock guards + `RwLock`, real lock-free atomics, channel
`select` + scoped threads, and atomically-refcounted `Arc`/`Weak`. Every phase
is differentially gated (JIT vs AOT, and the concurrent ones by a
deterministic-over-N-runs stress oracle — a lost update / data race fails
flakily, which repeating catches; the RAII/refcount ones also under
`MALLOC_CHECK_`).

### Added
- **Real `Send`/`Sync` marker traits** (Phase 167) — `Send` and `Sync` are now
  declarable zero-method marker traits (in the prelude), auto-derived
  structurally, manually grantable (`impl Send for Opaque {}`), and opt-out-able
  via a new negative-impl syntax `impl !Send for T {}`. A marker oracle consults
  explicit positive/negative impls and otherwise falls through to the
  (byte-identical) structural rule — so the three live enforcement sites
  (`chan_send` value, `mutex_new` cell, by-value `thread_spawn` capture) are now
  overridable. Fixes a latent gap: `char` (a Copy scalar) is now `Send`/`Sync`.
  Zero runtime cost.
- **Type-safe `RwLock<T>` + RAII lock guards** (Phase 168) — a new reader/writer
  lock (`pthread_rwlock_t`-backed, mirrors `Mutex`) plus move-only RAII guards
  `MutexGuard<T>` / `RwLockReadGuard<T>` / `RwLockWriteGuard<T>` that auto-release
  the lock on `Drop` (the scoped-lock pattern, à la C++ `lock_guard` /
  `shared_lock`). `RwLock`'s cell is `Send`-gated like `Mutex`'s.
- **Atomics + CAS + memory orderings** (Phase 169) — `AtomicI64` / `AtomicBool`
  (Copy `Send`+`Sync` handles) with `load`/`store`/`swap`/`fetch_add`/`sub`/`and`
  /`or`/`xor`/`compare_exchange`, lowered to real LLVM `atomicrmw`/`cmpxchg`/
  atomic-load/store/`fence`. The memory ordering is baked into the op name so the
  LLVM `AtomicOrdering` is a compile-time constant; an ergonomic
  `enum Ordering { Relaxed, Acquire, Release, AcqRel, SeqCst }` + `impl` layer
  (prelude) dispatches to the statically-named builtins. (`--emit-c` refuses
  atomics — the LLVM path is the oracle.)
- **Channel `select` + scoped threads** (Phase 170) — `select2`/`select3`/
  `select4(&r0,..)` block (poll-with-backoff) until one of N homogeneous
  `&Receiver<T>` is ready, returning a prelude
  `SelectResult<T> { Ready(idx, value), Closed(idx) }`. Scoped threads: a
  move-only `Scope` (`scope_new` / `scope_spawn(&s, f)`) whose `Drop` JOINS every
  thread it spawned — the roadmap's "all threads join before the scope ends", via
  RAII. (True cross-thread *borrow* capture is deferred — it needs a
  region/lifetime system; workers capture by value as `thread_spawn` does.)
- **`Arc<T>` / `Weak<T>`** (Phase 171) — atomically reference-counted shared
  ownership: a pointer to `{ i64 strong, i64 weak, T value }` with atomic
  refcounts (clone Relaxed, drop Release + an Acquire fence on the last strong;
  value dropped at strong==0, block freed at weak==0). `Weak<T>` is a non-owning,
  upgradable handle (`weak_upgrade -> Option<Arc<T>>` via an atomic CAS loop).
  Unlike `Rc`, `Arc`/`Weak` ARE `Send`+`Sync` when `T` is (`Send`+`Sync`) — the
  answer to "share owned data across threads" without lifetimes. Capturing an
  `Arc` into a thread clones it. Proven atomic by a 4-thread × 50k clone+drop
  stress (final `strong_count == 1`).

### Deferred / honest notes
- **Generic `thread_join<T>`** (the non-i64 thread-result half of Phase 171) is
  deferred to a follow-on: it rewrites the core OS-thread runtime (per-`T`
  control block + trampoline) the whole v31 test surface depends on. OS threads
  still return `i64` (the sound current behavior); `Arc<T>` is the shipped half.
- `select` is poll-with-backoff (a true blocking multi-channel wait needs a
  shared-condvar ABI change). Scoped threads deliver join-before-scope-end but
  not borrow-capture (needs lifetimes).

### Tests
- New smoke targets `smoke_test_phase167`–`171` (JIT-vs-AOT differential; the
  concurrent ones deterministic-over-N-runs + `MALLOC_CHECK_`). Unit:
  `typecheck_test` 311 → 316, `parser_test` 138 → 139.

## [0.30.0] — Roadmap v30 "the C backend, finished II (heap + RAII + generics)" (Phases 162-166)

Theme: take the `--emit-c` C-source backend (v23/v29) from the i64/bool/struct/
enum/ref/control subset all the way to the heap + RAII + the generic surface,
each phase differentially gated against LLVM (and the memory-safety phases ALSO
gated by an AddressSanitizer + LeakSanitizer oracle — a leak/double-free/stack-
use-after-scope signal the exit-code gate can't see).

### Added (C backend, `kardc --emit-c`)
- **`String` + heap strings** (Phase 162) — a faithful C `struct kdstr { char*
  data; int64_t len; int64_t cap; }` runtime (cap==0 = borrowed literal, copy-on-
  write), mirroring the LLVM builtins exactly (string_new, str_len, str_char_at,
  str_push_byte, string_push_str, str_eq, str_substring, int_to_string, the print
  family). Emitted only when the program uses String.
- **scalar-element `Vec`** (Phase 163) — a `struct kdvec` runtime for `Vec<i64>`/
  `Vec<bool>` (push/get/get_ref/len/pop/remove/insert/reverse/swap). Also a
  soundness fix: an unimplemented builtin is now refused instead of emitting an
  undefined-symbol call.
- **`Drop` / RAII** (Phase 164) — frees non-escaping heap-owning locals AND owned
  by-value params at function exit; a binding is dropped only when every use is a
  borrow and the fn has no early return (escaping/uncertain cases leak rather
  than risk a double-free). ASan-verified.
- **closures + fn-pointers** (Phase 165) — a closure → a hoisted `__cl_<n>(void*
  env, args)` over a stack capture env (scalar by-value captures, free vars the
  backend computes itself); a fn value → the fat pointer `struct kdfn<arity>`; a
  top-level fn → a thunk. An escaping fn value (returned closure) or an FnMut
  closure is refused (ASan caught the stack-env dangle).
- **generics** (Phase 166) — a generic fn is monomorphized ONCE at int64_t (every
  scalar shares one C representation); a non-scalar or const-generic
  instantiation is refused (the backend never emits C that fails to compile).

### Deferred (documented follow-ons)
- `HashMap`/`HashSet` (a keyed-hash C runtime); non-scalar `Vec`/generic
  instances (struct/String elements); user `impl Drop`; heap locals in nested
  blocks / on early-return paths.

718 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.29.0] — Roadmap v29 "the C backend, finished I (aggregates + control)" (Phases 157–161)

Theme: grow the `--emit-c` C-source backend (v23) from the i64/bool scalar
subset to aggregates + the full control surface, each phase differentially
gated against the LLVM backend (the LLVM-AOT exit code must equal the
emitted-C-compiled-by-the-system-cc exit code).

### Added (C backend, `kardc --emit-c`)
- **Structs** (Phase 157) — typedefs (inner-before-outer order), struct literals
  as C designated-initializer compound literals, field access/assignment, and
  struct-typed lets/params/returns. The backend is now type-aware (a value is
  `int64_t` or `struct <Name>`), not "everything is int64_t".
- **Enums + `match`** (Phase 158) — an enum lowers to a tagged struct
  `struct E { int64_t tag; int64_t p0..; }`; a variant constructor is a compound
  literal; `match` lowers (without the LLVM decision tree) to an if/else chain on
  the tag (enum) or value (int), binding scalar payloads from `.p<i>`.
- **References / borrows** (Phase 159) — `&T`/`&mut T` → C pointers; `&x`,
  `&<temporary>` (a pointer to a C99 block-scoped compound literal), `*r`, and
  `r.field` auto-dereferencing to `(*r).field`; plus unit-returning fns.
- **`for` / `loop`-with-value + multi-file modules** (Phase 160) —
  `for x in a..b` → a C `for`; `loop { … break v; }` → a `while (1)` yielding the
  break value; and `mod foo;` programs are merged (resolveModules on the raw
  source, sans prelude) so the C backend sees every module's fns.
- **A randomized C-vs-LLVM differential oracle** (Phase 161) — generates many
  random programs over the subset (arithmetic, comparisons, `&&`/`||`, nested
  if/else, helper fns, while loops) and asserts LLVM-AOT exit == `--emit-c` exit.

Out-of-subset code (traits/impls/strings/Vec/Drop/closures/generics/async) is
still refused with a clear error — the backend never emits wrong C. A
match-through-reference that binds a payload is a documented follow-on.

718 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.28.0] — Roadmap v28 "const-eval & generics, finished" (Phases 152–156)

Theme: finish the const-evaluator and the generics story — aggregate consts,
non-i64 const-generics, deeper inference, GATs, and monomorphization control.

### Added
- **const-eval beyond i64/bool** (Phase 152) — array / tuple / struct / enum
  `const` values, built and projected (`A[i]`, `p.field`, `t.0`) at compile time
  with const bounds-checking, and usable as runtime values (the initializer is
  re-emitted per use, Rust-style).
- **const-generics beyond i64** (Phase 153) — a `const N` parameter may be
  `i64`, `bool`, or `char`; a value-use has the param's type at the right width;
  a binding's type annotation supplies the const arg (expected-type propagation).
- **bidirectional inference** (Phase 154) — struct-literal field values get the
  same coercions a fn argument does (an unannotated `None` infers from the field,
  int literals narrow). Fixed a real mutual-resolution bug: a **generic enum as a
  struct field** (`struct H { m: Option<i64> }` built with `Some`/`None`) used to
  fail to type-check; now resolved via a second field-resolution round.
- **generic associated types (GATs)** (Phase 155) — `type Out<T>;` in a trait,
  `type Out<T> = Pair<T, T>;` in an impl, projected as `Self::Out<i64>` →
  `Pair<i64, i64>` (the concrete-`Self` case), with arity checking.
- **monomorphization control** (Phase 156) — generics are monomorphized on
  demand and deduplicated (each instance emitted once); a concrete impl
  **specializes** (beats) a bounded blanket impl; and `kardc --mono-report`
  prints the monomorphization footprint (code-bloat visibility).

### Deferred (documented follow-ons)
- `char` / `f64` *scalar* consts (the integer evaluator + const-use codegen
  width handle i64/bool today).
- Turbofish (`f::<T>()`) and a GAT projection on a *bounded generic param*
  (`C::Out<i64>`); enum const-generic params.

718 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.27.0] — Roadmap v27 "strings, text & formatting" (Phases 147–151)

Theme: make text a first-class, correct part of the language — a real `char`
type, UTF-8 correctness, and a `format!` story with `Display`/`Debug`.

### Added
- **A real `char` type** (Phase 147) — a Unicode scalar, distinct from the
  integer tower (lowers to an i32 codepoint). Char literals `'a'` with escapes
  (`\n \t \r \\ \' \0` and `\u{HEX}`); equality/ordering (no arithmetic); `char
  as <int>` / `<int> as char` casts; char literal patterns in `match`; and real
  UTF-8 char↔string bridges (`char_to_string`, `char_from_u32` validating to
  U+FFFD, `str_push_char`, `print_char`). A Copy scalar.
- **UTF-8 correctness** (Phase 148) — char-aware operations over a String's
  bytes: `str_char_width_at`, `str_decode_char_at`, `str_char_count` (chars vs
  `str_len`'s bytes), `string_chars` (`-> Vec<char>`), `str_is_valid_utf8`.
- **`format!` / `print!` / `println!`** (Phase 149) — built-in formatting forms
  (there is no general macro system yet), recognized in the parser and
  desugared to string-building over `Display::to_string`. `{}` Display holes,
  `{{`/`}}` literal braces, compile-time placeholder/argument-count checking.
- **The `Debug` trait + `{:?}`** (Phase 150) — `fmt_debug(&self) -> String`,
  distinct from Display (a String is quoted + escaped, a char single-quoted).
  Built-in impls for the scalars + String; `#[derive(Debug)]` for structs
  (`Name { f: <dbg>, … }`) and enums (`Variant(<dbg>, …)`), recursing.
- **char classification + string encode helpers** (Phase 151) —
  `char_is_digit`/`_alpha`/`_alnum`/`_whitespace`, `char_to_upper`/`_to_lower`
  (ASCII), and `str_join` / `str_replace` / `str_lines`.

### Fixed
- The literal-discriminated decision-tree matcher + the codegen literal compare
  only handled `Int` columns — extended to `Char` (a char `match` was collapsing
  to the first arm + segfaulting at AOT). The borrow checker's param-type
  reconstruction now knows `char` is a Copy scalar.

### Deferred (documented follow-ons)
- A distinct borrowed `&str` type (folded into the UTF-8 work; `&String` serves
  the borrowed-string role today).
- Grapheme-cluster segmentation (UAX #29) and full Unicode case folding — both
  need the Unicode character database; scalar-level iteration + ASCII case
  mapping are what's provided.
- `{:width}` / alignment / precision format specs (only `{}` and `{:?}` today).

715 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.26.0] — Roadmap v26 "patterns, types & borrow-check completeness" (Phases 141–146)

Theme: close the long-standing gaps in pattern matching, the type surface, the
closure model, the borrow checker, and module visibility. Most of the hard
pattern features are lowered in the parser to constructs the Maranget
decision-tree matcher already handles.

### Added
- **Match guards + or-patterns** (Phase 141) — `A | B => e` arms (split into
  per-alternative arms) and `x if cond => e` guards.
- **Struct / tuple patterns** (Phase 142) — destructuring `Point { x, y }` and
  tuples in `match` arms and let-bindings (lowered to a bind + block).
- **Slice patterns** (Phase 143) — `[a, b]`, length dispatch, and a `[first, ..]`
  prefix, desugared to a length-checked `if/else` chain over `slice_len` /
  `slice_get`; `&mut [T]` mutable slices.
- **Type aliases** (Phase 144) — `type Name = Target;`, resolved in both the
  type checker and codegen (and carried across the module merge).
- **`Fn` / `FnMut` / `FnOnce` closure-trait hierarchy** (Phase 145) — every
  closure is classified by how it uses its captures (reads → `Fn`, mutates →
  `FnMut`, moves a capture out → `FnOnce`). A parameter may be spelled
  `Fn(A) -> R` / `FnMut(A) -> R` / `FnOnce(A) -> R`; the checker enforces the
  lattice `Fn < FnMut < FnOnce` at coercion sites. The bound is compile-time
  only (shared fat-pointer ABI), so accepted programs lower identically.
- **Two-phase borrows** (Phase 146) — a `&mut place` taken as a call argument
  (or a `&mut self` receiver) is a *reserved* borrow that does not conflict with
  a `&place` read nested in a sibling argument, so `vec_push(&mut v, vec_len(&v))`
  (the `v.push(v.len())` shape) compiles. Genuine aliasing — `f(&mut v, &v)` as
  direct sibling args, or two `&mut v` in one call — is still rejected.
- **Module visibility** (Phase 146) — `pub(crate)` / `pub(super)` / `pub(self)` /
  `pub(in path)` parse; `pub(self)` is private (path-unreachable), the rest are
  reachable in this crate. Enforced through the existing path-qualified-call
  visibility check.
- **`use` / `pub use` imports** (Phase 146) — `use a::b::c;`, `use a::b as c;`,
  `pub use a::b;`. A plain import is a scope hint; `use ... as` synthesizes a
  forwarder so the alias is callable; importing a private fn is a `use error`.

### Deferred (documented follow-ons)
- Full NLL region inference, implicit `&T`-field reborrows, and the
  mut-second-argument two-phase case (the borrow checker stays position-based
  NLL-lite).
- Cross-crate visibility distinctions (`pub` vs `pub(crate)`) — collapse to
  "reachable within the crate" until a real crate boundary exists (the
  package-ecosystem arc); type/const `use` aliases and generic/async alias
  forwarders.
- Owned (by-move, non-Copy) closure captures / a true runtime `FnOnce` — needs a
  closure-env drop vtable (fat-pointer ABI change).

710 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.25.0] — Roadmap v25 "the trait system, finished" (Phases 135–140)

Theme: bring the trait system from MVP to a usable vocabulary — default methods,
inheritance, blanket impls, coherence, associated consts, and the standard
conversion traits. The enabling utility is a new AST deep-clone.

### Added
- **Default trait methods** (Phase 135) — a trait method may carry a `{ body }`;
  impls inherit it unless they override. A `fillTraitDefaults` pass synthesizes
  the method into each impl (deep-cloning the default via the new `ast_clone`),
  so the type checker / codegen treat it like a hand-written method (a default
  may call abstract or other default methods).
- **Supertraits** (Phase 136) — `trait Ord: Eq + …`; a type impl'ing a trait
  must also impl every supertrait (enforced at the impl site), and a subtrait's
  default can call supertrait methods.
- **Blanket impls** (Phase 137) — `impl<T: Bound> Trait for T`, expanded into
  concrete `impl Trait for X` for every user type X satisfying the bound.
- **Coherence / overlap checking** (Phase 138) — two impls of the same trait for
  the same type (explicit, or two overlapping blankets) are rejected; a precise
  per-instantiation key keeps `Pair<i64>` and `Pair<bool>` distinct.
- **Associated consts** (Phase 139) — `const N: T;` in a trait and `const N: T =
  expr;` in an impl, read as `Type::N()` (desugared to a no-self method).
- **`From` / `Into` conversion traits** (Phase 140) — added to the prelude
  (`Target::from(x)` / `x.into()`), generic over the source/target type.

### Internal
- **`ast_clone`** — a deep-clone of expression/statement/pattern subtrees (the
  AST is move-only `unique_ptr`s), reused by the default-method and blanket-impl
  expansion passes.

No new operator sugar (`Deref`/`Index` auto-coercion is v34); `Self::N()` from
within a method and the `From`↔`Into` auto-blanket are documented follow-ons.
704 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.24.0] — Roadmap v24 "diagnostics & the developer surface" (Phases 130–134)

Theme: developer experience — the highest-ROI gap on the road to production.
Errors went from one-line `<kind> error N:M: message` (where N indexed into the
~450-line prelude-prepended source, so a user error on line 3 reported "455") to
real, navigable diagnostics, plus a lint pass, error codes, and doc comments.

### Added
- **Rich diagnostics** (Phase 130) — a rustc-style source snippet with a caret
  under the offending column, the **user's own line number + file** (the prelude
  offset is recovered), and positions embedded in messages rewritten to match
  (`moved at 457:18` → `moved at 5:18`).
- **An opt-in lint pass `kardc -W`** (Phase 132) — **unused `let`** bindings (a
  sound complete-AST use-walk: a name used via a closure, fn-pointer call,
  method/builtin call, or match binding does not warn; `_`-prefixed skipped) and
  **unreachable code** after `return`/`break`/`continue`. Non-fatal and opt-in,
  so the existing corpus is unaffected.
- **Error codes + `kardc --explain Exxxx`** (Phase 133) — a curated table tags
  the common diagnostics (`E0308` mismatched types, `E0382` use-of-moved, …)
  rustc-style (`type error[E0308]:`); `--explain` prints an extended explanation.
- **`///` doc comments** (Phase 134) — captured in the AST and surfaced both by
  the formatter (`kardfmt` round-trips them) and in **LSP hover** (rendered as
  prose below the signature).

### Changed
- **Parser panic-mode error recovery** (Phase 131) — after a statement parse
  error the parser resynchronizes to the next `;`/boundary, so it reports one
  diagnostic per real error (two malformed `let`s → 2 errors, not 4) while still
  surfacing the later errors. Recovery only runs on error; valid programs are
  byte-for-byte unaffected.

No language-surface changes; the diagnostic header keeps `<kind> error` + the
message (plus an optional `[Exxxx]`), so message-grepping tooling/tests still
match. 704 unit cases (6 suites) + the full smoke sweep green, JIT and AOT.

## [0.23.0] — Roadmap v23 "a second backend" (Phase 129)

Theme: break the LLVM/Linux monoculture. kardashev gains a **second code
generator** — a C-source backend — chosen first because it is the most
*verifiable* option here (a C toolchain is present), so it can be
differentially gated against the LLVM backend. This release lands the
foundational subset; the backend grows by subset (structs → enums + match →
strings/Vec → Drop) in later phases, with WASM and a Windows target as the
follow-on reach.

### Added
- **`kardc --emit-c` — a C-source backend** (Phase 129). Walks the same
  typechecked AST that the LLVM backend lowers and emits portable C, compiled by
  the system C compiler. The supported SUBSET: `i64`/`bool`, the full operator
  set (arithmetic / comparison / `&&` `||` / bitwise / unary `- ! ~`), `let`
  (incl. `mut` + assignment), `if`/`else` as a value, `while`, blocks, direct
  calls + recursion + mutual recursion (forward prototypes), and top-level
  `const`. Everything maps to `int64_t`, which is sound for the subset under
  `cc -fwrapv` (two's-complement overflow, truncating `/`, dividend-signed `%`,
  arithmetic `>>`, short-circuit `&&`/`||` — all matching kardashev's i64
  semantics). Expression-oriented constructs (block / `if` / `while` as a value)
  use GNU statement-expressions `({ ... })`. Anything outside the subset
  (structs / enums / match / strings / `Vec` / closures / `Drop` / references /
  generics / async / `mod` / ...) is **refused with a clear error** — the
  backend never emits wrong C.
- **Differential gating** (`tests/smoke_test_phase129.sh`) — for each of 12
  subset programs (recursion, mutual recursion, `while` + nested `if`,
  early-return, every operator, signed modulo, `const`, `bool`) the LLVM AOT
  exit code equals the emitted-C (`cc -fwrapv`) exit code; an out-of-subset
  struct program is cleanly refused. Skips cleanly when no C compiler is present
  (the LLVM path is unaffected).

The C backend is an `emit_c` library with **no LLVM dependency**; the front-end
(parse → derive → typecheck → borrow-check) is shared with the LLVM path, and
emission re-parses the raw user source so the auto-injected prelude doesn't trip
the subset check. 704 unit cases (6 suites) + the full smoke sweep green.

## [0.22.0] — Roadmap v22 "ergonomics, docs, and platform hygiene" (Phases 124–127)

Theme: two small but long-requested surface ergonomics, an honest docs pass, and
a CI-stability tweak. The second-backend exploration is broken out to **v23** —
a full second code generator is its own roadmap, planned (a differentially-gated
C backend first) rather than rushed.

### Added
- **`||` short-circuit logical-or** (Phase 124) — resolves the long-standing
  collision with the zero-parameter closure `|| body`. Disambiguation is
  positional: `||` is logical-or in infix position (after an operand) and a
  closure at the head of an expression, so the two never alias. `||` binds looser
  than `&&` (`a || b && c` is `a || (b && c)`); the lowering mirrors `&&`'s
  short-circuit (a branch + phi, flipped — lhs true skips the rhs). Pinned by
  `tests/smoke_test_phase124.sh` plus parser-precedence and codegen
  short-circuit unit cases.
- **`&<temporary>`** (Phase 125) — taking a reference to an rvalue (`&A(10)`,
  `&5`, `&Foo { .. }`, `&(a + b)`, a nullary variant `&Nil`) now works: the value
  is materialized into a fresh entry-block slot (one slot reused across loop
  iterations, like a `let`), registered as a droppable temporary dropped at scope
  exit, and its address is the borrow. Previously this was a hard codegen error;
  the documented `let`-first workaround is no longer needed. A droppable
  temporary (`&Text(int_to_string(i))`, an enum owning a heap String) in a 500k
  loop drops exactly once — RSS flat, `MALLOC_CHECK_=3` clean. Pinned by
  `tests/smoke_test_phase125.sh`.

### Changed
- **Language-reference + stdlib docs reconciled with reality** (Phase 126) — `%`,
  `&&`/`||`, `&` of a literal/temporary, and enum-typed struct fields were all
  listed as deliberate limitations but compile today (Phases 33 / 36 / 124 / 125).
  The honesty note, the lexical-structure operator table, the enum-field section,
  the surface-limitations list, and the stale "Roadmap v5" version headers are
  brought in line with the implementation and the test suite. doclint stays green.
- **macOS `codegen_test` flaky-retry residual cut** (Phase 127) — the macOS-arm64
  ORC-JIT teardown abort (~50%/run, confirmed non-deterministic; root cause needs
  macOS-arm64 hardware) goes from 3 to 5 `--flaky_test_attempts`, scoped by regex
  to that one target so a real regression elsewhere is never masked (~12.5% → ~3%
  residual). The test is deterministic on Linux, so a genuine regression still
  fails all attempts.

### Fixed
- **`&` of a unit/void temporary no longer crashes** — `&()`, `&{ }`, and
  `&<unit-returning call>` reach the new materialization path; a void value has
  no storage, so they now report a clean codegen error instead of building an
  invalid `alloca void` (which aborted). Guarded in `emitRefToTemporary`.

No new language surface beyond `||` and `&<temporary>`; one deliberate change —
`&A(10)`-style ref-to-temporary now compiles where it previously errored, so the
`smoke_test_diag` known-bad program is repointed to `&()`. 704 unit cases (6
suites) + the full smoke sweep green on Linux, JIT and AOT.

## [0.21.0] — Roadmap v21 "prove it, and close the gaps" (Phases 120–123)

Theme: turn anecdotes into numbers, fix the real footprint leak, and close the
two most-cited stdlib/MVP gaps. v21 has no new surface syntax — it makes the
existing language honest: measured, leak-free, and less `i64`-shaped.

### Added
- **Benchmark suite** (Phase 120, `bench/` + `BENCHMARKS.md`) — each workload
  written identically in kardashev and C, AOT-compiled (`kardc -O2` / `clang
  -O2`), run best-of-3 with outputs checked equal. Result: kardashev is
  **C-competitive** — `fib` ≈ 1.0×, `collatz` ≈ 1.0×, a tight integer `loop` ≈
  2.2× C. Correctness pinned by `tests/smoke_test_bench.sh`; the ratios are
  committed in `BENCHMARKS.md`. This replaces the old "−O2 default / flat RSS"
  anecdotes with data (and flags the ~2.2× tight-loop gap as a codegen target).
- **`HashMap`/`HashSet` `remove`** (Phase 122) — the one genuinely-missing stdlib
  operation. `hashmap_remove<K,V>(m: &mut HashMap<K,V>, k: K) -> Option<V>`
  (value moved out, key dropped) and `hashset_remove<T>(s: &mut HashSet<T>, k: T)
  -> bool`. Open-addressing deletion uses **backward-shift** (Knuth Algorithm R),
  not tombstones, so `get`/`insert`/`grow` are untouched and the table never
  fills with tombstones (no load-factor or infinite-probe regression). Pinned by
  `tests/smoke_test_hashremove.sh`: head/middle/tail + wrap-around chain
  preservation, a 50-key oracle, and heap-clean String-map remove + a 200k churn
  loop under `MALLOC_CHECK_=3` (RSS-flat).
- **Generic `Mutex<T>` cell** (Phase 123) — `Mutex` was `Mutex<i64>` only; its
  guarded cell is now an arbitrary `T`, so you can guard a struct, `String`,
  `bool`, `Vec`, … including shared across threads (a `Mutex<Counter>`
  read-modify-write under lock lands on the exact total). It is a **phantom-typed
  named `Mutex<T>`**: the value is a bare i64 handle (Copy, captured by value into
  thread closures), but the type carries the cell `T`, so `mutex_get`/`mutex_set`
  are *tied* to it — `T` flows from the handle (no annotation needed) and a
  wrong-`T` get/set is a compile error (closing a heap-overflow/punning hole an
  earlier type-erased draft had — found in adversarial review). `mutex_new`/`get`/
  `set` are specialized per cell type over a `{ pthread_mutex_t, T }` block; `get`
  clones the cell and `set` drops the old value (a `Mutex<String>` over 100k sets
  is RSS-flat). The cell `T` must be **`Send`** and not a shared handle
  (`Rc`/`Sender`/`Receiver`) — enforced at `mutex_new`, so a non-Send value can't
  be smuggled across a thread boundary through the cell. Pinned by
  `tests/smoke_test_mutex_generic.sh` (positive cells + 3 negative soundness
  repros).

### Fixed
- **`spawn` + `join` frame leak** (Phase 121) — the async executor leaked a heap
  frame per spawned task (its task array grew unbounded), because `join` drove +
  read the result but never reclaimed the task (unlike `block_on`, which reaps).
  A naïve reap-after-join is *wrong* — driving one handle also completes sibling
  tasks (the executor interleaves), so an all-done reap frees a sibling's result
  before its own `join` reads it. Fixed with a **per-handle release**
  (`__kd_exec_release(h)`): free only task `h`'s frame+slot, resetting the
  executor only once every task is released. A spawn+join loop is now RSS-flat
  and multi-handle joins return the right distinct results
  (`tests/smoke_test_spawnleak.sh`). *(Measurement also confirmed the previously
  suspected HashMap interior-drop and `block_on`/`await` frame reclaim are
  already clean — only `spawn`/`join` leaked.)*

### Notes
- **Still MVP (documented, not stubbed):** the const-eval scalar set (`i64`/
  `bool`) and the OS-thread return value (`fn() -> i64`; async/await is the
  generic path) remain `i64`-shaped.
- **One source-level break (behavior unchanged):** the `Mutex` handle type is now
  `Mutex<T>` (was a bare `i64`). Programs that let the handle be inferred (`let m
  = mutex_new(0)`) are unaffected, but any that **named** the handle type `i64`
  (e.g. `fn bump(m: i64)`) must spell it `Mutex<i64>`. Runtime behavior is
  identical (the handle is still a Copy i64 at the ABI). No other v21 change
  alters an existing program; the HashMap/async/numeric suites and the 455 unit
  cases (155 codegen + 300 typecheck) pass unchanged.

## [0.20.0] — Roadmap v20 "toward a real bootstrap" (Phases 115–119)

Theme: move the self-hosted compiler past the toy. Through v19 "self-hosting"
meant a mini compiler that lowered a 2-type expression language to an in-process
stack VM (it emitted no real code). v20 makes it emit **real native code**,
proves that code **matches the host compiler**, and extends it to the aggregate
shapes kardashev itself is built from — **structs and enums**.

### Added
- **Real LLVM IR codegen** (Phase 115, `examples/selfhost/llvmgen.kd`) — the
  self-hosted compiler now lowers each `Expr` to SSA-form **textual LLVM IR**
  (`add`/`mul`/`icmp`+`zext`/branch-free `select`) and prints a complete module,
  so `clang out.ll -o prog && ./prog` runs **natively** — a real compilable
  artifact, not an interpreter. Differential-gated against the host.
- **A differential fuzzer over the self-hosted codegen** (Phase 116) — for many
  random valid functions with random args, the self-hosted-emitted IR (clang →
  native) must equal the host compiler's result. The self-hosted backend matches
  the host across random programs.
- **Structs** (Phase 117, `structgen.kd`) — `struct NAME { f: i64, ... }`, struct
  literals, and field access, lowered to first-class LLVM aggregates
  (`insertvalue`/`extractvalue`); every value carries its type. Differential-gated.
- **Enums + `match`** (Phase 118, `enumgen.kd`) — `enum NAME { V(i64), ... }`,
  variant construction, and `match`, with an enum as a tagged pair `{ i64, i64 }`,
  construction → `insertvalue`, and `match` → `extractvalue` + a branch-free
  **select-chain** on the tag (sound because the language is pure). Differential-
  gated across all branches/variants.

### Fixed
- **Adversarial review** (Phase 119) of the three self-hosted compilers (~80
  programs vs the host, IR validity, test honesty) found and fixed: a `match`
  whose arms return enum values lowering its select-chain as `i64` instead of the
  aggregate type (clang-rejected; host accepted); and a latent aggregate-return
  `main` mismatch. Both pinned by regression cases. IR validity and test honesty
  came back clean.

> The "self-hosting" is now well past "toy" — but it is still a **subset** (i64/
> bool + structs + enums, not all of kardashev), so this is **not** yet a true
> bootstrap. See [ROADMAP.md](ROADMAP.md).

## [0.19.0] — Roadmap v19 "hardening III" (Phases 112–114)

Theme: push differential fuzzing into the memory-safety and integer-arithmetic
codegen paths (the bug classes that mattered most), and clean up diagnostics.

### Added
- **A memory-safety fuzzer** (Phase 112) — random but borrow-valid struct
  programs: a struct with K fields, each owning a heap `String` and printing a
  unique id on `Drop`; a random subset of distinct fields is moved into a `Vec`,
  the rest drop at scope exit. Two oracles: heap-clean under `MALLOC_CHECK_=3`
  (a double-free aborts) and every id dropped EXACTLY once. A 1 M-iteration loop
  variant gates on RSS-flatness. 75 programs across 3 seeds are all sound —
  evidence the v17/v18 per-field move/drop machinery holds across varied inputs.
- **A division / modulo / bitwise fuzzer** (Phase 113) — the integer paths the
  arithmetic fuzzer skipped, and a classic miscompile source. Generates random
  `+ - * / % & | ^ << >>` programs with the kardashev source and a C-semantics
  Python reference in lockstep (truncating `sdiv`, dividend-signed `srem`,
  arithmetic `>>`). 200 programs across 4 seeds agree (JIT == AOT == reference) —
  the lowering follows C/Rust semantics, not Python's floor-mod.

### Fixed
- **Clean codegen diagnostics** (Phase 114) — when codegen reports a real error
  it kept emitting placeholder IR, and the module verifier then piled cascading
  "module verification failed" lines on top of the real diagnostic. Codegen now
  returns the real errors directly and skips the verifier when any error was
  already reported; the verifier still runs on the error-free path (catching
  codegen bugs that emit invalid IR without reporting an error).

## [0.18.0] — Roadmap v18 "hardening II" (Phases 108–111)

Theme: close the concrete gaps that dogfooding the self-hosted compiler (v15–v17)
and its adversarial review exposed, and deepen the test surface with differential
fuzzing.

### Fixed
- **Re-initializing a moved-out struct field is legal** (Phase 108) — v17's
  field-level move tracking conservatively rejected `s.a = new` after `s.a` was
  moved out. The borrow checker now clears that field from the root's moved set
  on a `root.field = v` assignment (after the RHS is consumed, so `s.a = f(s.a)`
  still flags the RHS), so the field and struct are usable again. Using a moved
  field *without* re-initializing it is still rejected.
- **A unit-returning async fn no longer crashes the compiler** (Phase 109) — an
  `async fn f(..) ! { .. } { stmt; }` (no `-> T`) SIGTRAP'd the compiler when its
  future was consumed: `block_on` / `.await` / `spawn`+`join` read the `Poll<T>`
  value slot as `T`, and for the unit result `T` maps to LLVM `void`, so a `load
  void` (and a named `void` call) emitted invalid IR. A void result now yields
  the unit placeholder without a load, and the `block_on` call is left unnamed.
  (Found by the v17 adversarial review.)

### Added
- **A differential fuzzer for the codegen path** (Phases 110–111) — generates
  random, always-valid programs and checks three oracles agree: the JIT-printed
  value, the AOT exit code, and a Python reference. Phase 110 covers arithmetic
  (`+ - * ( )` over `i64`); Phase 111 adds `let` bindings, comparisons, and
  `if/else` branch selection. Seeded for reproducibility; 500 programs across the
  two harnesses agree exactly — no miscompile found.

## [0.17.0] — Roadmap v17 "self-hosting, continued — a compiler in kardashev" (Phases 98–107)

Theme: complete the self-hosted compiler — type checker **and** code generator,
every stage written in kardashev — and fix the real compiler bugs that
dogfooding it surfaced. By the end, `examples/selfhost/compile.kd` is a mini
compiler that type-checks a whole function and compiles + runs its body.

### Added
- **A whole-function parser + interpreter** (Phase 98, `func.kd`) — parses a
  complete `fn NAME(PARAMS) -> RET { BODY }` into an `Fn` AST and interprets it
  (scope-check the body against the params, bind args, evaluate). JIT + AOT.
- **A real type checker** (Phase 101, `typeck.kd`) — past scope-checking: the
  self-hosted expression language now has **two** types, `i64` and `bool`.
  `type_of` infers each node's type against a type environment, enforcing
  arithmetic on `int×int→int`, comparison on `int×int→bool`, and an `if`
  condition that is `bool` with equal branch types — propagating a `TErr` tag on
  any mismatch.
- **A whole-function type checker** (Phase 102, `funcheck.kd`) — threads the
  checker through `fn NAME(PARAMS) -> RET { LETS ; RESULT }`: param types, `let`
  typing, and the body's type checked against the declared return type.
- **A code generator + VM** (Phase 103, `emit.kd`) — the back-end shape: lowers
  the `Expr` AST to a flat stack-machine bytecode (`PUSH/LOAD/ADD/MUL/LT/EQ/
  SELECT`) and executes it on an operand stack + register file. Proven correct
  by cross-checking every program against a tree-walking `eval`.
- **CAPSTONE: a self-hosted mini-compiler** (Phase 105, `compile.kd`) — takes a
  whole function, type-checks it, and (only if well-typed) compiles the body —
  now with `let` LOCALS lowered to a `STORE` into a register slot — and executes
  it on argument values. Ill-typed functions are rejected before any codegen.
  lex → parse → type-check → code-generate → execute, every stage in kardashev.

### Fixed
- **Field-move double-free** (Phases 99/100/106) — surfaced by self-hosting.
  Moving a non-Copy struct field by value double-freed. Phase 99 stopped the
  single-move double-free in codegen (clear the root binding's drop flag on a
  field/index partial move); Phase 100 made it leak-free with **per-field drop
  flags** so siblings still drop; Phase 106 closed the remaining **double**-move
  hole in the **borrow checker** with field-level (partial) move tracking
  (`Binding::movedFields`) — a second move of the same field, or a whole-struct
  use after a partial move, is rejected, while moving two distinct fields stays
  legal. (Found by an adversarial review of the field-move work.)
- **Unit-tail-`match` miscompile** (Phase 104) — a `match` (or any value-
  producing expression) in tail position of a unit-returning function emitted
  `ret i64` into a void function (invalid IR). The epilogue now gates `ret` vs
  `ret void` on the function's actual return type. (Found writing `emit.kd`.)
- **Field-assignment leak** (Phase 107) — `s.a = new` overwrote a droppable
  struct field without freeing the old value (RSS ballooned in a reassigning
  loop). Codegen now drops the old field value — guarded by the field's drop
  flag (so a moved-out field isn't double-freed) — before storing.

### CI
- **macOS reliably green** — the two non-deterministic macOS-only flakes
  (`codegen_test`'s arm64 ORC-JIT teardown abort, confirmed by a same-commit
  rerun; `smoke_test_executor`'s timing bounds) are marked `flaky = True` (Bazel
  retries up to 3×). A no-op on Linux, which stays deterministic, so a real
  regression is still caught by the ubuntu job.

## [0.16.0] — Roadmap v16 "self-hosting, continued" (Phases 94–97)

Theme: grow the self-hosted front (v15: lexer + parser + signature checker)
toward a full compiler — the BODY grammar: expressions, statements, scope
checking, and a function-body interpreter, all written in kardashev in
`examples/selfhost/`.

### Added
- **An expression parser + evaluator** (Phase 94, `expr.kd`) — a recursive-descent
  parser builds an `enum Expr` AST (`Num` / `Var` / `Add` / `Mul`, recursive via
  `Box`) for an arithmetic expression with VARIABLE REFERENCES (the step beyond
  `examples/calc`'s variable-free arithmetic), then evaluates it against a
  `HashMap<String, i64>` environment. Proves precedence (`a + b * 2` = 11) and
  parentheses (`(a + b) * 2` = 14).
- **A statement/block parser + evaluator** (Phase 95, `stmt.kd`) — grows the body
  to a block: `let NAME = EXPR ;` bindings + a result expression →
  `Block { lets: Vec<Stmt>, result: Box<Expr> }`, evaluated by running each `let`
  in order (extending the environment) then the result. `let x = a + 1 ; let y =
  x * 2 ; y` with `{ a: 3 }` → 8.
- **A scope/semantic checker** (Phase 96, `scopechk.kd`) — walks the block AST and
  reports UNDEFINED variable references (a `let` RHS checked before its own name
  binds; each `let` extends the scope). `… x + c` with `c` undeclared → 1 error.
- **Capstone: a function-body interpreter** (Phase 97, `interp.kd`) — ties the
  whole pipeline (lex → parse → scope-check → evaluate) into one
  `interpret(body, params, args)`: rejects a body referencing an undefined
  variable (`-1`), else binds the arguments and runs the block. `fn f(x=3, y=4)
  { let sq = x*x; let dbl = y+y; sq + dbl }` → 17.

### Notes
- A self-hosted interpreter for kardashev function bodies, written in the
  language it interprets. Surfaced two ergonomics findings handled in-source
  (candidate later-roadmap polish): a `Box`-AST child is dereferenced in `eval`
  as `&(**child)` (`&**child` doesn't parse), and the parser cursor threads as a
  `&mut Pos` struct cell since there is no `*pos = x` deref-assign of a `&mut
  i64`. All four phases green, JIT **and** AOT; Linux + macOS CI green.

## [0.15.0] — Roadmap v15 "self-hosting" (Phases 88–93)

Theme: the north-star arc toward a bootstrap — grow kardashev until a kardashev
compiler can be written *in* kardashev. v15 delivers a self-hosted compiler
**front-end** (lexer + parser + checker), each phase a real, tested kardashev
program in `examples/selfhost/`. The gating primitives already existed (file I/O
via `fs_read_to_string` → `Result<String, IoError>`; byte string access via
`str_char_at` / `str_push_byte` / `str_substring`; `enum` + `Box` for a recursive
AST; `HashMap` for symbol tables), so the front of the pipeline is expressible
today.

### Added
- **A lexer in kardashev** (Phase 88, `lexer.kd`) — scans a kardashev snippet
  byte-by-byte and groups the bytes into real tokens with correct boundaries
  (identifiers, numbers, the multi-char `->`, punctuation), whitespace skipped.
- **A token-stream lexer** (Phase 89, `tokens.kd`) — produces a `Vec<Token>` with
  each token's KIND and SPAN; the spans reconstruct via `str_substring` to `"fn"`
  / `"->"`, the typed interface a parser consumes.
- **A parser for kardashev syntax** (Phase 90, `parser.kd`) — parses a function
  SIGNATURE into a structured `FnSig { name, params: Vec<Param>, ret }` AST,
  recovering each name/type from the token spans. (Arithmetic-expression parsing
  was already shown by `examples/calc`; this parses the language's own grammar.)
- **An AST printer + round-trip** (Phase 91, `printer.kd`) — reprints the `FnSig`
  AST back to source and checks it is byte-identical, proving the AST losslessly
  captures the surface syntax.
- **A scope/semantic checker** (Phase 92, `checker.kd`) — builds a
  `HashMap<String, String>` symbol table over the AST, resolves a parameter's
  type by name, and rejects a duplicate parameter name.
- **Capstone: the front-end, end to end** (Phase 93, `front.kd`) — one program
  runs the whole front (lex → parse → check → reprint) over a function signature
  and proves it generalizes across a 2-param and a 3-param signature. A
  self-hosted compiler front-end written in the language it compiles.

### Notes
- All six phases green, JIT **and** AOT, deterministic; Linux CI green, macOS CI
  green except a flaky `codegen_test` abort (carried from v14, an arm64-JIT issue
  needing a macOS-arm64 environment). Full self-hosting (the whole compiler,
  incl. codegen) is a multi-roadmap effort the later roadmaps continue.

## [0.14.0] — Roadmap v14 "hardening" (Phases 82–87)

Theme: make the toolchain trustworthy across platforms and inputs — a green
**macOS CI** for the first time, a SIGPIPE-robust test harness, the last known
channel footgun closed as a precise compile error, and a JIT-vs-AOT differential
sweep. The consolidation roadmap after three feature roadmaps (v11–v13) that each
needed a soundness fix at review time.

### Added
- **Portable memory/leak gates** (Phase 82) — the constant-memory leak gates
  (peak-RSS checks that catch drop/refcount leaks) hard-required GNU
  `/usr/bin/time -v`, so on macOS (BSD `time`) they died under `set -euo
  pipefail` — 11 of the 12 long-standing macOS-CI failures. A shared portable
  `peak_rss_kb` (GNU `time -v` **or** BSD `time -l`, else a clean SKIP) keeps the
  gate *running* on both platforms; this took **macOS CI green for the first
  time**. Plus a CI step that dumps any failing test's `test.log` (an Aborted
  test prints nothing with `--test_output=errors`).
- **SIGPIPE-robust smoke harness** (Phases 84–85) — `echo "$big" | grep -q` /
  `awk '…exit'` / `$CMD | head -N` make the producer die with SIGPIPE (exit 141)
  when the consumer closes the pipe early — a load-sensitive flake under
  `set -o pipefail`. Swept ~51 such pipelines across 31 files to here-strings /
  capture-then-process; consumers that read to EOF (`tail`, `wc`, plain `grep`)
  left alone. const.sh went from ~3/5 to 12/12 under load.
- **The channel capture-and-keep footgun is a compile error** (Phase 86) — a
  `Sender` captured into a closure is owned by the closure's heap env, which
  never drops its captures, so the only way it is ever dropped (and the channel
  closes) is being MOVED out of the closure. The typechecker now rejects a
  captured `Sender` with no bare (by-value) use anywhere in the body — exactly
  the send-only-never-moved case that leaks and hangs a `recv`-until-`None`
  consumer. The rule is *precise* (a bare use is the only way a non-Copy Sender
  leaves an env, so sound code always has one): zero false positives across the
  whole v13 channel suite.
- **JIT-vs-AOT differential sweep** (Phase 87) — one test runs all 9 single-file
  capstones (calc, checksum, csvstats, json, kdlex, matrix, parstats, rpn,
  wordfreq) through both backends and asserts they agree. The ORC-JIT prints
  `main`'s `i64` return as a trailing line while the clang-linked AOT exits with
  it (& 255), so AOT stdout must equal JIT stdout minus that line and the line
  mod 256 must equal the AOT exit code. One place any future codegen change must
  keep green — verified 9/9 agree, on Linux **and** macOS-arm64.

### Fixed
- **jmp_buf alignment + size** (Phase 83) — the catch-stack `_setjmp` jmp_buf was
  a 1-aligned `[256 x i8]` byte array (the entry struct 264 bytes, so entries
  past the first landed at non-16 offsets). Now a generously-sized, 16-byte
  aligned `[32 x i128]` (512 bytes) cell — correct defensive hardening for any
  platform. (It did not clear the remaining macOS-arm64 `codegen_test` flake — an
  arm64-JIT-execution issue that is ASan/UBSan-clean on Linux and needs a
  macOS-arm64 environment to diagnose; tracked, not papered over.)

### Notes
- Tested green on a cleared clean build: 6 unit suites + the smoke aggregate
  (incl. the new differential + v13-review footgun checks), JIT **and** AOT.
  **Linux CI green; macOS CI green except a flaky `codegen_test` abort** (the
  9-capstone differential passing on macOS-arm64 confirms the *generated code*
  agrees across backends there — the flake is in the unit-test harness).

## [0.13.0] — Roadmap v13 "concurrency" (Phases 75–81)

Theme: make concurrency SAFE BY CONSTRUCTION — typed channels that move data
between threads, with thread-safety enforced *through the effect system* (the
language's differentiator). Designed via a 3-proposal / 3-judge multi-agent
panel (MVP-first won, grafting the structural Send/Sync rule + an `Rc` negative
witness). A pre-merge adversarial multi-agent review (3 reviewers, ~600 stress
runs) then found a use-after-free in the borrow-returning builtins and two
channel-lifecycle defects the green suite had missed — all fixed in Phase 81
(see Fixed); the Send/`share` soundness surface it hammered came back clean.

### Added
- The **`share` effect** (Phase 75) — the concurrency effect that makes
  thread-safety a CHECKED property rather than a library convention.
  `thread_spawn` now carries `share`, so a fn that spawns must declare
  `! { share }`. Because `share` is a built-in effect it rides the existing
  effect-SUBSET rule: a trait method declared without `share` can NEVER have an
  impl that spawns, so concurrent work can't be smuggled past a pure-looking
  `<T: Task>` / `&dyn Task` interface (the super-effecting impl is rejected).
  This is the value-crossing *control* half; the value-*safety* half (only
  `Send` data crosses) lands with channels in Phase 77.
- **Typed MPSC channels** (Phase 76) — `channel() -> (Sender<i64>,
  Receiver<i64>)`, `chan_send` / `chan_recv` (→ `Option`, `None` once closed
  AND drained) / `chan_close`. The runtime is an unbounded linked-list queue
  guarded by a **pthread mutex + condition variable** (`chan_recv` blocks on
  the condvar while the channel is empty and open). A producer thread sending
  1..=100 and the main thread draining sums to exactly 5050, deterministic
  across runs, JIT and AOT. `Sender`/`Receiver` are named generic structs that
  lower to an i64 handle into the channel block; a `Sender` (multi-producer,
  `Send`) crosses into a worker thread, while a `Receiver` is the
  single-consumer endpoint and is **not** `Send` (moving one into a thread is a
  compile error). *(Phase 81 made the endpoints refcounted, move-only owners so
  the block is reclaimed and the channel closes on the last sender — see
  Fixed; `chan_send`/`chan_recv` now borrow the endpoint, `sender_clone` makes
  a second producer.)*
- **Generic channels + the `Send` rule** (Phase 77) — `channel<T>` now MOVES a
  real `T` across threads (the queue node carries a `T`-sized cell, specialized
  per `T`), so an owned `String` or `Vec<i64>` is sent from one thread and
  received on another with ownership transferring sender → node → receiver,
  freed exactly once (no clone, no double-free). The structural **`Send`**
  predicate (`isSend`) gets teeth at `chan_send`: scalars / `String` / owning
  aggregates / the channel `Sender` are `Send`, while a `&T` borrow, the
  `Receiver`, and (Phase 78) `Rc` are not — sending a non-`Send` value on a
  channel is a compile error, so no borrow can dangle across a thread.
- **`Rc<T>`** (Phase 78) — a non-atomic reference-counted shared owner
  (`rc_new` / `rc_clone` / `rc_get` / `rc_strong_count`), a pointer to a heap
  `{ i64 strong, T value }`. The strong count tracks clones; the shared value
  and the block are dropped EXACTLY once when the last `Rc` drops (verified
  drop-once over a `Drop`-counted inner; 200k clone+drop pairs stay flat). It
  is the **legible non-`Send` witness**: its refcount is non-atomic, so an
  `Rc` may not cross a thread boundary (sending one on a channel is a compile
  error that names `Rc`) — the contrast to sharing safely via a `Mutex`.
- **The parallelism payoff + `chan_try_recv`** (Phase 79) — the v13 primitives
  compose into real fork-join parallelism: split 0..N across W worker threads,
  each summing its range and sending the partial on a SHARED `Sender`
  (multi-producer), with the main thread gathering the W partials over the
  MPSC channel (W producers → 1 consumer) and folding — deterministic, JIT and
  AOT. Plus `chan_try_recv` — a non-blocking receive (`Some` if ready, `None`
  if momentarily empty, never blocks on the condvar) for poll loops.
- **Capstone** `examples/parstats` (Phase 80) — "concurrency, applied": a
  parallel map-reduce, safe by construction. The series
  `data(i) = (i*7+13) mod 1000` over `0..10000` is split across 4 worker
  threads; each reduces its chunk to a `Stats` struct and SENDS it on a shared
  MPSC channel; the main thread gathers + merges into the global stats —
  deterministic and checked against the sequential answer (sum 4995000,
  count 10000, min 0, max 999). Exercises the whole v13 line at once:
  `thread_spawn` (`share`), channels moving a `Stats` struct across threads,
  the `Send` rule, fork-join, and the v12 `i64_min`/`i64_max` helpers.
- **Refcounted, move-only channel endpoints** (Phase 81, from the review) —
  the channel block now carries a mutex-guarded live-**sender** count and a
  live-**endpoint** count, and `Sender`/`Receiver` are move-only owners (not
  Copy) with drop glue. `chan_send`/`chan_recv`/`chan_try_recv` BORROW the
  endpoint (`&Sender` / `&Receiver`), so a single owner still sends/recvs in a
  loop; `sender_clone(&Sender) -> Sender` makes an additional producer, and
  capturing a `Sender` into a thread clones it automatically (each thread gets
  its own refcounted handle, dropped by the worker's by-value param). This is
  the Rust ownership model: the channel **closes when the last `Sender` drops**
  (so one producer finishing can't end the stream for the others), and the
  block — plus any queued nodes and undrained droppable payloads — is **drained
  and freed when the last endpoint drops**. `chan_close(Sender)` now consumes
  the sender (an explicit "this producer is done").

### Fixed
*(All found by the v13 pre-merge adversarial review; pinned by
`tests/smoke_test_v13_review.sh`.)*
- **Use-after-free via a borrow-returning builtin (BLOCKER).** `rc_get(&a)` and
  `vec_get_ref(&v, i)` return a `&T` that aliases the owner, but the borrow was
  not tracked against it — so `let r = rc_get(&a); consume(a); *r` compiled and
  read freed memory. The borrow checker now ties such a `let`-bound borrow to
  the owner (exactly like `let r = &a;`), so moving or dropping the owner while
  the borrow is live is a borrow error. (Closes the same hole on the
  stale-`vec_get_ref`-after-`vec_push` path.)
- **Unbounded channel leak (MAJOR).** Endpoints were Copy handles that nothing
  owned, so every `channel()` leaked its ~172-byte block (plus undrained nodes
  and their payloads) — unbounded in a channel-per-task loop. The refcounted
  move-only endpoints (Phase 81) reclaim the block, drain the queue, and drop
  remaining payloads when the last endpoint drops: RSS is now flat over
  1,000,000 created+drained channels and 200k dropped-with-undrained-`Vec`
  channels. Moving an owned value across a channel still drops it exactly once.
- **Multi-producer `chan_close` data loss (MAJOR).** Close set a single boolean,
  so any one producer closing made `chan_recv` return `None` while other live
  producers were still sending — 84/100 runs lost an entire producer's data.
  Close is now refcounted: the channel ends only when the **last** `Sender` is
  gone, so a producer finishing never abandons another's queued items (2
  producers × 300, one closing early → exactly 600 every run).

## [0.12.0] — Roadmap v12 "real stdlib" (Phases 69–74)

Theme: turn a language you can *compute* in into one you can *get data in and
out of* — parsing, richer collections, string and numeric methods. The second
step toward production use. A pre-merge adversarial multi-agent review fixed a
`parse_int` integer-overflow and a discarded-owned-temporary leak the green
suite had missed (see Fixed).

### Added
- **String → number parsing** (Phase 69): `parse_int(&String) -> Option<i64>`
  and `parse_f64(&String) -> Option<f64>` — the all-or-nothing parse a real
  stdlib needs (a string that is not *wholly* a valid number, including one
  with leading/trailing junk or whitespace, is `None`). Built on low-level
  `str_parse_i64` / `str_parse_f64` out-param primitives (C `strtoll`/`strtod`
  over a transient stack buffer, with strict full-consume + no-leading-
  whitespace validation). Plus `int_to_hex(i64) -> String` (lowercase hex, the
  two's-complement pattern for a negative). Reading data no longer needs a
  hand-rolled digit loop.
- **Vec mutation + query** (Phase 70): `vec_pop` / `vec_remove` / `vec_insert`
  / `vec_reverse` (built-ins) and `vec_contains` / `vec_index_of`
  (`Eq`-bounded prelude scans; index −1 when absent). `vec_pop` and
  `vec_remove` MOVE the element out (the length is decremented so the Vec no
  longer owns that slot — no clone, no double-free, the dual of the cloning
  `vec_get`), so they are sound for a non-Copy element type (`Vec<String>`).
  `vec_insert` grows when full and clamps its index to `[0, len]`.
- **HashMap / HashSet enumeration + membership** (Phase 71):
  `hashmap_contains(&HashMap, &K) -> bool` and `hashmap_values(&HashMap) ->
  Vec<V>` (`Eq`+`Clone`-bounded prelude scans over `hashmap_get_ref` /
  `hashmap_keys`, deep-cloning the values), plus `hashset_items(&HashSet) ->
  Vec<T>` — the first way to enumerate a `HashSet` (a codegen built-in
  delegating to the backing map's keys). `hashmap_remove` / `hashset_remove`
  are a deliberate deferral (open-addressing deletion needs tombstone-aware
  get/insert).
- **String methods** (Phase 72): `str_starts_with` / `str_ends_with` /
  `str_contains` / `str_index_of` (pure reads, substring index or −1) and
  `str_to_upper` / `str_to_lower` / `str_concat` / `str_repeat` (fresh heap
  Strings). All kardashev prelude functions over `str_char_at` / `str_len` /
  `str_push_byte` — high-level string manipulation without a manual char loop.
- **Numeric + math helpers** (Phase 73): integer `i64_abs` / `i64_min` /
  `i64_max` / `i64_pow` (prelude) and the f64 math `f64_sqrt` / `f64_floor` /
  `f64_ceil` / `f64_abs` (built-ins lowering to LLVM float intrinsics; the AOT
  link now pulls in `-lm`), plus more Option/Result inspectors
  (`option_is_some`, `option_ok_or`, `result_is_ok`). A real-number program no
  longer needs its own FFI declaration of libm.
- **Capstone** `examples/csvstats` (Phase 74) — "the real stdlib, applied": a
  CSV statistics aggregator that READS data (the thing v11 could not do),
  grouping `category,value` rows and reporting per-category count + sum + the
  running global max in sorted order. Exercises the whole v12 line at once —
  `parse_int` (with an `Option`-driven skip of a malformed row), `str_split`,
  HashMap aggregation, `i64_max`, `sort`, and `int_to_string` + `str_concat`
  formatting.

### Fixed
- A pre-merge adversarial multi-agent review found + fixed two MAJORs the green
  smoke suite had missed — both pinned by `tests/smoke_test_v12_review.sh`:
  - `parse_int` of a value PAST the `i64` range returned a silently-clamped
    `Some(i64::MAX/MIN)` instead of `None` (C `strtoll`'s `ERANGE` was
    unchecked). It now clears `errno` and rejects on `ERANGE`; `i64::MAX` /
    `i64::MIN` themselves still parse. (`parse_f64` keeps `strtod`'s
    overflow-to-`inf` — a valid `f64` parse, like Rust.)
  - a DISCARDED owned temporary leaked: a value moved out by
    `vec_remove(&mut v, 0);` (or any call result like `int_to_string(n);`) used
    as an expression-STATEMENT was never dropped, orphaning its heap. The
    codegen now drops a discarded droppable call-result via an entry-block temp
    — exactly once (the drop / dropleaks / soundness suites confirm no
    double-free).

## [0.11.0] — Roadmap v11 "real machine integers" (Phases 63–68)

Theme: the **numeric tower** — make kardashev practical by giving it real
machine integers (sized + unsigned + f32, `as` casts, bit ops, defined overflow)
instead of i64-only. The first step toward production use. A pre-merge
adversarial multi-agent review hardened a const-evaluation width/sign cluster
(including an invalid-IR blocker) plus two parser/lexer bugs the green suite had
missed (see Fixed).

### Added
- Sized SIGNED machine integers `i8` / `i16` / `i32` (Phase 63) — `i64` stays
  the default. The `Int` type carries a bit width + signedness; codegen lowers
  to the matching LLVM width (`i32 @add(i32, i32)`, not i64). The lattice is
  NON-coercive: no implicit widening (`i32` + `i64` is a type error — `as`
  bridges, Phase 65), and an out-of-range literal for a narrow width is a
  compile error. An unsuffixed literal is i64 by default and narrows to a
  concrete width in context (`let x: i32 = 5`); the type system carries zero
  literal churn (all v10 i64 programs are byte-for-byte unchanged).
- Integer-literal **width suffixes** and **radix prefixes** (Phase 64). A
  suffixed literal `5i32` *is* an `i32` with no annotation (it does not narrow,
  it has that concrete type), so `add(5i32, 3i32)` type-checks against an `i32`
  parameter directly; an out-of-range suffixed literal (`200i8`) is a compile
  error. Hexadecimal `0xFF` and binary `0b1010` literals parse to their value
  (default `i64`), compose with a suffix (`0xFFi32`), and work in `match`
  patterns (`0xFF => …`). Unsigned suffixes (`u8`..`u64`) are parsed and
  rejected with a clear "arrives in a later phase" diagnostic until Phase 66
  lands unsigned integers — never silently mis-typed.
- The **`as` cast operator** (Phase 65) — the only bridge across the
  non-coercive lattice. `operand as Type` converts between any two numeric
  types (an int of any width/signedness, or `f64`): integer widen (`sext`),
  narrow (`trunc`), and `int`↔`f64` (`sitofp` / `fptosi`, truncating toward
  zero), lowered to the width/signedness-correct LLVM cast. A cast is the only
  way to add an `i32` to an `i64` (`a as i64 + b`). `as` binds tighter than
  every binary operator but looser than a prefix unary (`-x as i32` is
  `(-x) as i32`, `a as i32 * 2` is `(a as i32) * 2`) and chains left-to-right
  (`x as i32 as i64`). An `int`→`int` cast is const-foldable and wraps with
  two's-complement semantics (`300 as i8` == 44) identically at compile time
  and run time. Casting from/to a non-numeric type (a struct, `bool`, String,
  reference) is a compile error.
- **Unsigned integers** `u8` / `u16` / `u32` / `u64` and the integer **bitwise
  operators** `& | ^ << >> ~` (Phase 66). Each unsigned type is a distinct
  non-coercive type (`u32` ≠ `i32`; `as` bridges), and codegen lowers its
  division, remainder, ordering comparison, and right-shift to the UNSIGNED
  opcode (`udiv` / `urem` / `icmp u…` / `lshr`) — a signed right-shift stays
  arithmetic (`ashr`). A `u64` literal past `i64::MAX` (e.g. the FNV-1a offset
  basis `0xcbf29ce484222325`) parses, and a wrapping `u64` multiply yields the
  textbook hash. Bitwise operators work on any integer width/signedness, fold
  in const expressions, and are rejected on `f64`. The `&` and `|` tokens are
  position-disambiguated (prefix `&` is still a borrow, a primary `|…|` is
  still a closure; infix they are bitwise-and / bitwise-or), and `<<` / `>>`
  are parsed by token adjacency so nested generics `Vec<Vec<T>>` stay
  unambiguous. Operator precedence now matches Rust: `&&` < comparison < `|` <
  `^` < `&` < shift < `+ -` < `* / %`.
- The **`f32`** single-precision float and **defined overflow semantics**
  (Phase 67). `f32` is a real type lowering to LLVM `float` (`f64` stays the
  default `double`); it is a distinct non-coercive type (`f32` ≠ `f64`), so an
  `as` cast bridges them with `fpext` (`f32`→`f64`) / `fptrunc` (`f64`→`f32`),
  an unsuffixed float literal is `f64` by default and narrows to `f32` in
  context, and `1.5f32` pins the width. Integer overflow is now DEFINED as
  two's-complement **wrapping** at every width (`127i8 + 1 == -128`,
  `255u8 + 1 == 0`), identically at compile and run time. Negative narrow-int
  literals narrow in context — `let x: i8 = -128` (i8::MIN) is valid even
  though `+128` would not fit, while `let x: u8 = -1` is a compile error.
- **Capstone** `examples/checksum` (Phase 68) — "the numeric tower, applied":
  three textbook algorithms written in kardashev, each checked against its
  known answer. **FNV-1a** (64-bit) uses a `u64` offset basis past `i64::MAX`
  (`0xcbf29ce484222325`) and a wrapping `u64` multiply; **CRC-32** (IEEE) uses
  a `u32` with a logical `>>`, the bitwise ops, and a branchless mask built by
  wrapping subtraction (`0 - (crc & 1)`); a **binary parser** assembles `u16`
  / `u32` from raw `u8` bytes with shifts and casts in both byte orders. Each
  routine is generic over its input length with a const-generic `[u8; N]`,
  integrating the v10 const-generic line with the whole v11 numeric tower —
  none of it is expressible in an i64-only language.

### Fixed
- A pre-merge adversarial multi-agent review hardened a cluster the green smoke
  suite had missed — every one with a verified repro, now pinned by
  `tests/smoke_test_v11_review.sh`:
  - **(blocker)** a narrow / unsigned `const` flowed into a narrow slot as a
    64-bit immediate — invalid LLVM IR (`call i32 @id(i64 7)`) / verifier
    crash. Codegen now emits a folded const at the const-reference's resolved
    int width.
  - a sized / unsigned `const`'s folded value disagreed with the same
    expression at run time — an unsigned `>>` folded as an arithmetic shift, a
    narrow result was not wrapped to its width (`100i8 + 100i8` → 200 at const
    time vs −56 at run time), and `1i32 << 31` silently held 2147483648 in an
    `i32`. The const evaluator now wraps every result to its expression-type
    width (two's-complement), so an unsigned `>>` is logical and every sized
    const folds identically to run time.
  - a plain-literal narrow / unsigned `const` (`const C: i32 = 100`) was
    rejected though the identical `let` was accepted — `const` now narrows its
    initializer like any other coercion site.
  - `expr as Type << ..` / `expr as Type < ..` was a parse error — the cast's
    target type greedily consumed the `<` / `<<` as a generic-argument list.
    A cast now parses only a bare (numeric) target, leaving the operator for
    the expression parser.
  - an integer/float width suffix was absorbed in tuple-index position
    (`t.0i32` silently became `t.0`) — the suffix is no longer taken after a
    `.`.

## [0.10.0] — Roadmap v10 "sized and sound at compile time" (Phases 57–62)

Theme: **sized and sound at compile time** — const-generic type params + the
effect system's last soundness floor. A pre-merge adversarial multi-agent review
hardened 5 blockers + 5 majors the green smoke suite had missed (see Fixed).

### Added
- Const-generic parameters parse and bind: `const N: i64` (mixed with type
  params), a symbolic `[i64; N]` array length, and the `let (a, b): (T, T) = ..`
  tuple-pattern annotation (Phase 57 — declaration shell only).
- Monomorphization over a const VALUE (Phase 58): `Mat<3>` and `Mat<5>` become
  DISTINCT LLVM struct types (`{ [3 x i64] }` vs `{ [5 x i64] }`, mangled
  `Mat__c3` / `Mat__c5`), incl. nested `Matrix<R, C>` over `[[i64; C]; R]`. The
  const value substitutes the symbolic array length; a struct literal infers
  each `const N` from the dimensions of the field that carries it
  (`Mat { data: [1,2,3] }` is a `Mat<3>`). Type/const argument slot mismatches,
  a const-value dimension mismatch, and negative const args are compile errors.
- Const-generic FUNCTIONS + compile-time dimension unification (Phase 59):
  `fn dot<const N>(a: [i64; N], b: [i64; N]) -> i64` infers N from the argument
  array lengths, lets `N` be used as a value in the body, and monomorphizes per
  size (`@dot__c3` over `[3 x i64]` vs `@dot__c2`). A dimension MISMATCH
  (`dot([i64;3], [i64;2])`) and a const param that appears in no argument array
  type are compile errors.

- `RingBuffer<T, const CAP>` (Phase 61): a struct generic over BOTH a type and
  a const param, with element-wise Drop and deep clone over a NON-Copy element.
  Fixed-size arrays `[T; N]` now allow non-Copy elements (String/struct/Vec/Box)
  — clone element-wise, drop element-wise; moving a non-Copy element out by
  index (`let x = a[i]`) is a compile error (clone or borrow instead). Symbolic
  const params flow through generic impls (`impl<T, const CAP> Clone for
  RingBuffer<T, CAP>`) and `derive(Clone)`. Plus closure-param INFERENCE:
  `vec_map(v, |x| *x * 2)` infers `x`'s type from the callee's fn-typed
  parameter — no `|x: &i64|` annotation needed.
- Array-repeat `[value; N]` (Phase 62) — `N` a literal, const item, or a
  const-generic param (a symbolic length).
- **Capstone** `examples/matrix` (Phase 62) — a fixed-size linear-algebra
  library: `Matrix<const R, const C>` carries its shape in the TYPE,
  `transpose() -> Matrix<C, R>` swaps the dims, and a dimension-checked
  `matmul(Matrix<R, K>, Matrix<K, C>) -> Matrix<R, C>` rejects a shape mismatch
  at COMPILE time (the shared inner dim `K` can't be two values). Integrates the
  whole v10 line: monomorphize-over-a-value, dimension unification, symbolic
  const params, non-Copy arrays, and array-repeat.

### Fixed
- A pre-merge adversarial multi-agent review (6 dimensions) hardened **5
  blockers + 5 majors** the green smoke suite had missed — every one with a
  verified repro — now pinned by `tests/smoke_test_v10_review.sh`:
  - a const param not threaded into a NESTED struct/enum field's type-args
    (`Inner<N>` field of `Outer<N>` mangled `Inner__c0` → LLVM-verifier failure);
  - a bare `b.clone()` on a const-generic struct leaving the const arg symbolic
    (mangled `c0`) → result type confusion;
  - `Drop` is no longer EXEMPT from the effect-subset rule — a `dyn Drop`
    dispatch could launder io/alloc through a pure-declared `Drop` trait;
  - a BOUNDED-generic method call (`<T: Trait>` + `t.method()`) attributed ZERO
    effects (vs the trait's declared effects) — the subset rule's actual floor;
  - forwarding a SYMBOLIC array length alongside a concrete one was accepted
    ill-typed (LLVM miscompile) and legitimate symbolic forwarding was wrongly
    rejected;
  - const-generic ENUM variant payloads (`[i64; N]`) were wrongly rejected;
  - a monomorphization name colliding with a user identifier (`g__i64`) silently
    resolved to the user fn — now a clear compile error;
  - assigning to a non-Copy array element `a[i] = x` was wrongly rejected;
  - array-repeat `[v; N]` ignored a local shadowing a const param;
  - a method-level const param leaked an internal mangled name — now a clear
    "declare it on the impl block" diagnostic.

### Changed
- The **effect-subset rule** (Phase 60), the effect system's last soundness
  floor: a trait impl method's effects must be a SUBSET of the trait method's
  declared effects, so a `dyn Trait` / `<T: Trait>` dispatch (which attributes
  the TRAIT's effects) can never under-count what an impl actually does. A
  super-effecting impl is a compile error. `Drop` is exempt (static drop glue,
  never dyn-dispatched). To make the prelude honest, `Eq::eq`,
  `Iterator::next`, `Display::to_string` and `Default::default` now declare
  `! { alloc }` (their container/heap impls allocate); a concrete `for` loop
  still attributes its concrete `next`'s effects, so pure (Range) loops stay
  pure, and `derive(Eq)` annotates `! { alloc }` only when a field's `eq`
  actually allocates (a map-/Vec-/generic-free struct's derived `eq` is pure).

## [0.9.0] — Roadmap v9 "data in motion" (Phases 51–56)

### Added
- `Box<T>` as a first-class impl target + `&*`/`**` deref ergonomics, and
  prelude `Clone`/`Eq` for `Box<T>`.
- Generic associated functions: a bounded `T::method()` (e.g. `T::default()`).
- `Vec` higher-order combinators `vec_map` / `vec_filter` / `vec_fold` over
  closures (effect-polymorphic).
- String tokenizing (`str_split`, `str_trim`) and `hashmap_entries → Vec<(K,V)>`.
- Capstone `examples/wordfreq` — a word-frequency histogram pipeline.

### Fixed
- A pre-merge adversarial review hardened 5 memory-safety / type-soundness
  holes the green smoke suite had missed (by-value container-getter double-free,
  `dyn Trait<T>` argument confusion, move-out-of-`&` via `*r`, `&mut` reborrow
  aliasing, an unjoined `if`-branch move-state) plus dyn/generic effect
  attribution — locked in by `tests/smoke_test_soundness.sh`.

## [0.8.0] — Roadmap v8 "generics, finished" (Phases 45–50)

### Added
- Bounded type params (`K: Hash + Eq`) inside container ops; prelude `Clone`/`Eq`
  trait impls for `HashMap`.
- `Ord` trait + a generic in-place `sort<T: Ord>` (+ `vec_swap`, `&mut → &`
  reborrow).
- `#[derive(Hash, Ord, Default)]` and associated functions (static
  `Type::method()`).
- `dyn Trait<T>` generic trait objects + dispatch through `Vec<Box<dyn …>>`.
- Capstone `examples/json` upgraded to JSON 3.0 — `HashMap<String, Json>`
  objects, fully `#[derive]`d, canonical sorted-key output.

## [0.7.0] — Roadmap v7 "real numbers, real abstraction" (Phases 39–44)

### Added
- `f64` floating point.
- Generic `impl<T: Bound>` blocks; generic `Clone`/`Eq` over containers;
  `#[derive(Clone, Eq, Display)]`.
- Runtime string escapes; the last async-frame leak closed.
- Capstone JSON 2.0 — floats + decoded escapes + derived `Clone`/`Eq`.

## [0.6.0] — Roadmap v6 "make the heap recursive" (Phases 33–38)

### Added
- Sound recursive heap-owning enums (`Box`/`Vec<Self>`/`HashMap<K,Self>`) with
  recursive `Drop` + deep `clone`; read-without-move + `match`-by-reference;
  enum-typed struct fields + non-Copy tuples; `Display` + de-`i64`'d iteration.
- Capstone: a full nested-JSON parser + serializer written in kardashev.

### Fixed
- An `-O1+` miscompile: the optimizer ran without the target datalayout, folding
  multi-field-aggregate reads-through-a-pointer to wrong byte offsets.

## [0.5.0] — Roadmap v5 (Phases 27–32)

### Added
- Stdlib depth (string toolkit, generic `HashMap<K,V>`), file I/O + CLI args,
  `Drop`-leak fixes, and self-written capstones (`examples/calc`,
  `examples/rpn`). Docs + a source-comment truth pass.

## [0.4.0] — Roadmap v4 (Phases 21–26)

### Added
- Generic trait parameters + associated types + `where` clauses; fixed-size
  arrays `[T; N]` + tuples `(A, B)`; compile-time `const` items + const
  evaluation (incl. const-generic array lengths); `extern "C"` FFI; an
  arithmetic-interpreter capstone written in kardashev.

## [0.3.0] — Roadmap v3 (Phases 15–20)

### Added
- Expression & item completeness (bool/unary ops, inherent impls); deterministic
  memory management — `Drop`/RAII with runtime drop flags; real panic + unwinding
  with cleanup; OS threads + `Mutex`; opt-level flags + the `kardc --test` runner.

## [0.2.0] — Roadmap v2 (Phases 9–14)

### Added
- Iteration (loops, ranges, `for`); closures + effect-carrying function types
  (first-class fn values, `FnMut` captures); `dyn Trait` dynamic dispatch; a
  growable stdlib (`String`, `HashMap`, `&[T]` slices, `map`/`filter`/`fold`
  combinators, `Option`/`Result` combinators); the source formatter (`kardfmt`)
  and richer LSP.

## [0.1.0] — Roadmap v1 (Phases 0–8)

### Added
- The MVP and foundation: the full pipeline (lexer → parser → Hindley-Milner
  type inference → LLVM IR → ORC JIT + AOT); ownership + non-lexical-lifetime
  borrow checking; ADTs + pattern matching; traits + generics + monomorphization;
  `Result` + the `?` operator; **effect labels** in signatures (the signature
  feature) with effect-row polymorphism; a minimal stdlib (`Option`/`Result`/
  `Vec`/`String`) + AOT pipeline; `async`/`await` + a single-thread executor;
  the module system + `kard` CLI + `rules_kardashev` Bazel rules; `-O0..-O3`
  pass pipelines + the `kard-lsp` language server.
