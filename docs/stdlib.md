# Standard Library

What ships with `kardc` today. Everything here is built into the
compiler — no external crate / module is imported. The prelude declarations (`Option`, `Result`, the
`Iterator` / `Hash` / `Eq` traits, the combinators, the file-I/O
wrappers) are prepended to your source automatically; the builtin
functions (`print`, `vec_*`, `hashmap_*`, `str_*`, …) are recognized
by the typechecker and lowered by codegen on demand. A program that
defines its own `Option` / `Iterator` / etc. suppresses the matching
prelude entry, so there's never a duplicate-declaration error.

> One reminder that governs every snippet below. `if` is an
> **expression and requires an `else`** (`if c { … } else { … }`, never
> a bare `if c { … }`). The short-circuit `&&` / `||` operators are
> available, and `&` of a literal or temporary works (`&5`, `&Foo { .. }`)
> — it materializes a statement-scoped, dropped slot.

## Prelude (auto-included)

### `Option<T>` and `Result<T, E>`

```rust
enum Option<T> { Some(T), None }
enum Result<T, E> { Ok(T), Err(E) }
```

Available in every program. `Result<T, E>` drives the `?` operator
(early-returns `Err` on failure). Tests that call `kardashev::parse`
directly bypass the driver and so don't get the prelude — they declare
these themselves.

#### Combinators

Effect-row-polymorphic library functions over an `i64` payload (the
shipped MVP shape). Each lowers like an ordinary generic `fn`, so a
combinator inherits its closure's effects — passing an `io` closure to
`option_map` makes the call site `io`.

```rust
fn option_map(o: Option<i64>, f: fn(i64) -> i64 ! {e}) -> Option<i64> ! {e}
fn option_unwrap_or(o: Option<i64>, default: i64) -> i64
fn option_and_then(o: Option<i64>, f: fn(i64) -> Option<i64> ! {e}) -> Option<i64> ! {e}
fn result_map(r: Result<i64, i64>, f: fn(i64) -> i64 ! {e}) -> Result<i64, i64> ! {e}
fn result_unwrap_or(r: Result<i64, i64>, default: i64) -> i64
```

```rust
fn main() -> i64 {
    let o = Some(20);
    let inc = |x| x + 1;
    option_unwrap_or(option_map(o, inc), 0)   // -> 21
}
```

### The `Iterator<T>` trait

```rust
trait Iterator<T> { fn next(&mut self) -> Option<T>; }
```

`for x in it` desugars through `next()` for any impl, and the
`fold` / `map` / `filter` adaptors are generic over `T`. The built-in
`Range` (below) ships an `impl Iterator<i64> for Range`; user types may
`impl Iterator<bool>`, etc.

```rust
fn main() -> i64 ! { io } {
    let mut total = 0;
    for x in 0..5 { total = total + x; }   // Range as an Iterator<i64>
    print(total);                          // -> "10"
    0
}
```

### The `Hash` and `Eq` traits

```rust
trait Hash { fn hash(&self) -> i64; }
trait Eq   { fn eq(&self, other: &Self) -> bool; }
```

Built-in impls ship for `i64` (identity hash, scalar equality) and
`String` (FNV-1a hash, byte-exact equality). These are what make a
`HashMap` key / `HashSet` element type pluggable: bound a generic on
`K: Hash + Eq` (multiple bounds per parameter shipped in Phase 28, both
inline and via `where`) and any user type with both impls becomes a
usable key.

```rust
struct Id { v: i64 }
impl Hash for Id { fn hash(&self) -> i64 { self.v } }
impl Eq   for Id { fn eq(&self, other: &Id) -> bool { self.v == other.v } }
```

## I/O and strings

A `String` is a `{ ptr, len, cap }` heap value with **no NUL
terminator**. String literals are backed in place (cap 0); `string_new`
/ `str_substring` / `int_to_string` produce fresh heap Strings. The
`&String` borrow forms feed every read-only string op.

### Printing

```rust
fn print(n: i64) -> i64 ! { io }            // one i64 + newline
fn print_str(s: &String) -> i64 ! { io }    // string + newline
fn print_string(s: &String) -> i64 ! { io } // alias of print_str
fn println(s: &String) -> i64 ! { io }      // string + newline (conventional name)
fn print_no_nl(s: &String) -> i64 ! { io }  // string with NO trailing newline
```

