# kardashev

A systems programming language with lightweight effect-label typing, built on LLVM.

**[📖 Documentation](https://kardashevlang.github.io/kardashev/)** · [Language Reference](https://kardashevlang.github.io/kardashev/language-reference.html) · [Effects](https://kardashevlang.github.io/kardashev/effects.html) · [Stdlib](https://kardashevlang.github.io/kardashev/stdlib.html) · [Architecture](https://kardashevlang.github.io/kardashev/architecture.html) — Licensed [MIT](LICENSE-MIT) OR [Apache-2.0](LICENSE-APACHE)

## What it is

kardashev is a Rust-flavored systems language whose signature feature is **lightweight effect labels in the type system**: every function declares which side-effects it can produce (`io`, `alloc`, `panic`, `async`, ...) as part of its signature, and the compiler tracks them across call chains. Unlike Koka, there are no handlers or continuations — effects are pure type-system information, with zero runtime cost.

```rust
fn add(a: i64, b: i64) -> i64 { a + b }                       // pure

fn read_cfg(path: &str) -> Result<Config> ! { io, alloc } {   // effects in signature
    let s = std::fs::read_to_string(path)?;
    parse(s)
}

fn map<T, U, e>(xs: Vec<T>, f: fn(T) -> U ! {e}) -> Vec<U> ! { e, alloc } {
    let mut out = Vec::with_capacity(xs.len());
    for x in xs { out.push(f(x)); }
    out
}
```

The `! { ... }` syntax after the return type is the effect row. `e` is a row variable making the function effect-polymorphic — `map` is pure when `f` is pure, and propagates whatever effects `f` introduces.

## Design

- **Memory model**: ownership + borrowing (Rust-style affine, non-lexical lifetimes)
- **Type system**: HM-based with generics, ADTs, traits + `impl`, monomorphization
- **Errors**: `Result<T, E>` + `?` operator
- **Concurrency**: `async` / `await` lightweight tasks (stackless state-machine transform)
- **Surface syntax**: Rust/Go-style — `{}`, `fn`, `->`, `match`, `let`
- **Effect labels**: row-polymorphic effect sets, no handlers, compile-time only
- **Backend**: LLVM (AOT to native binary + ORC JIT for the REPL)
- **Build system**: Bazel + `rules_kardashev` (Starlark) + thin `kard` CLI wrapper
- **Source extension**: `.kd`

### Built-in effect labels (v1)

| Label    | Meaning                                                            |
|----------|--------------------------------------------------------------------|
| `pure`   | No effects (empty row; the default if `! { ... }` is omitted)      |
| `alloc`  | Heap allocation                                                    |
| `io`     | File / network / stdio / general syscalls                          |
| `panic`  | Unrecoverable failure                                              |
| `async`  | Yields to the scheduler                                            |
| `unwind` | Stack unwinding for cancellation (distinct from `panic`)           |

Effect sets are unioned across the call graph and checked at definition sites; no runtime cost.

## Status

All sixteen roadmaps (Phases 0–97, **v1–v16**) have shipped and are merged to
`main` — 6 unit suites plus the full smoke-test aggregate pass **JIT and AOT**
on a cleared clean build. v15–v16 ("self-hosting") build a self-hosted compiler
front *in* kardashev — the north-star arc toward a bootstrap: v15 the front-end
(lexer + parser + signature checker), v16 the BODY (expression/statement parser,
scope checker, and a function-body interpreter; `examples/selfhost/`, capstone
`interp.kd`). v14 ("hardening") made the toolchain trustworthy
across platforms: **macOS CI went green for the first time** (portable leak
gates), the smoke harness is SIGPIPE-robust, the channel capture-and-keep footgun
is now a precise compile error, and a JIT-vs-AOT differential sweep over the 9
capstones confirms both backends agree (Linux + macOS-arm64). v13
("concurrency") made thread-safety a *checked
property*: the **`share` effect** (a pure-declared trait can't launder a
`thread_spawn`), **typed MPSC channels** that MOVE a real `T` across threads
guarded by the structural **`Send`** rule, and `Rc<T>` as the legible non-`Send`
witness — capped by a parallel map-reduce capstone (`examples/parstats`). A
pre-merge adversarial multi-agent review (3 reviewers, ~600 stress runs) found
a use-after-free in the borrow-returning builtins (`rc_get`/`vec_get_ref`) and
two channel-lifecycle defects (an unbounded block/node leak and multi-producer
`chan_close` data loss); all three are fixed by refcounted, move-only channel
endpoints + borrow-provenance tracking, pinned by
`tests/smoke_test_v13_review.sh`. The Send/`share` soundness surface came back
clean. v12 ("real stdlib") made kardashev able to *get data
in and out*: string→number parsing (`parse_int` / `parse_f64` → `Option`), Vec
mutation + query (`vec_pop` / `vec_remove` / `vec_insert` / `vec_reverse` /
`vec_contains`), HashMap/HashSet enumeration + membership, string methods
(`str_contains` / `str_index_of` / `str_to_upper` / `str_concat` / …), and
integer + f64 math helpers — capped by a CSV-statistics capstone
(`examples/csvstats`). A pre-merge adversarial multi-agent review fixed a
`parse_int` integer-overflow (silently clamped instead of `None`) and a
discarded-owned-temporary leak (pinned by `tests/smoke_test_v12_review.sh`).
v11 ("real machine integers" — the **numeric tower**) made kardashev practical:
sized signed/unsigned integers `i8`…`i64` / `u8`…`u64` and `f32`/`f64` over a
NON-coercive lattice, the `as` cast operator, literal width suffixes +
hex/binary radix, the bitwise operators `& | ^ << >> ~`, defined two's-
complement wrapping overflow, and a checksum/binary-parser capstone
(`examples/checksum` — FNV-1a, CRC-32, an endian byte parser); its review
hardened a const-evaluation width/sign cluster (incl. an invalid-IR blocker) +
two parser/lexer bugs (pinned by `tests/smoke_test_v11_review.sh`). v10
("sized and sound at compile time") added const-generic type params, a
dimension-checked linear-algebra capstone, and the effect-subset soundness
floor; its review hardened **5 blockers + 5 majors** (pinned by
`tests/smoke_test_v10_review.sh`). Built locally with `bazel build //...
&& bazel test //...` or, when Bazel isn't available, the `Makefile.local` shim
(LLVM + clang). The CI matrix runs both ubuntu-latest and macos-latest via Bazel
on every push; every commit goes in green.

📖 **Documentation: <https://kardashevlang.github.io/kardashev/>** — the
language reference, effects-system notes, stdlib catalog, and
compiler-architecture deep dive (built from [`docs/`](docs/) with mdBook).
[`examples/hello/`](examples/hello/) shows a two-file program built
through the Bazel rules.

What works today:

```rust
// Generics + traits + bounded params (Phase 3)
trait Show { fn show(self) -> i64; }
struct Point { x: i64, y: i64 }
impl Show for Point { fn show(self) -> i64 { self.x + self.y } }
fn use_show<T: Show>(t: T) -> i64 { t.show() }

// Result + ? operator (Phase 3.4)
enum Result<T, E> { Ok(T), Err(E) }
fn double(n: i64) -> Result<i64, i64> {
    let x = parse(n)?;        // early-returns Err if parse fails
    Ok(x + x)
}

// References + NLL borrow check (Phase 2.4)
fn read(p: &Point) -> i64 { p.x + p.y }
fn main() -> i64 {
    let p = Point { x: 3, y: 4 };
    let r = &p;
    let a = read(r);          // r's last use here; borrow is now dead
    let b = consume(p);       // OK to move — NLL allows it
    a + b
}

// Effect labels (Phase 4) — pure by default; explicit effects propagate
fn raw_read() -> i64 ! { io } { 42 }
fn main() -> i64 ! { io, alloc } { raw_read() }    // pure-caller would error
```

`Option<T>` and `Result<T, E>` are auto-included via a built-in prelude
so user programs can use `Some` / `None` / `Ok` / `Err` without
redeclaring them. A growable `Vec` (heap-allocated `i64` buffer) and
immutable `String` ship too, with `vec_new` / `vec_push` / `vec_get` /
`vec_len` and `print_str` / `str_len`. A built-in `print(n: i64) -> i64
! { io }` writes one integer plus newline to stdout. Callers must
declare every effect they use, same rule as any other effect:

```rust
fn main() -> i64 ! { io, alloc } {
    let s = "hello, kardashev";
    print_str(&s);              // -> "hello, kardashev"
    print(42);                  // -> "42"
    let v = vec_new();
    vec_push(&mut v, 10);
    print(vec_len(&v));         // -> "1"
    0
}
```

Async fns return the built-in `Future` opaque type; `.await` unwraps:

```rust
async fn add(a: i64, b: i64) -> i64 { a + b }
async fn double(n: i64) -> i64 { add(n, n).await }
fn main() -> i64 ! { async, io } {
    print(double(21).await);    // -> "42"
    0
}
```

Multi-file programs: write `mod foo;` at the top of a `.kd` file to
pull in `foo.kd` from the same directory. `pub fn` gates path-qualified
references across module boundaries; bare-name references still resolve
via the Phase 7.1 flat-merge that runs alongside.

```
// util.kd
pub fn double(n: i64) -> i64 { n + n }
// main.kd
mod util;
fn main() -> i64 { util::double(21) }      // -> 42
```

Since v5 the language has grown a full data layer and abstraction layer
(roadmaps v6–v9, detailed below):

- **`f64` floats** + the recursive heap (`Box<T>`, `Vec<Self>`, `HashMap<K,V>` of
  self) — arbitrarily nested values that drop and deep-clone soundly.
- **Traits + `#[derive]`**: `Clone`, `Eq`, `Ord`, `Hash`, `Display`, `Default`
  derive automatically (over structs, recursive enums, generics, and `HashMap`/
  `Box` fields); generic `impl<T: Bound>` blocks, prelude trait impls for the
  containers, and **generic trait objects** `dyn Trait<T>` (heterogeneous
  `Vec<Box<dyn …>>` with dynamic dispatch).
- **Generic associated functions** — `T::default()` for a bounded `T`.
- **`Ord` + a generic in-place `sort<T: Ord>`**, and `Vec` higher-order
  combinators `vec_map` / `vec_filter` / `vec_fold` over closures
  (effect-polymorphic in the closure's effects).
- **Strings & maps**: `str_split` / `str_trim` tokenizing; `hashmap_entries ->
  Vec<(K, V)>` for ranking/printing a map.

Two capstones written entirely in kardashev exercise it end to end:
[`examples/json/`](examples/json/) — a full nested-JSON parser+serializer with
`JObj(HashMap<String, Json>)`, `#[derive(Clone, Eq)]`, and canonical
sorted-key output, round-tripping via the derived `Eq`; and
[`examples/wordfreq/`](examples/wordfreq/) — a word-frequency histogram
(tokenize → count in a `HashMap` → rank by a derived `Ord` → top-N). Both run
leak-free under a constant-memory gate, JIT and AOT.

> Before v6–v9 landed on `main`, an adversarial multi-agent review hardened five
> memory-safety / type-soundness holes the green smoke suite had missed (a
> by-value container-getter double-free, a `dyn Trait<T>` argument-confusion, a
> move-out-of-`&` via `*r`, a `&mut` reborrow aliasing `f(r, r)`, and an
> unjoined `if`-branch move-state) plus dynamic-dispatch effect attribution —
> all locked in by `tests/smoke_test_soundness.sh`.

Four driver entry points:

```
kardc                            # interactive REPL (JIT each expression)
kardc <file.kd>                  # JIT-run main() and print result
kardc -o <out> <file.kd>         # AOT-compile to a native executable
kardc --test <file.kd>           # run every `test_*() -> i64` fn (0 = pass)
kardc -O0|-O1|-O2|-O3 ...         # optimization level (default -O2)
kard-lsp                         # Language Server Protocol over stdio
                                 #   (diagnostics, hover, completion,
                                 #    go-to-definition, find-references, rename)
kard build | kard run            # build/run a kard.toml project (no file arg)
```

The optimization flag selects the post-codegen LLVM pass pipeline: `-O0`
runs LLVM's minimal O0 pipeline (no inlining; alloca-heavy IR kept), while
`-O1/-O2/-O3` run the matching `buildPerModuleDefaultPipeline`. The default
is `-O2` — byte-for-byte the historic behavior. The level is folded into the
AOT compile-cache key, so `-O0` and `-O2` objects never collide.

`kardc --test` discovers test functions by convention — `fn test_*() -> i64`
(no params; `return 0` = pass, nonzero = fail) — compiles the file once,
JIT-runs each test, prints a `running N tests … result: X passed, Y failed`
summary, and exits nonzero if any test failed. A test file need not define
`main()`.

Plus the thin `kard` shell wrapper (`kard build`, `kard run`, `kard test`,
`kard repl`) and Bazel rules (`kardashev_library`, `kardashev_binary`) for
projects that want to compose kardashev targets into a larger Bazel
monorepo. A worked capstone — a reverse-Polish-notation calculator
exercising `Vec`/`HashMap`/structs/trait dispatch/`match`/`Option`/`Result`
— lives in `examples/rpn/`.

### Package manifest (`kard.toml`) + local-path dependencies

A project can carry a `kard.toml` at its root. `kard build` / `kard run`
**with no file argument** read it, compile the declared entry point, and make
each local-path dependency resolvable:

```toml
[package]
name = "app"
version = "0.1.0"
entry = "src/main.kd"            # optional; defaults to src/main.kd

[dependencies]
mathlib = { path = "../mathlib" }   # inline-table form
strutil = "../strutil"               # bare-string form (path)
```

```
// src/main.kd
mod mathlib;
fn main() -> i64 { square(5) + cube(3) }   // square/cube come from mathlib
```

A local dependency's library `.kd` is located under its path (`<dep>.kd`,
`src/lib.kd`, `lib.kd`, or `src/<dep>.kd`, or the path itself if it's a `.kd`
file) and staged as a `mod`-resolvable sibling of the entry, so kardashev's
existing intra-project flat-merge picks it up — then unstaged afterward (the
source tree is left untouched). The manifest parser is a tiny hand-written
reader in the `kard` wrapper (no TOML library dependency). A missing/malformed
manifest, a missing entry, or an unresolvable dependency each fail with a
clear message.

**Deferred (documented, not stubbed):** resolving *third-party* dependencies
via the Bazel module registry. Bazel can't run in this build environment, so a
real registry integration isn't verifiable here and is intentionally left as
future work — the `kard.toml` manifest + local-path resolution above is what's
implemented and tested (`tests/smoke_test_manifest.sh`).

The AOT path emits a native object via LLVM's `TargetMachine`,
synthesizes a C-compatible `int main()` wrapper that returns the
kardashev `fn main() -> i64` result truncated to an exit code, and
shells out to `clang` for linking. Programs compile through lexer →
parser → HM typechecker → NLL borrow-checker → effect inference →
LLVM IR → LLVM O2 pipeline → ORC v2 JIT (or AOT).

## Roadmap v1 — shipped

Phases 0–8 are implemented and green on CI (ubuntu + macOS). This is the
self-hosting-compiler MVP: the language compiles, type-checks, borrow-checks,
JITs, and AOT-links real programs.

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Scaffold: Bazel + LLVM toolchain + CI + a JIT binary returning `42` | ✅ |
| 1 | MVP: JIT REPL running `fib` (lexer + parser + monotype HM + LLVM IR + ORC JIT) | ✅ |
| 2 | Ownership + NLL borrow check + structs + enums + pattern matching | ✅ |
| 3 | Traits + generics + `Result` + `?` operator + monomorphization | ✅ |
| 4 | Effect labels in signatures (the signature feature lands here) | ✅ concrete labels (`io`, `alloc`, `panic`, `async`, `unwind`) + propagation + first-class fn-pointer values flow through `let`-bindings and conditionals, dispatched via LLVM indirect calls. |
| 5 | AOT pipeline + minimal stdlib (`Option`, `Result`, `Vec`, `String`) | ✅ AOT + Option/Result via prelude + heap-backed generic `Vec<T>` (per-T specialization with `DataLayout`-sized stride; works for `i64`, `bool`, structs, enums) + immutable `String` (literal-backed). |
| 6 | `async` / `await` + state-machine transform + basic executor | ✅ `async fn` returns the built-in `Future`. Codegen splits each async fn into `__async_body_<n>` + a Future-wrapping shim; `.await` lowers to a poll loop that branches on `READY`, ready to plug a real scheduler under the existing `pending` block once blocking primitives exist. |
| 7 | Module system + complete `rules_kardashev` + `kard` CLI | ✅ `mod foo;` resolves siblings recursively; `pub` enforced on path-qualified references; `foo::bar` path syntax parses; `kard` shell wrapper + Bazel `kardashev_library` / `kardashev_binary` rules ship. |
| 8 | Optimization passes + LSP + docs site | ✅ LLVM O2 PassBuilder pipeline runs on every emitted module; `kard-lsp` speaks the LSP protocol over stdio and publishes diagnostics; `docs/` carries the language reference, effects system notes, stdlib catalog, and compiler-architecture deep dive. |

## Roadmap v2 — shipped

v1 proved the pipeline end to end but leaned on recursion + `match` for control
flow, top-level `fn`s for higher-order code, and static dispatch everywhere. v2
closed those gaps and is now implemented and green on the full test suite (6
unit suites + 18 smoke tests, JIT + AOT). The **north star** held: the
effect-polymorphic higher-order pattern — closures + effect-carrying function
types + iteration — now composes end to end, which is exactly what the
language's thesis ("effects are part of the type") demands.

| Phase | Goal | Status |
|-------|------|--------|
| 9 | **Iteration**: `while`, `loop`/`break`/`continue`, `for x in it`, and an `Iterator` trait | ✅ `while` / `loop` with `break`/`break v`/`continue`; `let mut` + assignment; `for x in a..b` / `a..=b` over a first-class `Range`; a prelude `Iterator` trait (`fn next(&mut self) -> Option<i64>`) with `for` desugaring through `next()` for any impl (Phase 13a). |
| 10 | **Closures + effect-carrying function types** | ✅ The `Function` type carries an effect row (`effectLabels` + an HM row variable); `fn(T) -> U ! {e}` row-polymorphism is real — a pure caller invoking `apply(ioFn)` is rejected. Capturing closures `\|x\| x + n` lower to a heap env-struct + a uniform fat-pointer fn-value that subsumes the Phase 4.3 path; a closure's effect row is inferred from its body. **The capstone of the signature feature.** |
| 11 | **Trait objects + dynamic dispatch**: `dyn Trait`, vtables, `Box<dyn Trait>` | ✅ Trait objects are `{data, vtable}` fat pointers behind `&dyn Trait` / `Box<dyn Trait>`; per-impl vtable globals + thunks; one call site dispatches to multiple runtime impls; object-safety enforced. Static dispatch is an unchanged separate path. |
| 12 | **Real async runtime**: suspending primitive + single-threaded executor + multi-state state-machine transform | ✅ `Future = {poll, frame}`; each `async fn` lowers to a resumable poll fn over a heap frame that `switch`es on a resume state and spills locals live across awaits; `.await` genuinely suspends (returns `Pending`) and resumes. `yield_now` suspends once; `block_on` drives to completion. Verified: the `pending` path is taken (poll count > awaits) and pre-await effects fire once. |
| 13 | **Growable stdlib**: mutable `String`, `HashMap<K,V>`, `&[T]` slices, iterator adaptors (`map`/`filter`/`fold`), `Option`/`Result` combinators | ✅ Method receivers autoref-borrow (`&self`/`&mut self`) so stateful methods work; heap-backed growable `String` (`push_str`), `HashMap<i64,i64>` (linear-probing + rehash) returning `Option`, `&v[a..b]` slices; `fold`/`map`/`filter` as generic closure-driven library fns; prelude `Option`/`Result` combinators — all effect-row aware. |
| 14 | **Tooling + ecosystem**: `kard fmt`, richer LSP (completion/hover/go-to-def), incremental build cache, dependency resolution via the Bazel module registry, DWARF debug info | ✅ (14a) `kardfmt` idempotent source formatter + DWARF debug info behind `-g`. ✅ (14b) `kard-lsp` serves hover (signature **with the effect row**), scope-aware completion, and go-to-definition on top of diagnostics; `kardc -o` has a content-addressed incremental compile cache (`${XDG_CACHE_HOME:-~/.cache}/kardashev`, keyed on resolved source + flags + format version; `--no-cache` to bypass). **Deferred (1 item):** cross-project dependency resolution via the Bazel module registry — intra-project deps work via `mod foo;` + the `kardashev_library`/`kardashev_binary` rules, but a third-party package registry isn't implemented (it can't be verified in this build environment, so it was intentionally not stubbed). |

