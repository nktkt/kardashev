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