`print`, `print_str`, `print_string`, and `println` all force a
trailing newline; `print_no_nl` is the one that does not, so you can
compose a line piece by piece. All four return 0 and require the caller
to declare `io`.

```rust
fn main() -> i64 ! { io } {
    let greeting = "hello, kardashev";
    print_no_nl(&greeting);   // no newline
    print(42);                // "42\n"
    0
}
```

### Inspecting and building strings

```rust
fn str_len(s: &String) -> i64
fn str_char_at(s: &String, i: i64) -> i64               // the byte at index i (0..len)
fn str_eq(a: &String, b: &String) -> bool               // byte-exact (length + contents)
fn str_substring(s: &String, start: i64, len: i64) -> String ! { alloc }  // start/len clamped into bounds
fn int_to_string(n: i64) -> String ! { alloc }          // decimal, via snprintf "%lld"

fn string_new() -> String ! { alloc }
fn string_push_str(s: &mut String, other: String) -> i64 ! { alloc }  // takes a String literal directly
fn string_len(s: &String) -> i64
```

`str_substring` **clamps** `start` and `len` into bounds rather than
panicking. `string_push_str` accepts a literal as the second argument
(`string_push_str(&mut s, "cd")`).

```rust
fn main() -> i64 ! { io, alloc } {
    let mut s = string_new();
    string_push_str(&mut s, "ab");
    string_push_str(&mut s, "cd");
    print(string_len(&s));         // "4"
    let n = int_to_string(123);
    print_str(&n);                 // "123"
    0
}
```

## Containers

### `Vec<T>`

A growable, heap-backed buffer, specialized per element type `T` (the
codegen sizes the stride from the `DataLayout`, so `i64`, `bool`,
structs, and enums all work). `malloc` / `realloc` with capacity
doubling.

```rust
fn vec_new() -> Vec<T> ! { alloc }
fn vec_push(v: &mut Vec<T>, x: T) -> i64 ! { alloc }   // appends, returns 0
fn vec_get(v: &Vec<T>, i: i64) -> T                     // bounds-checked (see below)
fn vec_len(v: &Vec<T>) -> i64
```

`vec_get` is bounds-checked but **does not panic**: a negative index,
an index `>= len`, or an empty buffer yields a type-correct zero/null
value of `T` instead of an out-of-bounds load. (It is fixed-size array
indexing — `arr[i]` — that OOB-*panics*, since Phase 23; `vec_get`
predates that and keeps its safe-zero behavior.) `vec_get` returns a
**shallow copy** of the element.

```rust
fn main() -> i64 ! { alloc, io } {
    let mut v = vec_new();
    vec_push(&mut v, 10);
    vec_push(&mut v, 20);
    print(vec_get(&v, 1));   // "20"
    print(vec_len(&v));      // "2"
    0
}
```

### `HashMap<K, V>`

Open-addressing (linear-probing) map with rehash on growth, generic
over **both** key and value (the headline v5 feature). `K` may be a
built-in `i64` (inline identity-hash + `icmp`) or `String` (FNV-1a +
`str_eq`), or any user type with `impl Hash` + `impl Eq`. A
non-hashable key type is a clear compile error.

```rust
fn hashmap_new() -> HashMap<K, V> ! { alloc }
fn hashmap_insert(m: &mut HashMap<K, V>, k: K, v: V) -> i64 ! { alloc }
fn hashmap_get(m: &HashMap<K, V>, k: K) -> Option<V>
fn hashmap_len(m: &HashMap<K, V>) -> i64
```

```rust
fn main() -> i64 ! { alloc, io } {
    let mut m = hashmap_new();
    let key = "answer";
    hashmap_insert(&mut m, key, 42);   // HashMap<String, i64>
    let lookup = "answer";
    match hashmap_get(&m, lookup) {
        Some(v) => print(v),           // "42"
        None    => print(0 - 1),
    };
    0
}
```

### `HashSet<T>`

A set over a hashable element `T` (same key requirement as
`HashMap`; codegen reuses the map table with a dummy value).