## Roadmap v3 — shipped

v2 made the language expressive; v3 makes it a language you can run in production,
and is now implemented and green on the full suite (6 unit suites + 30 smoke tests,
JIT + AOT, ubuntu + macOS). The **north star held**: a program that allocates in a
loop now runs in *constant memory* (Drop frees deterministically), and you write
idiomatic code (`if flag`, `else if`, `!done`) without contortions. One item stays
deferred (cross-project deps via the Bazel module registry — unverifiable in this
build environment); everything else is real and tested.

| Phase | Goal | Status |
|-------|------|--------|
| 15 | **Expression & item completeness**: boolean literals `true`/`false`, unary `-x` (neg) and `!x` (logical not), `else if` chains, inherent impls `impl Type { … }`, `pub` on all item kinds | ✅ `true`/`false` → `i1`; unary `-x`/`!x` with a precedence layer tighter than binary ops; `else if` ladders; inherent `impl Type { … }` resolving into the same method table as trait impls (incl. `&mut self`); `pub` parsed/stored on all item kinds (enforcement stays fn-only). Also fixed `fn main() -> bool` JIT/AOT return-width handling. |
| 16 | **Deterministic memory management (Drop / RAII)**: a `Drop` trait + compiler-inserted drops at scope exit, driven by the existing NLL ownership/move analysis | ✅ `trait Drop`; Vec/String/HashMap/Box get built-in free glue; drops are inserted at scope exit in reverse order via **runtime drop flags** (so conditional moves drop exactly once — never double-free/UAF); moved/returned values aren't dropped. Verified: reverse drop order, move semantics, and a 2 M-iteration alloc loop running in **constant ~1.5 MB RSS**. (Closure-env + async-frame contents aren't dropped yet — documented leak, no UAF.) |
| 17 | **Fully generic stdlib + closures**: de-`i64`-ify `Iterator`/`Future<T>`/`HashMap<K,V>`/adaptors; `FnMut` + capture-by-reference; calling a field-held fn value | ✅ Generic `Future<T>` (async returns `bool`/structs; `block_on<T>` specialized per-T) and `HashMap<i64, V>` (generic value via `DataLayout` stride); `FnMut`/capture-by-reference closures (env stores a pointer to the captured slot; by-ref closures can't escape); calling a field-held fn value `(s.f)(x)`. **Deferred:** generic `Iterator<T>` element type (still `i64`) — needs generic-trait/associated-type support; and `HashMap` non-`i64` keys (needs a `Hash` trait). |
| 18 | **Real async I/O + multitask executor**: an epoll/kqueue reactor, timer + I/O-readiness leaf futures, `spawn` + `join` | ✅ A process-global executor holds a growable queue of type-erased `Future` tasks and round-robins them: `spawn(f)` enqueues a task (returns an i64 handle), `block_on(f)`/`join(h)` drive the whole queue (so spawned tasks interleave) until the target completes. `sleep_ms(n)` is a real `CLOCK_MONOTONIC` timer leaf; when every task is Pending the reactor **sleeps** (`nanosleep`, or `epoll_wait` on Linux) until the nearest deadline instead of hot-spinning (3×100 ms sleeps finish in ~100 ms at ~0% CPU). Linux/epoll fd-readiness landed (`pipe_make`/`pipe_send`/`read_pipe` — an async reader suspends in `epoll_wait` and wakes on a write); macOS/kqueue fd-readiness is deferred (timers work there via `nanosleep`). |
| 19 | **Threads + compile-time data-race freedom**: OS threads, `Send`/`Sync` marker traits enforced by the type system, channels, `Mutex`/atomics | ✅ Real `pthread`-backed `thread_spawn`/`thread_join` running a kardashev fn value on a new thread; `Mutex` (pthread_mutex-backed) — verified 2 threads × 100 000 shared increments == exactly 200 000, deterministic. **Data-race freedom (enforced):** `thread_spawn` rejects at compile time any closure that captures a binding **by reference** (it would alias the spawning frame's stack across threads) — captures must be by value (`Send` by move) or shared via a `Mutex` handle. **Deferred:** full `Send`/`Sync` marker traits + a `Sync` rule for shared `&T`, channels, and atomics (the by-ref rejection is the enforced floor). |
| 20 | **Production toolchain & ecosystem**: cross-project dependency resolution via the Bazel module registry (the v2 deferral), a package manifest, `kard test`, `-O0/-O2/-O3` opt-level flags, LSP rename/find-references — capped by building a real non-trivial program end to end | ✅ (20a) `-O0..-O3` opt-level flags (folded into the AOT cache key), the `kardc --test` runner (`fn test_*() -> i64`, pass/fail counts + exit code), and the RPN-calculator capstone (`examples/rpn/`). ✅ (20b) `kard-lsp` adds **find-all-references** (resolves the symbol under the cursor to its definition site over the Phase 14b occurrence index, then returns every occurrence that resolves to it; honors `includeDeclaration`) and **rename** (a single-document `WorkspaceEdit` rewriting every occurrence + the declaration); a `kard.toml` **package manifest** + **local-path dependency** resolution drive `kard build`/`kard run` with no file argument. **Deferred (1 item, unchanged from v2):** third-party dependency resolution via the Bazel module registry — Bazel can't run in this build environment, so it isn't verifiable here and was intentionally not stubbed; intra-project + local-path deps (`mod foo;`, `kardashev_library`/`kardashev_binary`, and now `kard.toml` `[dependencies]`) are what ship. |

Landed in order **15 → 16 → 17 → 18 → 19 → 20** (17's generic `Future<T>`
unblocked 18; 16's Drop underpins 19's safe sharing), each verified — clean build
+ direct behavior checks — and CI-green on ubuntu + macOS before the next built on it.

## Roadmap v4 — shipped

v3 made kardashev a real systems language you can *run*; v4 makes it one you'd
reach for, and is now implemented and green on the full suite (6 unit suites + 35
smoke tests, JIT + AOT, ubuntu + macOS). The stdlib is no longer `i64`-bound, the
`panic`/`unwind` labels have a real runtime, the language talks to C, and there are
arrays/tuples/`const`. The **north star** is met by `examples/calc/` — a real
recursive-descent arithmetic interpreter written *in kardashev itself*, tokenizing
+ parsing-with-precedence + evaluating, compiled by `kardc`.

| Phase | Goal | Status |
|-------|------|--------|
| 21 | **Generic traits + associated types + `where` clauses**: `trait Iterator<T>` / `trait Container { type Item; }` / `fn f<T>() where T: Bound` | ✅ Trait type parameters (`trait Name<T>`), impls supplying them (`impl Iterator<i64> for Range`), parameterized bounds (`fn head<T, C: Container<T>>`), and the prelude `Iterator` migrated to `Iterator<T>` so `for`/`fold`/`map`/`filter` work over any element type. Associated types (`type Item;`, `Self::Item`, and `C::Item` at a bounded call site) + `where` clauses (desugared to inline bounds, byte-identical IR). **Deferred:** generic `dyn Trait<T>` (static dispatch only); impls take no own generic list; multiple bounds per param; assoc-type bounds/defaults. |
| 22 | **Aggregate data: fixed-size arrays `[T; N]` + tuples `(A, B)`** | ✅ Stack value-aggregates: arrays `[T; N]` → LLVM `[N x T]` (literal `[a,b,c]`, indexing, `arr[i] = x`, `&[T;N]` auto-deref) and tuples `(A, B)` → anonymous struct (`.0`/`.1` access, `let (x, y) = t` destructuring). **Deferred:** tuple `match` patterns, non-Copy elements, runtime bounds-checking of dynamic indices. |
| 23 | **Real panic + unwinding**: make the `panic`/`unwind` effect labels honest | ✅ `panic(msg)` prints to stderr then unwinds via setjmp/longjmp + a cleanup stack, **running Drop glue on the way out** (verified: guards drop in reverse during unwind), with `catch(f, recover)` to recover and an uncaught panic exiting 101. The single Phase-16 drop flag gates both the normal and panic paths so every value drops exactly once (no double-free, verified with a 100k-panic constant-memory loop). Panic-free programs emit zero panic machinery. |
| 24 | **FFI / C interop**: `extern "C"` import + export | ✅ `extern "C" fn name(args) -> T;` (and block form) declares external C functions, lowered to an unmangled LLVM extern + direct call; JIT resolves from the host process, AOT links via clang. Type→C-ABI mapping incl. a `i32`/C-`int`-width spelling (trunc/sext at the boundary) and `&String`/`&[T]` → C pointer; extern calls carry `io`. Verified `abs(-7)=7`, `strlen("hello")=5` in JIT+AOT. **Deferred:** export-to-C attribute. |
| 25 | **comptime / const evaluation**: `const` items, `const fn`, const folding, const generics | ✅ `const NAME: T = <const-expr>;` (i64/bool) evaluated at compile time and folded to a literal at every use (verified in `--emit-llvm`: `ret i64 5`, no runtime load/global); a const may reference an earlier const, and a cyclic/forward-bad reference is a clear error. `const fn` runs at compile time when called in a const context (with constant args) and is **also** an ordinary runtime fn (codegen unchanged). The evaluator covers int/bool literals, arithmetic/comparison/unary ops, `if`/`else`, `let`, and const-fn calls; it is bounded by call-depth + a step budget (runaway → clear error, not a hang), and integer overflow / div-by-zero are compile errors. **Const generics:** the `N` in `[T; N]` (Phase 22) is now any const-expr — a `const` item, a const-fn call, or arithmetic over them (`[i64; N]`, `[i64; sq(2)]`, `[i64; A + 1]`), evaluated to the array length. **Deferred:** const types beyond i64/bool, and full const-generic *type parameters* (`struct Arr<const N: i64>`) — only const-expr array lengths are in scope. |
| 26 | **Self-hosting milestone + stdlib depth** | ✅ `examples/calc/` — a real recursive-descent arithmetic interpreter written *in kardashev*, compiled by `kardc`: it tokenizes a string byte-by-byte (via the new `str_char_at` builtin) into a `Vec<Tok>`, parses with proper precedence through mutually-recursive grammar rules, and evaluates — exercising enums+`match`, structs, tuples, `Vec`, recursion, loops, and `const`. Verified end to end (`12 + 3 * (4 - 1)` → 21, etc.). Stdlib gained `str_char_at` (the tokenizer's core need). **Deferred:** broader stdlib (file I/O, formatting, generic sets) beyond what the capstone needed. |

Landed in order **21 → 22 → 23 → 24 → 25 → 26** (21's generic traits de-`i64`'d the
Iterator stdlib; 22's arrays fed 25's const-generic lengths; 23 built on v3's Drop;
24's FFI + 26's capstone pair up), each verified — clean build + direct behavior
checks — and CI-green on ubuntu + macOS before the next built on it.

## Roadmap v5 — shipped

v4 proved kardashev can compile a real program written *in itself* (`examples/calc/`).
v5's **north star** made that no longer a demo: two genuinely non-trivial tools written in
kardashev — a **JSON parser** (`examples/json/`) and a **kardashev-subset lexer**
(`examples/kdlex/`) — compile and run green, standing on a deeper stdlib, a tighter memory
model, and accurate docs. Phases 27–32 are implemented and green on the full suite (6 unit
suites + 46 smoke tests, JIT + AOT, ubuntu + macOS). As in v1–v4, each phase shipped fully
green before the next built on it, and everything unverifiable in this build environment is
documented-deferred, never stubbed.

| Phase | Goal | Status |
|-------|------|--------|
| 27 | **String toolkit** | ✅ `str_eq` (byte-exact, pure), `str_substring` (a fresh heap slice, start/len **clamped** into bounds), `int_to_string` (`snprintf "%lld"`), and `print_no_nl` — the genuinely new output op, since `print_str` / `print_string` / the new `println` all force a trailing newline. Verified JIT + AOT (`smoke_test_strings`). |
| 28 | **`Hash` trait + generic map/set keys** | ✅ Multiple trait bounds (`T: A + B`, inline + via `where`); a prelude `Hash` + `Eq` trait with built-in i64/String impls; **generic `HashMap<K, V>`** (the bucket entry now stores `K` — i64 keys keep the historic inline identity-hash+icmp; String keys hash via FNV-1a + compare via `str_eq`; **user key types** dispatch through their `impl Hash`/`impl Eq`); and `HashSet<T>`. A non-hashable key is a clear error. Verified at i64/String/user-struct keys (`smoke_test_hash`). |
| 29 | **Plug the documented Drop leaks** | ✅ The closure-env and `match`-payload-binding leaks are closed: a fn-value is droppable (its heap capture env is freed), and a droppable enum payload bound in a `match` arm drops at arm exit unless moved out. Verified with 2 M-iteration constant-memory loops (~1.5 MB RSS) for both, plus a no-UAF check that a moved-out payload survives (`smoke_test_dropleaks`). *(Dynamic array-index OOB already panics since Phase 23.)* **Documented-deferred:** async-frame interior free — freeing a completed `Future`'s heap frame needs reworking the executor task lifecycle (read-after-free risk on the poll slot) and async is off the v5 capstone path, so it stays a known leak rather than a risky half-fix. |
| 30 | **File I/O + CLI args** | ✅ `fs_read_to_string` → `Result<String, IoError>`, `fs_write` → `Result<i64, IoError>`, `fs_exists` → `bool`, and `args()` → `Vec<String>` (`arg_count`/`arg_get`). Errors are classified **portably** via `access()` probes (no libc `errno` symbol, so the IR links on Linux + macOS); the AOT `int main(argc, argv)` wrapper captures argv (JIT sees none); the runtime is emitted only when a program uses it. Verified JIT + AOT (`smoke_test_io`). |
| 31 | **Capstones, written in kardashev** | ✅ `examples/json/` parses a JSON object into a `HashMap<String, i64>` (the headline Phase-28 map), and `examples/kdlex/` lexes a kardashev subset into a `Vec<Tok>` and counts `fn` decls + checks brace balance — both compiled by `kardc`, JIT + AOT (`smoke_test_capstones`). **The north star.** *(Subset, documented: the JSON parser targets a numeric-config object — top-level, integer values — the sound shape that exercises the String-keyed map end to end; nested/string/bool values are out of scope.)* |
| 32 | **Docs + source-comment truth pass** | ✅ `docs/` (language reference, effects, stdlib, architecture) rewritten from the Phase-7/v2 era to v5; the stale "Phase 6 (stub)" comments that labelled now-fully-working async code are purged; a `doc-lint` smoke test (`smoke_test_doclint`) guards against the markers and the worst stale claims regressing. |

**Documented-deferred (carried forward unchanged, never stubbed):** third-party dependency
resolution via the Bazel module registry (Bazel can't run in this build environment;
`mod foo;` + `kard.toml` local-path deps are what ship); macOS/kqueue async fd-readiness
(Linux/`epoll` only here; timers work cross-platform, and CI covers macOS); and the new v5
deferral above — async-frame interior free (the `Future` heap frame is reclaimed by neither
`block_on` nor the executor yet, so a long-running async workload leaks frames; a bounded
one-shot does not).

Landed in order **27 → 28 → 29 → 30 → 31 → 32** along the planned dependency arc: 27's
strings fed 28's string-key hashing and 31's tokenizers; 28's `Hash` trait preceded the
generic keys; 29 plugged the Drop leaks 27–28's new droppable values made load-bearing;
30's `Result<String, IoError>` drops cleanly on the error path *because* 29 closed that
hole; 31 integrated 27–30 into the self-written capstones; 32 documented the result last.
Each shipped green before the next, exactly as v1–v4 did.

## Roadmap v17 — in progress

> **Status: in progress** on `feat/roadmap-v17`. "Self-hosting, continued" — unify
> v15's signatures and v16's body interpreter into a complete `Fn`, then push
> toward a type checker and codegen. Each phase a real, tested kardashev program
> in `examples/selfhost/`.
>
> - **Phase 98 — a whole-function parser + interpreter (done).** `examples/selfhost/func.kd`
>   parses a complete `fn NAME ( PARAMS ) -> RET { BODY }` into an `Fn { name,
>   params, ret, body }` AST (v15's signature + v16's body block) and INTERPRETS
>   it: scope-check the body against the parameters, bind the call arguments, and
>   evaluate the body. `fn f(a: i64, b: i64) -> i64 { let x = a + b ; x * 2 }`
>   called with `(3, 4)` → `x = 7`, `x * 2` = `14`, JIT + AOT.
>
 - **Phase 99 — fixed a memory-safety bug self-hosting surfaced (done).** Writing
>   Phase 98 hit a real **double-free**: moving a non-Copy struct field by value
>   (`vec_push(&mut v, p.name)`) double-frees — the borrow checker modeled
>   `s.field` as a read while codegen extracted it as an uncleared move, so the
>   struct's drop freed the field again (proven with a Drop counter: the field
>   dropped *twice*). Fixed by clearing the **root binding's** drop flag on a
>   field/index partial move, conservatively disabling the whole struct's drop so
>   the moved field can't be double-freed — eliminating the double-free for all
>   field types with no compile-error regression. Verified: the field now drops
>   exactly once; a loop of String field-moves is heap-clean (MALLOC_CHECK_=3);
>   all unit + smoke suites green. Pinned by `tests/smoke_test_fieldmove.sh`.
>   **This is the payoff of self-hosting: dogfooding the compiler found a real
>   memory-safety bug.**
>
> - **Phase 100 — per-field partial-move tracking, leak-free (done).** Phase 99's
>   fix was conservative: disabling the whole struct's drop after a partial move
>   left the *sibling* fields leaked (safe, but a leak). Phase 100 makes it
>   precise — an eligible plain struct local (no user `Drop`, non-panic program)
>   gets one drop flag **per droppable field**; moving a field out clears only
>   that field's flag, so its siblings are still freed at scope exit. Gated off
>   in may-panic programs (the unwind cleanup thunk drops the whole struct via a
>   single flag, which per-field flags would desync into a double-free on unwind)
>   and for structs with a user destructor (a partial move must not skip it) —
>   those keep the conservative whole-disable, leak-but-safe. Verified: after
>   `vec_push(&mut v, s.a)`, field `a` drops exactly once **and** sibling `b`
>   drops exactly once (was 0 — leaked); all unit + smoke suites stay green
>   including the panic/unwind path. Pinned by `tests/smoke_test_fieldmove.sh`
>   (sibling-drop case).
>
 - **Phase 101 — a real type checker, self-hosted (done).** Past scope-checking:
>   the self-hosted expression language now has TWO types — `i64` and `bool` — so
>   `examples/selfhost/typeck.kd` does genuine type inference. A recursive-descent
>   parser builds an `enum Expr` (numbers, vars, `+ * < ==`, `if/else`); `type_of`
>   infers each node's type against a type environment (var → type tag), enforcing
>   arithmetic on `int×int→int`, comparison on `int×int→bool`, and that an `if`
>   condition is `bool` with equal branch types — propagating a `TErr` tag on any
>   mismatch. Checked on a well-typed `if` (→`TInt`), an `int + bool` mismatch
>   (→`TErr`), a non-`bool` `if` condition (→`TErr`), and a bare comparison
>   (→`TBool`). JIT + AOT; pinned by `tests/smoke_test_phase101.sh`. A type
>   checker for kardashev, written in kardashev.
>
> Planned: thread the type checker through whole functions; eventually emit
> IR/code — toward a bootstrap.

## Roadmap v16 — shipped

> **Status: shipped.** "Self-hosting, continued" — grow the self-hosted front from
> v15's signatures to the BODY grammar: expressions, statements, scope checking,
> and a function-body interpreter, all written in kardashev in
> `examples/selfhost/`. All of v16 (Phases 94–97) is implemented and green — 6
> unit suites + the smoke aggregate, JIT **and** AOT.
>
> - **Phase 94 — an expression parser + evaluator (done).** `examples/selfhost/expr.kd`
>   is a recursive-descent parser for the body grammar: it builds an `enum Expr`
>   AST (`Num` / `Var` / `Add` / `Mul`, recursive via `Box`) for an arithmetic
>   expression with VARIABLE REFERENCES (the step beyond `examples/calc`'s
>   variable-free arithmetic), then EVALUATES it against a `HashMap<String, i64>`
>   environment `{ a: 3, b: 4 }`. Proves `*` binds tighter than `+`
>   (`a + b * 2` = 11) and that parentheses override it (`(a + b) * 2` = 14),
>   JIT + AOT. (`eval` walks the Box-recursive tree via the `&(**child)` borrow;
>   the cursor threads as a `&mut Pos` cell since kardashev has no `&mut i64`
>   deref-assign — a candidate ergonomics improvement.)
> - **Phase 95 — a statement/block parser + evaluator (done).** `examples/selfhost/stmt.kd`
>   grows the body grammar to a BLOCK: a sequence of `let NAME = EXPR ;` bindings
>   plus a final result expression, parsed into `Block { lets: Vec<Stmt>, result:
>   Box<Expr> }` and evaluated by running each `let` in order — EXTENDING the
>   environment — then the result, exactly how an interpreter executes a function
>   body. Over `let x = a + 1 ; let y = x * 2 ; y` with `{ a: 3 }`: `x = 4`,
>   `y = 8` (a `let` references both the outer `a` and the earlier `x`), result
>   `8`, JIT + AOT.
> - **Phase 96 — a scope/semantic checker (done).** `examples/selfhost/scopechk.kd`
>   walks the block AST and verifies every VARIABLE REFERENCE is bound — an outer
>   parameter or an earlier `let` — reporting UNDEFINED variables (a `let` RHS is
>   checked before its own name binds; each `let` extends the scope). With params
>   `{ a, b }`: `let x = a + 1 ; x + b` → 0 undefined; `let x = a + 1 ; x + c` → 1
>   (the undeclared `c`), JIT + AOT.
> - **Phase 97 — CAPSTONE: a function-body interpreter (done).** `examples/selfhost/interp.kd`
>   ties the whole body pipeline (lex → parse → scope-check → evaluate) into one
>   `interpret(body, params, args)`: it REJECTS a body referencing an undefined
>   variable (returns `-1`), and otherwise binds the arguments to the parameters
>   and runs the block to a value. `fn f(x=3, y=4) { let sq = x*x; let dbl = y+y;
>   sq + dbl }` → `17`; an ill-scoped body → `-1`. A self-hosted interpreter for
>   kardashev function bodies, written in the language it interprets, JIT + AOT.
>
> Planned: wire the body interpreter to the v15 `FnSig` (a full `Fn`); a real type
> checker; eventually emit code — closing language gaps as they surface.

## Roadmap v15 — shipped

> **Status: shipped.** "Self-hosting" — the north-star arc toward a bootstrap.
> v15 delivers a self-hosted compiler **front-end** (lexer + parser + checker)
> written *in* kardashev, in `examples/selfhost/`. All of v15 (Phases 88–93) is
> implemented and green — 6 unit suites + the smoke aggregate, JIT **and** AOT.
> The language already had the gating primitives — file I/O (`fs_read_to_string`
> → `Result<String, IoError>`), byte-level string access (`str_char_at` /
> `str_push_byte` / `str_substring`), recursive data (`enum` + `Box`), and symbol
> tables (`HashMap`) — so the front of the pipeline is expressible today. Full
> self-hosting (the whole compiler, incl. codegen) is a multi-roadmap effort the
> later roadmaps continue.
>
> - **Phase 88 — the lexer foundation proof (done).** `examples/selfhost/lexer.kd`
>   is a lexer *written in kardashev*: it scans a kardashev snippet
>   (`fn add(a: i64, b: i64) -> i64 { a + b + 42 }`) byte-by-byte and groups the
>   bytes into real tokens with correct boundaries — identifiers (`is_alpha` then
>   `is_alnum*`), integer literals, the multi-char `->`, and single-char
>   punctuation, whitespace skipped — reporting 9 idents / 1 number / 1 arrow /
>   9 punct and a position-weighted witness (9010109), checked JIT + AOT. Proves
>   the compiler front is expressible in the language itself.
> - **Phase 89 — a token-stream lexer (done).** `examples/selfhost/tokens.kd`
>   grows the classifier into a real lexer that returns a `Vec<Token>` — each
>   token carrying its KIND and its SPAN (`start` + `len`) into the source, the
>   typed interface a parser sits on. Over the same snippet it yields 20 tokens;
>   the first reconstructs (via `str_substring` over its span) to `"fn"` and the
>   arrow token to `"->"`, proving the spans are correct, with a deterministic
>   stream fingerprint — JIT + AOT.
> - **Phase 90 — a parser for kardashev syntax (done).** `examples/selfhost/parser.kd`
>   parses a function SIGNATURE — `fn add(a: i64, b: i64) -> i64` — into a
>   structured AST (`FnSig { name, params: Vec<Param>, ret }`), recovering each
>   name/type by `str_substring` over the token spans: name `"add"`, 2 params
>   (first `a: i64`), return `i64`, witness 1211, JIT + AOT. (Arithmetic-expression
>   parsing was already shown by `examples/calc`; this parses the *language's own*
>   grammar — the genuine step toward a self-hosted front-end.)
> - **Phase 91 — an AST printer + round-trip (done).** `examples/selfhost/printer.kd`
>   reprints the parsed `FnSig` back to source and checks it ROUND-TRIPS:
>   `fn add(a: i64, b: i64) -> i64` → tokens → AST → source is byte-identical
>   (length 29, `round_trips = 1`), JIT + AOT. A lossless round-trip proves the
>   AST captures everything the surface syntax carries — the invariant a real
>   parser must hold.
> - **Phase 92 — a scope/semantic checker (done).** `examples/selfhost/checker.kd`
>   builds a `HashMap<String, String>` SYMBOL TABLE over the `FnSig` AST and runs
>   real semantic checks: it RESOLVES a parameter's type by name (`b` → `i64`) and
>   REJECTS a duplicate parameter name (`fn bad(a: i64, a: i64)` is flagged). The
>   analysis phase a compiler runs after parsing, in kardashev — JIT + AOT.
> - **Phase 93 — CAPSTONE: the front-end, end to end (done).** `examples/selfhost/front.kd`
>   runs the WHOLE front it built (lex → parse → check → reprint) over a function
>   signature in one program, scoring it, and proves it generalizes across a
>   2-param and a 3-param signature (`211`, `311`, pipeline witness `211311`),
>   JIT + AOT. A self-hosted compiler **front-end written in the language it
>   compiles** — tokens with spans, a real `FnSig` AST, a HashMap symbol table
>   with duplicate-param + type-resolution checks, and a lossless source
>   round-trip.
>
> v15 delivers the self-hosted **front-end** (lexer + parser + checker). Full
> self-hosting (the whole compiler, incl. codegen) is a multi-roadmap effort the
> later roadmaps continue — parsing more of the grammar (statements, expressions,
> items) → a fuller `enum` AST → a real type checker → eventually emitting code,
> closing language gaps (richer errors, string ops) as they surface.

## Roadmap v14 — shipped

> **Status: shipped.** "Hardening" — make the toolchain trustworthy across
> platforms and inputs. All of v14 (Phases 82–87) is implemented and green — 6
> unit suites + the smoke aggregate (incl. the new JIT/AOT differential + the
> footgun checks), JIT **and** AOT. The headline: **macOS CI is green for the
> first time** (Linux CI green throughout; macOS green except a flaky
> `codegen_test` abort that's an arm64-JIT issue, documented below). The
> consolidation roadmap after three feature roadmaps (v11–v13) that each needed a
> soundness fix at review time.
>
> - **Phase 82 — portable memory/leak gates.** The RSS leak gates (the
>   constant-memory checks that catch drop/refcount leaks) hard-required GNU
>   `/usr/bin/time -v`, so on macOS (BSD `time`) they failed the build under
>   `set -euo pipefail` — the bulk of the long-standing macOS-CI red (11 of 12
>   failures). A shared portable `peak_rss_kb` (GNU `time -v` *or* BSD `time -l`,
>   else a clean SKIP) keeps the gate **running** on both platforms. This took
>   macOS CI from 12 failures to GREEN for the first time. Plus a CI step that
>   dumps any failing test's `test.log` so a red run is diagnosable from the log
>   alone.
> - **Phase 83 — jmp_buf hardening + the `codegen_test` flake, investigated
>   honestly.** The one remaining macOS failure, `codegen_test (Aborted)`, is
>   *flaky*, not deterministic — its crash point varies run to run (one run
>   completes the 50k-unwind panic test, the next crashes before any output). The
>   catch-stack `_setjmp` jmp_buf is now a generously-sized, 16-byte-aligned cell
>   (was a 1-aligned `[256 x i8]`) — correct defensive hardening, Linux-verified
>   (154 unit cases + the full suite), and monotonically safer. But it did **not**
>   clear the flake (3/3 macOS runs still aborted), and `codegen_test` built with
>   `-fsanitize=address,undefined` on Linux is 154/154 clean with **zero** findings.
>   So the flake is an **arm64-JIT-execution** issue (codegen_test JITs OS-thread
>   and setjmp/longjmp cases), not heap/UB corruption — undiagnosable on x86 Linux;
>   it needs a macOS-arm64 environment. Documented as the remaining blocker to a
>   *guaranteed-stable* green macOS, rather than papered over.
>
 - **Phases 84–85 — a SIGPIPE-robust smoke harness.** `echo "$big" | grep -q` /
>   `awk '…exit'` / `$CMD | head -N` make the producer die with SIGPIPE (exit
>   141) when the consumer closes the pipe early — a load-sensitive flake under
>   `set -o pipefail` that can bite shared CI runners. Swept ~51 such pipelines
>   across 31 files to here-strings / capture-then-process (consumers that read
>   to EOF — `tail`, `wc`, plain `grep` — left alone). const.sh went from ~3/5 to
>   12/12 under load.
> - **Phase 86 — the channel capture-and-keep footgun is now a compile error.**
>   A `Sender` captured into a closure is owned by the closure's heap env, which
>   never drops its captures — so the only way it is ever dropped (and the channel
>   eventually closes) is being *moved out* of the closure. If every use is `&tx`
>   (send-only, never moved into the worker or `chan_close`'d), the Sender leaks
>   and a `recv`-until-`None` consumer hangs. The typechecker now rejects exactly
>   that — a captured `Sender` with no by-value (move) use — with an actionable
>   message. The rule is *precise* (a bare by-value use is the only way a non-Copy
>   Sender leaves an env, so sound code always has one): zero false positives
>   across the whole v13 channel suite; pinned in `smoke_test_v13_review.sh`.
>
 - **Phase 87 — a JIT-vs-AOT differential sweep.** One test (`smoke_test_differential`)
>   runs all 9 single-file capstones (calc, checksum, csvstats, json, kdlex,
>   matrix, parstats, rpn, wordfreq) through BOTH backends and asserts they agree:
>   the JIT prints `main`'s `i64` return as a trailing line while the AOT process
>   exits with it (& 255), so AOT stdout must equal JIT stdout minus that line and
>   the line mod 256 must equal the AOT exit code. A single place that any future
>   codegen change must keep green — catches an ORC-JIT-vs-clang-AOT divergence on
>   real, diverse programs (parsing, the numeric tower, generics, traits,
>   collections, recursion, threads/channels, const-generics, Drop).
>
> Planned: property/fuzz testing (grammar-aware program generation); the macOS
> `codegen_test` flake (needs a macOS-arm64 runner to diagnose); then the v14
> close-out (version 0.14.0, PR to main, tag/release).

## Roadmap v13 — shipped

> **Status: shipped.** "Concurrency" — make thread-safety a CHECKED property, not a library convention. All of v13 (Phases 75–81) is implemented and fully green — 6 unit suites + the smoke-test aggregate (incl. ~600 concurrency stress runs), JIT **and** AOT, on a cleared clean build.
>
> - **The `share` effect** (75): `thread_spawn` carries `share`, so a fn that spawns must declare `! { share }`; it rides the effect-SUBSET rule, so a pure-declared trait method can NEVER have an impl that spawns — concurrent work can't be smuggled past a `<T: Task>` / `&dyn Task` interface.
> - **Typed MPSC channels** (76–77): `channel<T>() -> (Sender<T>, Receiver<T>)` over a pthread mutex+condvar queue that MOVES a real `T` across threads (`String`, `Vec`, structs), with ownership transferring sender → node → receiver. The structural **`Send`** rule gets teeth at `chan_send` — a `&T` borrow / `Receiver` / `Rc` (or any aggregate containing one) can't cross; the `Receiver` is single-consumer and not `Send`.
> - **`Rc<T>`** (78): a non-atomic refcounted shared owner — drop-once, no leak, and the legible **non-`Send` witness** (sending one on a channel names `Rc` as the error).
> - **Fork-join + capstone** (79–80): the primitives compose into real parallelism — `chan_try_recv` (non-blocking) and a parallel map-reduce (`examples/parstats`) split across 4 worker threads, gathered + merged, checked against the sequential answer.
> - **Refcounted move-only endpoints** (81, from the review): the Rust ownership model — `chan_send`/`chan_recv` borrow the endpoint, `sender_clone` makes a second producer, the channel closes when the last `Sender` drops, and the block + queue are freed when the last endpoint drops.
>
> **A pre-merge adversarial multi-agent review (3 reviewers, ~600 stress runs) found + fixed a BLOCKER and two MAJORs the green suite missed** (pinned by `tests/smoke_test_v13_review.sh`): a **use-after-free** (`rc_get`/`vec_get_ref` returned a `&T` whose lifetime wasn't tied to the owner, so the owner could be moved while the borrow was live — now a borrow error); an **unbounded channel leak** (Copy handles owned nothing, so each `channel()` leaked its block + undrained nodes — now reclaimed at last-endpoint drop, RSS flat over 1M channels); and **multi-producer `chan_close` data loss** (a single boolean close let one producer end the stream for others — 84/100 runs lost data; now refcounted, closing only on the last sender). The Send/`share` soundness surface the review hammered came back **clean**. **As with v6–v12: a green smoke suite ≠ sound; review before merge.**

## Roadmap v12 — shipped

> **Status: shipped.** "Real stdlib" — turn a language you can *compute* in into one you can *get data in and out of*. All of v12 (Phases 69–74) is implemented and fully green — 6 unit suites + the smoke-test aggregate, JIT **and** AOT, on a cleared clean build.
>
> - **String → number parsing** (69): `parse_int` / `parse_f64` returning `Option` (all-or-nothing, ERANGE-checked), `int_to_hex`. The #1 gap — reading data — closed.
> - **Vec mutation + query** (70): `vec_pop` / `vec_remove` (MOVE the element out, sound for non-Copy) / `vec_insert` / `vec_reverse` / `vec_contains` / `vec_index_of`.
> - **HashMap/HashSet enumeration** (71): `hashmap_contains` / `hashmap_values` / `hashset_items`.
> - **String methods** (72): `str_starts_with` / `str_ends_with` / `str_contains` / `str_index_of` / `str_to_upper` / `str_to_lower` / `str_concat` / `str_repeat`.
> - **Numeric + math** (73): `i64_abs` / `min` / `max` / `pow`; `f64_sqrt` / `floor` / `ceil` / `abs` (LLVM intrinsics, `-lm`); more `Option`/`Result` inspectors.
> - **Capstone** (74) `examples/csvstats`: a CSV statistics aggregator that READS data — grouping `category,value` rows into per-category count + sum + global max, sorted — exercising the whole v12 line at once.
>
> **A pre-merge adversarial multi-agent review found + fixed two MAJORs the green suite missed** (pinned by `tests/smoke_test_v12_review.sh`): a `parse_int` integer overflow (a value past `i64` range returned a silently-clamped `Some` instead of `None`), and a discarded-owned-temporary leak (`vec_remove(&mut v, 0);` as a statement never dropped its moved-out value). **As with v6–v11: a green smoke suite ≠ sound; review before merge.**

## Roadmap v11 — shipped

> **Status: shipped.** "Real machine integers" — the **numeric tower** that turns kardashev from an i64-only toy into something you can write a hash or a binary format in. All of v11 (Phases 63–68) is implemented and fully green — 6 unit suites + the smoke-test aggregate, JIT **and** AOT, on a cleared clean build. Each phase shipped green before the next.
>
> - **Sized integers** (63): signed `i8`/`i16`/`i32`/`i64`, lowering to the matching LLVM width over a NON-coercive lattice — no implicit widening (`i32 + i64` is a type error), only an explicit `as` bridges.
> - **Literal suffixes + radix** (64): `5i32`, `0xFF`, `0b1010`, `0xFFi32`; hex/binary work in `match` patterns; `u64` literals past `i64::MAX`.
> - **The `as` cast operator** (65): the conversion matrix — `trunc`/`sext`/`zext`, `sitofp`/`fptosi`, `fpext`/`fptrunc` — width- and sign-correct, const-foldable.
> - **Unsigned integers + bitwise ops** (66): `u8`…`u64` with sign-correct `udiv`/`urem`/`lshr`/`icmp-u`, and `& | ^ << >> ~` at any width.
> - **Defined overflow + f32** (67): two's-complement wrapping at every width; `f32` single-precision over the same non-coercive lattice.
> - **Capstone** (68) `examples/checksum`: **FNV-1a**, **CRC-32**, and an endian **binary parser** — each checked against its known answer, none expressible in an i64-only language.
>
> **A pre-merge adversarial multi-agent review found + fixed a const-evaluation width/sign cluster (incl. an invalid-IR BLOCKER — a narrow const flowed into a narrow slot as an i64 immediate) plus a `expr as Type <<` cast-parse bug and a tuple-index suffix lex bug** — each with a verified repro, all pinned by `tests/smoke_test_v11_review.sh`. **As with v6–v10: a green smoke suite ≠ sound; review before merge.**

## Roadmap v10 — shipped

> **Status: shipped.** All of v10 (Phases 57–62) is implemented and fully green — 6 unit suites + the smoke-test aggregate (71 smoke tests), JIT **and** AOT, on a cleared clean build. Each phase shipped green before the next, exactly as v1–v9 did. The capstone is a **fixed-size linear-algebra library** (`examples/matrix`): `Matrix<const R, const C>` carries its shape in the TYPE, `transpose()` returns `Matrix<C, R>`, and a dimension-checked `matmul` rejects a shape mismatch at *compile* time. **A pre-merge adversarial multi-agent review (6 dimensions) found + fixed 5 BLOCKERS + 5 majors the green smoke suite had missed** — nested-const-field mangling, a bare const-generic `.clone()` type-confusion, `Drop` laundering io via `dyn Drop`, a bounded-generic call attributing ZERO effects, a symbolic-dimension-forwarding miscompile, and a mangling collision — each with a verified repro, all pinned by `tests/smoke_test_v10_review.sh`. **As with v6–v9: a green smoke suite ≠ sound; review before merge.**

v9 closed out "data in motion", but the deferral ledger still names const-generic **type** params as the most-leveraged unfinished spine: Phase 25 shipped const-expr array *lengths* but explicitly deferred `struct Arr<const N: i64>`, and `<const N: i64>` does not even parse today. v10's theme is **sized and sound at compile time**: make a SIZE a first-class, substitutable, monomorphized type argument (turning a folded literal into a real, mangled, per-shape LLVM type), and — on the same spine — retire the signature feature's last soundness floor (an **effect-subset rule** on impl methods, so the dyn/generic effect attribution becomes sound by construction). It climaxes in a fixed-size **linear-algebra library written entirely in kardashev** whose dimension mismatches are *compile* errors and whose pure-vs-IO split is effect-honest. Two grafts beyond pure const-generics (chosen by a design review's judge panel) make it worth shipping rather than a single-axis demo: the effect-subset rule, and the closure-param-type inference fix (the highest-fan-out v9 deferral, so the capstone's reductions read as plain unannotated closures).

| Phase | Goal | Status |
|-------|------|--------|
| 57 | **Parse + bind const-generic params; symbolic array lengths (+ tuple-let annotation)** | ✅ *Shipped.* Accept `const N: i64` in `parseOptionalGenericParams` (mixed with type params), record a symbolic `[i64; N]` length on `TypeRef`/`Type` (mirroring how `arrayElem` carries an unresolved Var), and close the documented `let (a, b): (T, T) = ..` deferral (a tuple-pattern annotation hook in `parseLetStmt`). Parse + type-check the declaration shell only — no instantiation yet. Header change ⇒ `make clean`. **Accept:** `struct Mat<const N: i64> { data: [i64; N] }` + `let (a, b): (i64, i64) = (3, 4)` parse with zero errors and run (exit 7), JIT + AOT; `Mat<const N: bool>` and an arity-mismatched tuple-let are clear errors; ast_print round-trips both spellings. |
| 58 | **Monomorphize over a const value (the core)** | ✅ *Shipped.* Turn the symbolic length into a substituted, mangled, monomorphized type argument: bind the const arg in the instance env (parallel to the type-param map), fix `resolveInInstance`'s Array case to substitute `arrayLenVar` instead of copying it (the verified one-line gap), const-eval the supplied length (`evalConstI64`, so `Mat<2+1>` works), and mangle the const into the struct's `typeArgs` so `Mat<3>` and `Mat<5>` are distinct LLVM types. **Accept:** a `Mat<3>` and a `Mat<5>` built + indexed return a known signal (JIT + AOT); a wrong-length literal is a type error; `--emit-llvm` shows two DISTINCT struct types (`[3 x i64]` vs `[5 x i64]`) — monomorphization, not erasure. |
| 59 | **Const-generic functions + compile-time dimension unification** | ✅ *Shipped.* `fn zeros<const N: i64>() -> Mat<N>` / `fn dot<const N: i64>(a: &Mat<N>, b: &Mat<N>) -> i64` where the SAME `N` must UNIFY across args — a length mismatch is a *compile* error, the type-level analog of a bounds check. Reuse the fn-monomorphization worklist; const params are usable as i64 in the body (loop bounds/indices), folded to literals. **Accept:** `dot` over two `Mat<3>` returns 32 (JIT + AOT); `dot(mat3, mat5)` is a compile error ("expected `Mat<3>`, found `Mat<5>`"); the smoke test covers the value AND the negative-compile case. |
| 60 | **Effect-subset rule: an impl method may not exceed its trait's declared effects** | ✅ *Shipped.* Establish the soundness FLOOR the effect feature has lacked: after `checkEffects` over impl methods, require the impl method's effect set ⊆ the trait method's declared set (after effect-row-var substitution). This makes the dyn/generic effect attribution (which trusts the trait sig) sound *by construction* — the trait's effects become a true upper bound, closing the v8/v9 effect-relaxation residual. The Phase-46 container `Eq`/`Clone` impls that legitimately allocate are handled by WIDENING the relevant prelude trait sigs to `! { alloc }`, not by special-casing. **Accept:** an impl declaring an effect its trait doesn't is rejected with a clear effect-subset error; a generic/`dyn` call on a conforming impl is attributed correctly; `smoke_test_phase45/46` + `smoke_test_soundness` stay green. |
| 61 | **Const-generic ring buffer (Drop/clone) + closure-param inference** | ✅ *Shipped.* Prove const generics carry the memory model: a `RingBuffer<T, const CAP: i64>` over a NON-Copy `T` (`[T; CAP]` backing + head/len), with push/pop, `#[derive(Clone)]`, sound recursive Drop (every live slot dropped exactly once), and a push-when-full `Result` error (no UB). Graft the closure-param-type inference fix: when a call argument is a closure and the expected param is a `fn(..)` type, pre-unify the closure's param/return Vars against it BEFORE checking the body — so `vec_map(v, |x| int_to_string(*x))` works UNANNOTATED. **Accept:** a 200k `RingBuffer<String,3>` build/push-pop/drop loop holds peak RSS ≤ 32 MB; derived `Clone` is an independent deep copy; unannotated `vec_map`/`vec_filter`/`vec_fold` closures match their annotated forms — JIT + AOT. |
| 62 | **Capstone: a fixed-size linear-algebra library, in kardashev** | ✅ *Shipped.* Integrate the whole spine: a `Matrix<const R: i64, const C: i64>` (two independent const dimensions) with `new`/`identity`, `transpose() -> Matrix<C, R>` (dimensions SWAP at the type level), and a dimension-checked `mul<const K: i64>(&Matrix<R,K>, &Matrix<K,C>) -> Matrix<R,C>` whose inner `K` must unify at COMPILE time; row/col reductions via UNANNOTATED `vec_map`/`vec_fold` closures (P61); a PURE trace/determinant path alongside an IO pretty-printer, proven sound by the effect-subset rule (P60); `let (r, c): (i64, i64) = m.dims()` (P57). Each `(R,C)` is a distinct monomorphized LLVM type (P58). **Accept:** `examples/matrix/main.kd` — a 2×3 · 3×2 yields a 2×2 matching a hand-computed reference (signal value), `transpose` has type `Matrix<3,2>`, a 2×3 · 2×2 multiply is a compile-time dimension error, a leaked IO effect under a pure trait method is rejected; identical JIT + AOT, constant peak-RSS, distinct LLVM types per shape, wired into `make -f Makefile.local test`. **A size, made real — sound by construction.** |

**Landing order — 57 → 58 → 59 → 60 → 61 → 62.** 57 is parse/AST/`types.hpp` plumbing only (the single header-touching phase, so the `make clean` discipline lands once up front) and clears the tuple-let deferral the capstone needs. 58 is the core vertebra — symbolic length → substituted, mangled, monomorphized value — with an `--emit-llvm` distinct-type gate proving real monomorphization before any library leans on it. 59 lifts the size from structs to functions and adds compile-time dimension unification. 60 (effect-subset) lands before the heavy generic-dispatch call sites so 61's closure-carrying ops and the capstone's pure-vs-IO split are effect-honest by construction (widening the prelude container sigs first keeps Phase-46 green). 61 fuses const generics with Drop/clone over a non-Copy buffer and grafts the closure-inference fix. 62 integrates all five into a dimension-checked, effect-sound, per-shape-monomorphized linear-algebra library written in kardashev. Each phase ships fully green before the next, as v1–v9 did.

## Roadmap v9 — shipped

> **Status: shipped.** All of v9 (Phases 51–56) is implemented and fully green — 6 unit suites (631 cases) + the smoke-test aggregate, JIT **and** AOT, on a cleared clean build. Each phase shipped green before the next built on it, exactly as v1–v8 did. The capstone is a **word-frequency histogram**: a real data pipeline — tokenize → count in a `HashMap` → rank by a derived `Ord` → print top-N — leak-free under the constant-memory gate.

v8 finished the generics core and proved it with a HashMap-backed, fully-derived JSON 3.0. But the stdlib still had no way to *transform* a collection without a hand-written `while` loop, two deferrals remained (trait `Clone`/`Eq` for `Box`; **generic** associated functions), and there was no string tokenizer. v9's theme is **data in motion**: it closed those two deferrals, gave `Vec` real higher-order combinators (`map`/`filter`/`fold` over closures), added string splitting + HashMap entry iteration, and proved it with a genuine data pipeline — a word-frequency histogram, counted in a `HashMap` and ranked by a derived `Ord`.

| Phase | Goal | Status |
|-------|------|--------|
| 51 | **`Box<T>` as a first-class impl target + deref ergonomics** | ✅ `Box` is now a registrable impl target (a `typeName` in impl registration + method-call resolution); the auto-deref peel STOPS at a `Box` that has the called method (so `box.clone()` reaches the Box impl, not the inner T's); `&*e`/`&**e` lowers to the pointer; and a `&self` method impl'd on Box gets the receiver's address. Prelude `impl<T: Clone> Clone for Box<T>` + `impl<T: Eq> Eq for Box<T>` — closing the v8 deferral. Verified: Box<i64>/Box<String> clone+eq; a `#[derive(Clone, Eq)] enum List { Nil, Cons(i64, Box<List>) }` round-trips — JIT + AOT (`smoke_test_phase51`). |
| 52 | **Generic associated functions** | ✅ Extended Phase-48's `Type::method()` static call to a *bounded generic* `T::method()`: checkCall resolves it via T's bound trait (Self → T) and records (trait, method, varId); codegen resolves the var to the concrete type at the instance and calls that impl. Verified: `fn first<T: Default>() -> T` and `pair_of<T: Default>()` run for i64/String, a user `Default` reached through the bound — JIT + AOT (`smoke_test_phase52`). *(Generic-TYPE static calls `Pair::default()` + enum `Default` still deferred — concrete + bounded-param static calls work.)* |
| 53 | **`Vec` higher-order combinators** | ✅ Prelude generic `vec_map<T,U>` / `vec_filter<T>` / `vec_fold<T,A>` over closures, each effect-polymorphic in the closure's effect row (pure mapper → pure caller; allocating → adds `alloc`); the closure gets each element by `&T`; map/filter return fresh owned Vecs (filter deep-clones the kept elements). Fixed a real borrow-check bug en route: an assignment now re-initializes its target's move-state, so `acc = f(acc, x)` (fold) is valid in a loop. Verified: map Vec<i64>→Vec<String>, filter evens, fold sums; constant-memory — JIT + AOT (`smoke_test_phase53`). *(A closure passed to these needs its param type annotated — `\|x: &i64\| ..`.)* |
| 54 | **String tokenizing** | ✅ Prelude `str_split(&String, sep: i64) -> Vec<String>` (split on a byte; empty pieces kept) + `str_trim` (ASCII whitespace), written in kardashev over `str_char_at`/`str_substring`. Verified: `"a,bb,,c"` → 4 pieces incl. the empty one, `"  hi  "` → `"hi"`, constant-memory — JIT + AOT (`smoke_test_phase54`). |
| 55 | **HashMap entry iteration → `Vec<(K, V)>`** | ✅ Prelude `hashmap_entries<K: Hash+Eq+Clone, V: Clone>(&HashMap<K,V>) -> Vec<(K, V)>` (each entry a deep-cloned tuple), built on `hashmap_keys` + Phase-36 non-Copy tuples — a map is rankable/printable without manual key lookups. Verified: a `HashMap<String,i64>` → `Vec<(String,i64)>` ranked via a derived-`Ord` wrapper + sort, constant-memory — JIT + AOT (`smoke_test_phase55`). |
| 56 | **Capstone: word-frequency histogram** | ✅ `examples/wordfreq` — tokenize (`str_split`/`str_trim`) → count in a `HashMap<String, i64>` → `hashmap_entries` → wrap in `#[derive(Ord, Eq)] struct Count` ordered freq-desc-then-word-asc → `sort` → print top-N. Building it surfaced + fixed a genuine latent leak: a **duplicate-key** `hashmap_insert` (the counter hot path) never dropped the old value nor freed the redundant key — a 200k loop fell from ~46 MB to ~1.6 MB. Verified: over a fixed text, top 3 = `the 3`, `cat 2`, `mat 1`, deterministic, constant-memory, JIT + AOT, wired into `make test` (`smoke_test_wordfreq`). **A data pipeline, end to end, in kardashev.** |

**Landing order — 51 → 52 → 53 → 54 → 55 → 56.** 51 and 52 cleared the two v8 deferrals (Box trait impls; generic associated functions). 53 added the `Vec` combinators the pipeline transforms with. 54 added the tokenizer that feeds it. 55 made a `HashMap` rankable as entries. 56 integrated all of it — split → count → entries → derived-`Ord` sort → top-N — the data pipeline, and closed a real duplicate-key-insert leak. Each phase shipped fully green before the next, as v1–v8 did.

**Documented-deferred (honest, never stubbed):** generic-TYPE static calls (`Pair::default()`) + enum `Default` (concrete + bounded-param `T::default()` work); closure-param-type inference from a generic fn-typed param (annotate the param); type-annotated tuple-pattern `let (a, b): (T, T) = ..`. Candidate v10 themes: lazy iterator adaptors, `Result`/`?` over the combinators, `Send`/`Sync` + channels, const generics.

## Roadmap v8 — shipped

> **Status: shipped.** All of v8 (Phases 45–50) is implemented and fully green — 6 unit suites (631 cases) + the smoke-test aggregate, JIT **and** AOT, on a cleared clean build. Each phase shipped green before the next built on it, exactly as v1–v7 did. The capstone is **JSON 3.0**: objects are a real `HashMap<String, Json>` again, the whole value `#[derive]`s Clone + Eq, and serialization is canonical (sorted keys) — round-tripping via the derived order-independent `Eq`, leak-free under the constant-memory gate.

v7 gave kardashev floats, generic `impl<T>` blocks, `#[derive]`, and a JSON-2.0 capstone — but that capstone had to drop to an order-preserving `Vec<Entry>` for objects because a generic `Clone`/`Eq` **trait impl** for `HashMap` couldn't be written (a generic-impl body couldn't satisfy the HashMap-op `K: Hash + Eq` requirement from a *bounded type param*). v8's theme is **generics, finished**: it made bounded type params first-class inside container ops, shipped the container trait impls (restoring HashMap-backed JSON), rounded out `#[derive]`, added the `Ord`/`sort` the stdlib was missing, and added generic trait objects — proving it all with a JSON 3.0 capstone whose objects are real `HashMap`s again, with canonical sorted-key output.

| Phase | Goal | Status |
|-------|------|--------|
| 45 | **Bounded type params inside container ops** | ✅ A bounded generic param `K: Hash + Eq` now satisfies the `HashMap<K, V>` / `HashSet<K>` key requirement from inside a generic body: the key-hashable gate tracks the FULL bound set of every generic Var in scope (primary + `extraBounds`), exposed before every `forType`/signature/body resolution. Verified: `fn count_keys<K: Hash + Eq, V>(m: &HashMap<K,V>)` runs for `HashMap<String,i64>` AND `HashMap<i64,i64>`; a generic-impl method builds/queries a `HashMap<K,V>` (K from the receiver); an unbounded key is rejected — JIT + AOT (`smoke_test_phase45`). |
| 46 | **Generic `Clone`/`Eq` trait impls for `HashMap`** | ✅ Prelude `impl<K: Hash+Eq+Clone, V: Clone> Clone for HashMap<K,V>` + `impl<K: Hash+Eq+Clone, V: Eq> Eq for HashMap<K,V>` (order-independent), so `.clone()`/`.eq()` dispatch through entries and `#[derive(Clone, Eq)]` covers a HashMap field. A trait-impl method may carry MORE effects than the trait (the Eq impl allocates); a derived `eq` is annotated `! { alloc }` ONLY when a field transitively contains a map (a map-free derived eq stays pure). Verified incl. a `#[derive(Clone, Eq)]` struct with a map field — JIT + AOT (`smoke_test_phase46`). *(Trait `Clone`/`Eq` for `Box<T>` deferred — `Box` isn't a registrable impl target yet and `&**self` deref doesn't parse; the `clone(&x)` intrinsic still deep-clones a Box.)* |
| 47 | **`Ord` trait + generic `sort`** | ✅ An `Ord` trait (`cmp(&self, &Self) -> i64`, −1/0/1) with built-in impls for i64/f64/String (byte-wise lexicographic), and a generic in-place `fn sort<T: Ord>(v: &mut Vec<T>)` (insertion sort). Needed three supporting pieces: a `vec_swap` builtin (ownership-neutral slot exchange, sound for non-Copy T), a `&mut T → &T` reborrow coercion, and a borrow-check reborrow (a `&mut` arg reborrows rather than moves). Verified: sort orders `Vec<i64>`/`Vec<String>`/`Vec<f64>`/a user `Ord` type — JIT + AOT (`smoke_test_phase47`). |
| 48 | **`#[derive(Hash, Ord, Default)]` + associated functions** | ✅ `Hash` (combine field/payload hashes + variant ordinal — makes a derived type a HashMap key), `Ord` (lexicographic fields; variant-ordinal-then-payload for enums), and `Default` (field-wise). `Default` needed **associated functions**: no-self trait methods + `Type::method()` call resolution (the parser keeps the path qualifier; checkCall resolves it against the type's impl; codegen calls the mangled impl method). Verified — JIT + AOT (`smoke_test_phase48`). *(Generic associated functions — `Pair<T>::default()` — and enum `Default` deferred; concrete static calls work.)* |
| 49 | **`dyn Trait<T>` — generic trait objects** | ✅ A parameterized `dyn Trait<Arg>` object: the parser accepts trait type args, the Dyn type carries them, `checkDynMethodCall` binds the trait's params so a method returning the trait param resolves concretely, and the vtable thunk forwards to the impl's already-concrete signature (no codegen change). Also fixed dispatch through the `&Box<dyn>` that `vec_get_ref` returns, so a heterogeneous `Vec<Box<dyn Trait>>` is iterated with dynamic dispatch. Verified: a `Vec<Box<dyn Producer<i64>>>` with two impls dispatched at one call site, plus a `Vec<Box<dyn Display>>` — JIT + AOT (`smoke_test_phase49`). |
| 50 | **Capstone: JSON 3.0 — HashMap objects + full derive + canonical output** | ✅ `examples/json` restores `JObj(HashMap<String, Json>)` (the v6 north-star shape), now `#[derive(Clone, Eq)]`-able via 45–46; the serializer SORTS the map keys (47's `sort`) for canonical byte-stable output; the round trip holds via the DERIVED order-independent `Eq` (no hand-written `json_eq`, no clone intrinsic). Verified: parse `{ "b": 2, "a": [1, 2.5], "c": "x\ny" }` → canonical `{"a":[1,2.5],"b":2,"c":"x\ny"}` (signal 30) → derived-Eq round trip, 200k parse+serialize+drop loop flat at ~1.7 MB, JIT + AOT, wired into `make test` (`smoke_test_json`). **The north star — HashMap JSON, fully derived — reached.** |

**Landing order — 45 → 46 → 47 → 48 → 49 → 50.** 45 made bounded params first-class in container ops (the blocker for everything map-generic). 46 used it to ship the container trait impls. 47 added `Ord`/`sort` (needed for canonical output). 48 extended `#[derive]` to Hash/Ord/Default (and added associated functions for `Default`). 49 added generic trait objects + heterogeneous-collection dispatch. 50 integrated all of it into a HashMap-backed, fully-derived, canonically-serialized JSON 3.0. Each phase shipped fully green before the next, exactly as v1–v7 did.

**Documented-deferred (honest, never stubbed):** trait `Clone`/`Eq` for `Box<T>` (Box isn't a registrable impl target + `&**self` deref doesn't parse; the `clone(&x)` intrinsic deep-clones a Box); **generic associated functions** (`Pair<T>::default()`) + enum `Default` (concrete `Type::default()` works); multiple parameterizations of one trait on one type as distinct `dyn` objects (the vtable keys on trait name + type). Candidate v9 themes: `Box` as an impl target + better deref ergonomics, generic associated functions, `Iterator` adaptors (map/filter/collect) over the now-real `Ord`/`Hash`/`Default`, `Send`/`Sync` + channels.

## Roadmap v7 — shipped

> **Status: shipped.** All of v7 (Phases 39–44) is implemented and fully green — 6 unit suites (631 cases) + the smoke-test aggregate, JIT **and** AOT, on a cleared clean build. The capstone is **JSON 2.0**: the nested-JSON value now carries `f64` numbers and decoded string escapes, and its `Clone`/`Eq` are `#[derive]`d — round-tripping via the derived `Eq`, leak-free under the constant-memory gate.

v6 made the heap recursive and proved it on a full nested-JSON value — but that capstone could only carry **integers** (`1.5` didn't even lex) and its helpers were hand-written because kardashev had **no generic `impl<T>` blocks**. v7's theme is **real numbers, real abstraction**: it gave the value layer `f64`, gave the abstraction layer generic impls so `Clone`/`Display`/`Eq` are real `#[derive]`-able traits, and proved it by upgrading JSON to floats + escapes with derived helpers — still leak-free.

| Phase | Goal | Status |
|-------|------|--------|
| 39 | **`f64` floating-point** | ✅ Lexer float literals (`1.5`, `-2.0`, `3e8` — carefully not swallowing `1..5`); an `f64` type → LLVM `double`; arithmetic (`+ - * /`), ordered comparisons, FNeg; explicit `to_f64` / `float_to_int` conversions + `print_f64` (no implicit i64↔f64 mixing); `f64` is Copy + a valid Copy aggregate element. Verified: an `f64` kernel + a `Vec<f64>` sum, JIT + AOT (`smoke_test_phase39`). *(Single width; NaN/Inf as LLVM gives.)* |
| 40 | **Generic `impl<T: Bound>` blocks** | ✅ Parse + typecheck + monomorphize `impl<T: Trait> Trait2 for Container<T>` (the open Phase-21 gap): the impl's params are prepended to each method's generic vars, bound-checked, and inferred from the receiver; methods monomorphize per concrete T. Verified: `impl<T: Display> Display for Pair<T>` formats `Pair<i64>` AND `Pair<String>` (distinct instances); a generic impl on built-in `Vec<T>`; unbounded-T trait call rejected — JIT + AOT (`smoke_test_phase40`). |
| 41 | **Real generic `Clone` / `Eq` over containers** | ✅ Prelude `impl<T: Clone> Clone for Vec<T>` + `impl<T: Eq> Eq for Vec<T>` dispatch through each element's own impl (the `clone(&x)` intrinsic stays the fallback). Required completing the Phase-40 deferral — a generic-impl method dispatching THROUGH a bounded-generic receiver to another generic-impl method (the call site now threads the receiver's concrete type args). Verified: `Vec<Pair<String>>` deep-cloned via a user `Clone for Pair` (→ String::clone) + compared via `Eq`, constant memory, JIT + AOT (`smoke_test_phase41`). *(Box/HashMap trait Clone/Eq deferred to the intrinsic.)* |
| 42 | **`#[derive(Clone, Eq, Display)]`** | ✅ A `#[derive(...)]` attribute (new `#` token) that SYNTHESIZES the impls by generating impl source from the type's fields/variants and re-parsing it — riding the real generic-impl machinery. Verified: `#[derive(Clone, Eq)]` on a recursive `enum Json` clones + deep-compares with no hand impl; `#[derive(Display)]` yields `P { x: .., y: .. }` / `C(a, b)`; works on a generic `Pair<T>` — JIT + AOT (`smoke_test_phase42`). |
| 43 | **Runtime string escapes + close the async-frame leak** | ✅ `str_unescape`/`str_escape` (kardashev prelude fns over a new `str_push_byte`) decode/encode `\n \t \r \" \\ \/ \uXXXX` for runtime strings (\u = Latin-1). **And the last documented leak is closed**: completed Futures' heap frames + poll slots are reclaimed — block_on reaps when the executor is idle (safe: never frees a live frame nor invalidates a spawn/join handle), and an `.await` frees the completed sub-future's frame. A 300k block_on loop drops ~50 MB → flat ~1.5 MB; spawn/join/executor intact. Verified JIT + AOT (`smoke_test_phase43`). |
| 44 | **Capstone: JSON 2.0 — floats + escapes + derived helpers** | ✅ `examples/json` now carries `JNum(f64)` + escape-aware string parse/serialize (+ a new `f64_to_string`), with `#[derive(Clone, Eq)]` on `Json`/`Entry` — the hand-written `json_eq` retired. Round-trips via the DERIVED `Eq`. Verified: parse `{"pi":3.14159,"msg":"a\nb","xs":[1.5,-2.0,true,null]}`, derived-Eq round-trip, 200k parse+serialize+drop loop flat at ~1.6 MB, JIT + AOT, wired into `make test` (`smoke_test_json`). *(Objects are an order-preserving `Vec<Entry>` so `#[derive]` applies uniformly — a HashMap trait Clone/Eq needs a bounded-key Hash+Eq path, deferred; the v6 HashMap object form + its intrinsic clone remain valid.)* **The north star — reached.** |

**Landing order — 39 → 40 → 41 → 42 → 43 → 44.** 39 added the missing scalar (self-contained). 40 was the **keystone**: generic `impl<T>` blocks unblocked everything abstraction-related. 41 used 40 to make `Clone`/`Eq` real generic traits; 42 derived them automatically. 43 closed the runtime-escape + async-frame gaps. 44 integrated all five into the floats-and-escapes JSON with derived helpers — the integration proof under its constant-memory gate. Each phase shipped fully green before the next, exactly as v1–v6 did.

## Roadmap v6 — shipped

> **Status: shipped.** All of v6 (Phases 33–38) is implemented and fully green — 6 unit suites (631 cases) + the smoke-test aggregate, JIT **and** AOT, on a cleared cache. Each phase shipped green before the next built on it, exactly as v1–v5 did. The capstone — a **full nested-JSON parser + serializer written in kardashev** — round-trips and runs leak-free under a constant-memory gate.

v5 proved kardashev can compile genuinely non-trivial tools written *in itself* — but its JSON capstone (`examples/json/`) had to collapse to a **flat numeric-config subset** (`HashMap<String, i64>`, integer values only), because reading a value back out of a container is a buffer-sharing *shallow copy* that Phase-29 drop then double-frees, and `match` cannot go *through* a reference. v6's **north star** retires that asterisk: a **FULL nested-JSON value type written in kardashev** —

```rust
enum Json { JNull, JBool(bool), JInt(i64), JStr(String), JArr(Vec<Json>), JObj(HashMap<String, Json>) }
```

— that parses arbitrarily nested input, traverses it **by reference**, round-trips back to canonical text through a `Display` serializer, and runs **leak-free under a constant-memory smoke test** (JIT + AOT). The **theme** is *make the heap recursive*: close the recursive heap-owning-enum soundness gaps (read-without-move, match-through-reference, sound recursive `Drop` + `Clone`, enum-typed fields), then dress the surface for real programs (`%`/`&&`, `Result` errors, `Display` formatting, de-`i64`'d iteration), and prove it end to end by promoting the v5 SUBSET to a full recursive tree.

| Phase | Goal | Status |
|-------|------|--------|
| 33 | **Container interior drop + rehash reclaim + the missing `%`/`&&` operators** | ✅ The HashMap/HashSet `emitDropGlue` arm now walks occupied buckets and drops every live key+value before freeing the array (generalizing the Vec template), and rehash-grow frees the **old** bucket buffer. `%` modulo (lexer `Percent` → `*`-tier → LLVM `SRem`) and short-circuit `&&` (two-char `AmpAmp` → branch IR) ship. Verified: a `HashMap<String, Vec<i64>>` built/dropped in a 2 M-iteration loop holds flat peak-RSS; `%`/`&&` green JIT + AOT (`smoke_test_phase33`). *(Only `%`/`&&` — `\|\|` is the zero-param-closure token.)* |
| 34 | **Read-without-move: borrow into containers + `match &x` / `*r`** | ✅ `vec_get_ref(&v,i) -> &T` / `hashmap_get_ref(&m,k) -> Option<&V>` return a **borrow**; a `*r` deref operator; and `match &Enum` binds payload sub-patterns **as references** (one-layer auto-deref). Verified: a `Vec<String>` / a `Vec` of a heap-owning enum read & `match &`'d element-by-element, originals drop exactly once, constant memory JIT + AOT (`smoke_test_refread`). Two pre-existing latent bugs fixed en route: a String-struct double-layout, and a per-iteration loop-binding stack overflow (alloca hoisting). *The keystone.* |
| 35 | **Sound recursive heap enums + `clone`** | ✅ A self-recursive type (`Vec<Self>`/`HashMap<K,Self>`/`Box<Self>` payloads) lays out (a `resolveInInstance` cycle guard) and drops via a per-type **drop function** that recurses at run time. `clone<T>(&T) -> T` is a deep-copy intrinsic over scalars/String/Vec/HashMap/Box and user structs/enums incl. recursive types (subsumes the planned trait+hand-written-impls since generic `impl<T>` blocks stay deferred — the intrinsic is strictly more capable). **Fixed a serious pre-existing miscompile**: the optimizer ran without the target datalayout, folding multi-field-aggregate GEPs to wrong offsets (a value read through a pointer returned garbage at -O1+). Verified: a 1000-node-deep `Tree` cloned + dropped, and a 200k build+clone+match+drop loop flat at ~1.6 MB (`smoke_test_clone`). |
| 36 | **Enum-typed struct fields + non-Copy tuples + tuple `match` + `Result` errors** | ✅ Struct/enum layout split into name + body passes so a struct field of enum type (and vice-versa) resolves; `&place` borrows (`match &h.t`). Tuples hold non-Copy elements (`(String, i64)`, `(Json, i64)`) with drop + move-tracking (arrays stay Copy-only); tuple `match` patterns across parser → typecheck → the match compiler (irrefutable single-constructor) → codegen; `Result<_, ParseError>` via the shipped `?`. Verified: recursive-enum field read, `(String,i64)` returned + destructured (dropped once), tuple/Result match — JIT + AOT, 300k loop flat (`smoke_test_phase36`). |
| 37 | **`Display`/`to_string` + de-`i64`'d generic iteration & slices** | ✅ A prelude `Display` trait (`to_string(&self) -> String`) with built-in impls for i64/bool/String (user types hand-write `impl Display`); `&[T]` slices generic per element T (`slice_len`/`slice_get`/`slice_get_ref`), non-owning. `checkFor` already bound the loop variable to the iterator's real `next()` payload — verified with an `Iterator<String>`. Verified: a bounded `show<T: Display>`, a recursive `impl Display for Json` serializing `[3,[1,22]]`, a `for s in <Iterator<String>>`, and a `&[String]` slice read — JIT + AOT (`smoke_test_phase37`). *(Static dispatch only; `dyn Display` deferred.)* |
| 38 | **Capstone: FULL nested-JSON (parse + serialize, leak-free) + doc-truth fixup** | ✅ `examples/json/` is the full recursive `enum Json { … JArr(Vec<Json>), JObj(HashMap<String, Json>) }`: a `&mut self` recursive-descent parser, a recursive `Display` serializer, and an order-independent deep-equality round trip — all in kardashev. Added `hashmap_keys` (enumerate a map) and fixed three by-value-consumer leaks (`string_push_str`, `hashmap_get`/`get_ref` now free what they consume). Corrected the stale closure-env "not freed today" comments. Verified: parses `{"a":[1,{"b":true},"x"],"n":null,"k":-42}`, round-trips, and a 200k parse+serialize+drop loop flat at ~1.6 MB, JIT + AOT, wired into `make test` (`smoke_test_json`). **The north star — reached.** |

**Landing order — 33 → 34 → 35 → 36 → 37 → 38 — along a strict dependency arc.** 33 established the interior-drop + constant-memory leak-probe discipline on the proven Vec template and folded in the two cost-free operators. 34 was the **keystone**: read/`match`-by-reference is the root-cause fix for the v5 subset, so it preceded everything touching recursive data. 35 made the recursive value itself a sound droppable/clonable heap citizen. 36 gave the AST node its *shape* (enum-typed fields, non-Copy tuples). 37 supplied the `Display` serializer backbone and de-`i64`'d iteration. 38 integrated all five into the full-nested-JSON capstone — the integration proof that surfaced (and closed) every latent leak/UAF under its constant-memory gate. Each phase shipped fully green before the next built on it, exactly as v1–v5 did.

**Documented-deferred (carried forward unchanged, never stubbed):** third-party dependency resolution via the **Bazel module registry** (Bazel cannot run in this build environment — `MODULE.bazel`-only, no `WORKSPACE`; `mod foo;` + `kard.toml` local-path deps are what ship); **macOS/kqueue** async fd-readiness (Linux/`epoll` only; timers work cross-platform and CI covers macOS); the v5 **async-frame interior free** + **all droppable async-fn locals** (the `inAsyncFn_` early-return at codegen.cpp:6279 — async is off the v6 north-star path, so the known leak/no-UAF stays rather than a risky executor-lifecycle half-fix). **Off-lens, still open** (honestly not in v6 scope): full **`Send`/`Sync`** marker traits + channels + atomics (the by-ref-capture rejection at typecheck.cpp:4841 remains the enforced floor); **generic `dyn Trait<T>`**, generic `impl<T>` blocks, associated-type bounds/defaults (Phase 21); **const-generic type params** + non-i64/bool const values (Phase 25); `extern "C"` **export** (Phase 24); full **`&mut` auto-reborrow** through recursive calls; **`#[derive]`** machinery; cross-file LSP rename; `pub` enforcement beyond functions; real module namespacing. Anything verifiable only under Bazel or macOS is flagged, not pretended-done.

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.

## License

Licensed under either of

 * Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
   <http://www.apache.org/licenses/LICENSE-2.0>)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) or
   <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in the work by you, as defined in the Apache-2.0 license, shall be
dual licensed as above, without any additional terms or conditions.
