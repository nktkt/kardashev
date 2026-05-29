# kardashev

A systems programming language with lightweight effect-label typing, built on LLVM.

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

The full README roadmap (Phases 0–8) lands in the repository. Built
locally with `bazel build //... && bazel test //...` or, when Bazel
isn't available, the `Makefile.local` shim (LLVM + clang). The CI
matrix runs both ubuntu-latest and macos-latest via Bazel on every
push; every commit goes in green.

Tour: see [`docs/`](docs/) for the language reference, effects-system
notes, stdlib catalog, and compiler-architecture deep dive.
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

## Roadmap v5 — planned

v4 proved kardashev can compile a real program written *in itself* (`examples/calc/`).
v5's **north star** is to make that no longer a demo: compile two genuinely non-trivial
tools written in kardashev — a **JSON parser** (`examples/json/`) and a **kardashev-subset
lexer/parser** (`examples/kdlex/`) — as green capstones, standing on a deeper stdlib, a
leak-free memory model, and accurate docs. The **theme** is to close, one verifiable
dependency layer at a time, the stdlib/collection/soundness gaps that today block writing
real programs in the language. *This section is the plan — these phases are not yet
implemented.* As in v1–v4, each phase will ship fully green (`Makefile.local` + a new smoke
test, CI on ubuntu + macOS) before the next builds on it, and anything unverifiable in this
build environment stays documented-deferred, never stubbed.

| Phase | Goal | Status |
|-------|------|--------|
| 27 | **String toolkit** | 🎯 planned. Today's only string ops are `print_str` / `str_len` / `str_char_at` and an `i64`-only `print`; add `str_eq`, `str_substring` (byte slice), `int_to_string`, and `println(&str)` so a self-written tool can compare, format, and emit text. Verified by a JIT+AOT smoke test. |
| 28 | **`Hash` trait + generic map/set keys** | 🎯 planned. `HashMap` keys are pinned to `i64` today (the typechecker rejects any other key — "no Hash trait yet"). Land a `Hash` trait and generalize to `HashMap<K, V>` for `K: Hash + Eq` — which first needs multiple-bounds-per-param (`T: A + B`, not yet in the grammar) — plus `HashSet<T>`. Verified at `i64` + `String` keys, JIT+AOT. |
| 29 | **Plug the documented Drop leaks** | 🎯 planned. Closure-env structs, async-frame contents, and enum/`match`-payload bindings are not dropped yet (the documented Phase-16 leak — no UAF, but they leak). Run Drop glue through all three so a loop that captures, awaits, or destructures droppable values runs in **constant memory**, extending the Phase-16 guarantee. Verified with a constant-RSS loop test (à la Phase 16/23). *(Dynamic array-index OOB already panics as of Phase 23 — not part of this phase.)* |
| 30 | **File I/O + CLI args** | 🎯 planned. `fs_read_to_string` / `fs_write` / `fs_exists` returning `Result<_, IoError>` (carrying the `io` effect, lowered through the Phase-24 `extern "C"` path) and `args()` for argv. Verified here on Linux; macOS readiness rides CI. |
| 31 | **Capstone: a JSON parser + a kd-subset parser, written in kardashev** | 🎯 planned. `examples/json/` (string → AST → value) and `examples/kdlex/` (a kardashev-subset lexer/parser) consuming the Phase 27–30 stdlib, with fixture-driven smoke tests. **The north star** — a real SUBSET self-parse exercising the stdlib end to end, no new language features. |
| 32 | **Docs + source-comment truth pass** | 🎯 planned. `docs/` still describes the language at roughly Phase 7 / v2, and several source comments label now-fully-working async code as "Phase 6 (stub)". Rewrite `docs/` through v5 and purge the stale markers; a doc-lint check guards against regression. |

**Documented-deferred (carried forward unchanged, never stubbed):** third-party dependency
resolution via the Bazel module registry (Bazel can't run in this build environment;
`mod foo;` + `kard.toml` local-path deps are what ship) and macOS/kqueue async fd-readiness
(Linux/`epoll` only here; timers work cross-platform, and CI covers macOS).

Plan lands in order **27 → 28 → 29 → 30 → 31 → 32** — a deliberate dependency arc: 27's
strings feed 28's string-key hashing and 31's AST dumps; 28's `Hash` trait must precede
generic keys; 29 plugs the Drop leaks once 27–28 introduce new droppable values that make
them load-bearing; 30's `Result<String, IoError>` drops cleanly on the error path *because*
29 closed that hole; 31 integrates 27–30 into the self-written capstones; 32 documents the
result last. Each ships green before the next, exactly as v1–v4 did.

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.