```rust
fn hashset_new() -> HashSet<T> ! { alloc }
fn hashset_insert(s: &mut HashSet<T>, k: T) -> i64 ! { alloc }
fn hashset_contains(s: &HashSet<T>, k: T) -> bool
fn hashset_len(s: &HashSet<T>) -> i64
```

```rust
fn main() -> i64 ! { alloc, io } {
    let mut s = hashset_new();
    hashset_insert(&mut s, 7);
    let present = if hashset_contains(&s, 7) { 1 } else { 0 };
    print(present);    // "1"
    0
}
```

### `&[T]` slices

`&v[a..b]` produces a fat-pointer slice (`{ ptr, len }`) over an
existing buffer — constructing one does **not** allocate. The element
type is `i64` in this MVP.

```rust
fn slice_len(s: &[i64]) -> i64
fn slice_get(s: &[i64], i: i64) -> i64
```

### `Box<T>` and `Range`

`Box<T>` is a heap-owning pointer (also the payload of
`Box<dyn Trait>` trait objects). `Range` is the first-class iterable
produced by `a..b` (half-open) and `a..=b` (inclusive); it impls
`Iterator<i64>` and powers `for x in a..b`.

## File I/O and CLI args

These are added lazily — the file-I/O / argv runtime is emitted only
when your source actually mentions one of these names, so an
I/O-free program carries none of the machinery.

```rust
enum IoError { IoNotFound, IoPermissionDenied, IoOther }

fn fs_read_to_string(path: &String) -> Result<String, IoError> ! { io, alloc }
fn fs_write(path: &String, contents: &String) -> Result<i64, IoError> ! { io }
fn fs_exists(path: &String) -> bool ! { io }

fn args() -> Vec<String> ! { alloc }   // process argv (argv[0] included)
fn arg_count() -> i64
fn arg_get(i: i64) -> String           // a borrowed view of argv[i] (cap 0)
```

Errors are classified **portably** via `access()` probes (there is no
reliance on a libc `errno` symbol), so the IR links on both Linux and
macOS. The variant names are `Io`-prefixed so the auto-injected enum
can't clash with a user's own `NotFound`. `args()` reflects real argv
in an **AOT** binary (the generated `int main(argc, argv)` captures it);
under the JIT there is no process argv, so it is **empty**.

```rust
fn main() -> i64 ! { io, alloc } {
    let path = "config.txt";
    match fs_read_to_string(&path) {
        Ok(s)  => print(str_len(&s)),
        Err(e) => match e {
            IoNotFound         => print(0 - 1),
            IoPermissionDenied => print(0 - 2),
            IoOther            => print(0 - 3),
        },
    };
    0
}
```

## Documented deferrals (honest, not stubbed)

Carried forward unchanged, exactly as the README records — these are
real, known gaps, never silent stubs:

- **HashMap / HashSet interior keys & values are not individually
  dropped.** Dropping a map/set frees its bucket array (no UAF), but a
  droppable `K` or `V` stored inside the table leaks — a documented
  leak in the same class as the closure-env / `match`-payload leaks
  that Phase 29 *did* close. Plain `Vec` / `String` / `Box` and
  droppable `Vec` *elements*, user-struct fields, closure-env captures,
  and `match`-payload bindings all drop deterministically.
- **Async-frame interior free is deferred.** A completed `Future`'s
  heap frame is reclaimed by neither `block_on` nor the executor, so a
  long-running async workload leaks frames (a bounded one-shot does
  not). Freeing it safely needs reworking the executor task lifecycle
  (read-after-free risk on the poll slot), and async is off the v5
  capstone path, so it stays a known leak rather than a risky half-fix.
- **macOS/kqueue async fd-readiness** is deferred — Linux/`epoll` only
  here; timers (`sleep_ms`) work cross-platform, and CI covers macOS.
- **Third-party dependency resolution via the Bazel module registry**
  is deferred — `mod foo;` + `kard.toml` local-path deps are what ship.

The v5 JSON capstone (`examples/json/`) is, by design, a **numeric
config subset**: it parses a top-level object with integer values into
a `HashMap<String, i64>` — the sound shape that exercises the
String-keyed map end to end. Nested / string / bool values are out of
scope. `examples/kdlex/` lexes a kardashev subset into a `Vec<Tok>`.
