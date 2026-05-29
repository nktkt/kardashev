// kardc — kardashev V1 driver.
//
// Three modes:
//   1. Interactive REPL (default, when no argv[1] is given):
//        - Read a line; if it starts with `fn `, append to an accumulator
//          and re-typecheck the whole accumulator. On success, the line
//          becomes part of the available environment for subsequent
//          expressions.
//        - Otherwise treat the line as an expression: wrap it in a
//          synthesized `fn __eval() -> i64 { <expr> }`, codegen + JIT
//          the accumulator-plus-wrapper as a single module, look up
//          `__eval`, call it, print its return value.
//        - Each evaluation builds a fresh LLJIT — V1 prioritizes
//          simplicity over speed.
//
//   2. File mode (kardc <path.kd>):
//        - Compile the whole file, JIT-run `main()`, print its result.
//
//   3. AOT mode (kardc -o <out> <path.kd>) — Phase 5a:
//        - Compile to a native object, invoke the system clang to link it
//          into an executable at <out>. The user's `main()` (returning i64)
//          is renamed to `__kd_main` and a C-compatible `int main()`
//          wrapper is generated that returns the kardashev result
//          truncated to a process exit code.
//
// The REPL prompt is only emitted when stdin is a TTY, so piped input
// works cleanly (smoke_test exercises this).

#include "kardashev/borrow_check.hpp"
#include "kardashev/codegen.hpp"
#include "kardashev/parser.hpp"
#include "kardashev/typecheck.hpp"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Error.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h> // isatty
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Phase 5b: prelude — `Option<T>` and `Result<T, E>` are auto-defined
// for kardc users so they don't have to redeclare them in every
// program. We prepend the prelude only for declarations the user
// hasn't already supplied; the grep-based check is intentional, since
// the typechecker keeps variant names globally unique and we'd
// otherwise force a duplicate-decl error on programs that declare
// their own Option / Result. Tests that call `kardashev::parse`
// directly bypass this and keep their existing self-contained
// fixtures.
std::string applyPrelude(const std::string& userSrc) {
    std::string prelude;
    if (userSrc.find("enum Option") == std::string::npos) {
        prelude += "enum Option<T> { Some(T), None }\n";
    }
    if (userSrc.find("enum Result") == std::string::npos) {
        prelude += "enum Result<T, E> { Ok(T), Err(E) }\n";
    }
    // Phase 13a / 21a: the generic `Iterator<T>` trait + its impl for the
    // built-in `Range`. Phase 21a migrated the element type from a hardcoded
    // i64 to a trait type parameter `T`; `Range` impls `Iterator<i64>` (its
    // element is still i64), so `for x in <range>` and the adaptors keep
    // working byte-for-byte, while user types can now `impl Iterator<bool>`
    // etc. `next` returns `Option<T>`; the impl supplies T=i64 and writes
    // `Option<i64>` directly. `for` over literal ranges keeps its Phase 9 fast
    // path in codegen, so this impl is only exercised when a Range is used as
    // a generic `Iterator`. Guard: a user who defines their own `Iterator`
    // (generic or not) suppresses the prelude one entirely.
    if (userSrc.find("trait Iterator") == std::string::npos) {
        prelude +=
            "trait Iterator<T> { fn next(&mut self) -> Option<T>; }\n"
            "impl Iterator<i64> for Range {\n"
            "    fn next(&mut self) -> Option<i64> {\n"
            "        if self.inclusive != 0 {\n"
            "            if self.start > self.end { None }\n"
            "            else { let v = self.start;"
            " self.start = self.start + 1; Some(v) }\n"
            "        } else {\n"
            "            if self.start >= self.end { None }\n"
            "            else { let v = self.start;"
            " self.start = self.start + 1; Some(v) }\n"
            "        }\n"
            "    }\n"
            "}\n";
    }
    // Phase 28: the `Hash` and `Eq` traits with built-in impls for i64 and
    // String, so user code can call `k.hash()` / `a.eq(&b)` and bound a
    // generic on them (`fn f<K: Hash + Eq>(...)`). The impl bodies forward to
    // pure builtins (identity / FNV-1a hash; scalar / byte-wise equality). The
    // generic `HashMap<K,V>` / `HashSet<T>` container hashes i64 and String
    // keys directly via the same primitives and dispatches user key types
    // through their `Hash`/`Eq` impls — so these traits are what make a key
    // type pluggable. Each guarded so a user-defined `Hash`/`Eq` wins without a
    // duplicate-decl error.
    if (userSrc.find("trait Hash") == std::string::npos) {
        prelude +=
            "trait Hash { fn hash(&self) -> i64; }\n"
            "impl Hash for i64 { fn hash(&self) -> i64 { hash_i64(self) } }\n"
            "impl Hash for String"
            " { fn hash(&self) -> i64 { hash_string(self) } }\n"
            // Phase 48: bool / f64 Hash, so #[derive(Hash)] covers those fields.
            // bool -> 0/1; f64 -> hash of its truncated-to-i64 value (lossy but
            // valid — Eq disambiguates collisions; float map keys are rare).
            "impl Hash for bool"
            " { fn hash(&self) -> i64 { if *self { 1 } else { 0 } } }\n"
            "impl Hash for f64"
            " { fn hash(&self) -> i64 { let v = float_to_int(*self); hash_i64(&v) } }\n";
    }
    if (userSrc.find("trait Eq") == std::string::npos) {
        prelude +=
            "trait Eq { fn eq(&self, other: &Self) -> bool; }\n"
            "impl Eq for i64"
            " { fn eq(&self, other: &i64) -> bool { int_eq(self, other) } }\n"
            "impl Eq for String"
            " { fn eq(&self, other: &String) -> bool"
            " { str_eq(self, other) } }\n"
            // Phase 44: bool / f64 Eq (scalars). (A generic HashMap Eq trait
            // impl is deferred for the same reason as HashMap Clone above; a
            // hand-written order-independent compare is straightforward where
            // needed.)
            "impl Eq for bool"
            " { fn eq(&self, other: &bool) -> bool"
            " { if *self { *other } else { !*other } } }\n"
            "impl Eq for f64"
            " { fn eq(&self, other: &f64) -> bool { *self == *other } }\n";
    }
    // Phase 37: the `Display` trait — `to_string(&self) -> String` — with
    // built-in impls for the scalar/heap primitives, so a generic can be
    // bounded `<T: Display>` and uniformly format i64 / bool / String, and a
    // user type formats by hand-writing `impl Display for T`. Static dispatch
    // only (a `dyn Display` needs the still-open generic-`dyn Trait<T>` work).
    // The impls forward to existing builtins: i64 -> int_to_string, String ->
    // a deep clone (so the result is owned), bool -> a literal. Method bodies
    // are codegen'd only on use, so declaring these costs unused programs
    // nothing. Guarded so a user-defined `Display` wins.
    if (userSrc.find("trait Display") == std::string::npos) {
        prelude +=
            "trait Display { fn to_string(&self) -> String; }\n"
            "impl Display for i64"
            " { fn to_string(&self) -> String ! { alloc }"
            " { int_to_string(*self) } }\n"
            "impl Display for String"
            " { fn to_string(&self) -> String ! { alloc }"
            " { clone(self) } }\n"
            "impl Display for bool"
            " { fn to_string(&self) -> String"
            " { if *self { \"true\" } else { \"false\" } } }\n";
    }
    // Phase 41: the `Clone` trait — `clone(&self) -> Self` — a DEEP copy that
    // dispatches through each element's own impl. Built-in impls for scalars +
    // String (String copies via str_substring); a generic `impl<T: Clone> Clone
    // for Vec<T>` clones element-wise via the now-working generic-impl + nested
    // bounded-generic dispatch (Phase 40). The `clone(&x)` intrinsic (Phase 35)
    // remains the structural workhorse / fallback for types with no impl. (The
    // method is `x.clone()`; the intrinsic is the free call `clone(&x)` — they
    // coexist.) Guarded so a user-defined `Clone` wins.
    if (userSrc.find("trait Clone") == std::string::npos) {
        prelude +=
            "trait Clone { fn clone(&self) -> Self ! { alloc }; }\n"
            "impl Clone for i64"
            " { fn clone(&self) -> i64 ! { alloc } { *self } }\n"
            "impl Clone for bool"
            " { fn clone(&self) -> bool ! { alloc } { *self } }\n"
            "impl Clone for String { fn clone(&self) -> String ! { alloc }"
            " { str_substring(self, 0, str_len(self)) } }\n"
            "impl<T: Clone> Clone for Vec<T> {\n"
            "    fn clone(&self) -> Vec<T> ! { alloc } {\n"
            "        let mut out = vec_new();\n"
            "        let mut i = 0;\n"
            "        while i < vec_len(self) {\n"
            "            vec_push(&mut out, vec_get_ref(self, i).clone());\n"
            "            i = i + 1;\n"
            "        }\n"
            "        out\n"
            "    }\n"
            "}\n"
            "impl Clone for f64"
            " { fn clone(&self) -> f64 ! { alloc } { *self } }\n"
            // Phase 46 (v8): the generic HashMap Clone trait impl — the v7
            // deferral, unblocked by Phase 45 (a `K: Hash + Eq` bound now
            // satisfies the container key gate from inside a generic body). A
            // deep copy: enumerate keys (bucket-order), clone each key + value
            // through their own Clone impls, rebuild. Requires K: Clone too
            // (the keys are copied into the new map). This makes `#[derive(Clone)]`
            // apply to a struct/enum with a HashMap field, and `map.clone()`
            // dispatch through the trait. (The `clone(&x)` intrinsic still
            // deep-clones structurally as the no-impl fallback.)
            "impl<K: Hash + Eq + Clone, V: Clone> Clone for HashMap<K, V> {\n"
            "    fn clone(&self) -> HashMap<K, V> ! { alloc } {\n"
            "        let mut out = hashmap_new();\n"
            "        let ks = hashmap_keys(self);\n"
            "        let mut i = 0;\n"
            "        while i < vec_len(&ks) {\n"
            "            let kref = vec_get_ref(&ks, i);\n"
            "            match hashmap_get_ref(self, kref.clone()) {\n"
            "                Some(vref) => { hashmap_insert(&mut out, kref.clone(), vref.clone()); },\n"
            "                None => {},\n"
            "            }\n"
            "            i = i + 1;\n"
            "        }\n"
            "        out\n"
            "    }\n"
            "}\n"
            // Phase 51 (v9): trait `Clone` for `Box<T>` — the v8 deferral, now
            // that Box is a registrable impl target and `**self` / `&*` deref
            // ergonomics work. Deep-clones the boxed value through T's own
            // Clone, so `#[derive(Clone)]` covers a Box<T> field (recursive
            // types) without falling back to the structural intrinsic.
            "impl<T: Clone> Clone for Box<T> {\n"
            "    fn clone(&self) -> Box<T> ! { alloc } { Box::new((**self).clone()) }\n"
            "}\n";
    }
    // Phase 41: a generic `impl<T: Eq> Eq for Vec<T>` (the `Eq` trait + its
    // i64/String impls are injected below) — element-wise deep equality, the
    // order-sensitive comparison for sequences. Guarded together with `Eq`.
    if (userSrc.find("trait Eq") == std::string::npos) {
        prelude +=
            "impl<T: Eq> Eq for Vec<T> {\n"
            "    fn eq(&self, other: &Vec<T>) -> bool {\n"
            "        if vec_len(self) != vec_len(other) { false }\n"
            "        else {\n"
            "            let mut i = 0;\n"
            "            let mut same = true;\n"
            "            while i < vec_len(self) {\n"
            "                if !vec_get_ref(self, i).eq(vec_get_ref(other, i))"
            " { same = false; } else {}\n"
            "                i = i + 1;\n"
            "            }\n"
            "            same\n"
            "        }\n"
            "    }\n"
            "}\n"
            // Phase 46 (v8): the generic HashMap Eq trait impl — order-
            // INDEPENDENT key-set + per-key value equality (the right notion
            // for a map). Same length, then every key of `self` must be present
            // in `other` with an equal value. The body allocates (it enumerates
            // keys via hashmap_keys and clones keys for the by-value lookups), so
            // the method is declared `! { alloc }` even though the `Eq` trait's
            // `eq` is pure — a trait-impl method may carry MORE effects than the
            // trait declares; static dispatch attributes the impl's real effects
            // at the call site. Requires K: Clone (keys are cloned per lookup).
            "impl<K: Hash + Eq + Clone, V: Eq> Eq for HashMap<K, V> {\n"
            "    fn eq(&self, other: &HashMap<K, V>) -> bool ! { alloc } {\n"
            "        if hashmap_len(self) != hashmap_len(other) { false }\n"
            "        else {\n"
            "            let ks = hashmap_keys(self);\n"
            "            let mut i = 0;\n"
            "            let mut same = true;\n"
            "            while i < vec_len(&ks) {\n"
            "                let kref = vec_get_ref(&ks, i);\n"
            "                match hashmap_get_ref(self, kref.clone()) {\n"
            "                    Some(sv) => match hashmap_get_ref(other, kref.clone()) {\n"
            "                        Some(ov) => { if !sv.eq(ov) { same = false; } else {} },\n"
            "                        None => { same = false; },\n"
            "                    },\n"
            "                    None => {},\n"
            "                }\n"
            "                i = i + 1;\n"
            "            }\n"
            "            same\n"
            "        }\n"
            "    }\n"
            "}\n"
            // Phase 51 (v9): trait `Eq` for `Box<T>` — deep-compares the boxed
            // values through T's own Eq (`&(**other)` is the `&*` deref that now
            // lowers to the box pointer). Lets `#[derive(Eq)]` cover a Box field.
            "impl<T: Eq> Eq for Box<T> {\n"
            "    fn eq(&self, other: &Box<T>) -> bool { (**self).eq(&(**other)) }\n"
            "}\n";
    }
    // Phase 47 (v8): the `Ord` trait — `cmp(&self, &Self) -> i64` returning
    // -1 / 0 / 1 (less / equal / greater) — with built-in impls for the
    // ordered primitives (i64, f64, String; String is byte-wise lexicographic
    // over str_char_at), plus a generic in-place `sort<T: Ord>(v: &mut Vec<T>)`
    // (insertion sort over vec_swap — the first stdlib algorithm written over a
    // user trait bound). All pure (no alloc): cmp reads, vec_swap exchanges
    // slots. Guarded so a user-defined `Ord` wins.
    if (userSrc.find("trait Ord") == std::string::npos) {
        prelude +=
            "trait Ord { fn cmp(&self, other: &Self) -> i64; }\n"
            "impl Ord for i64 { fn cmp(&self, other: &i64) -> i64 {\n"
            "    if *self < *other { 0 - 1 } else { if *self > *other { 1 } else { 0 } } } }\n"
            "impl Ord for f64 { fn cmp(&self, other: &f64) -> i64 {\n"
            "    if *self < *other { 0 - 1 } else { if *self > *other { 1 } else { 0 } } } }\n"
            // String compare: byte-wise lexicographic. A shorter string that is
            // a prefix of the longer compares less. No allocation.
            "impl Ord for String { fn cmp(&self, other: &String) -> i64 {\n"
            "    let la = str_len(self);\n"
            "    let lb = str_len(other);\n"
            "    let mut i = 0;\n"
            "    let mut res = 0;\n"
            "    let mut go = true;\n"
            "    while go {\n"
            "        if i >= la {\n"
            "            if i >= lb { res = 0; } else { res = 0 - 1; }\n"
            "            go = false;\n"
            "        } else { if i >= lb { res = 1; go = false; }\n"
            "        else {\n"
            "            let ca = str_char_at(self, i);\n"
            "            let cb = str_char_at(other, i);\n"
            "            if ca < cb { res = 0 - 1; go = false; }\n"
            "            else { if ca > cb { res = 1; go = false; } else { i = i + 1; } }\n"
            "        } }\n"
            "    }\n"
            "    res\n"
            "} }\n"
            // Generic in-place sort: insertion sort, stable, O(n^2) — adequate
            // for the canonical-key-ordering the JSON capstone needs; swaps via
            // the ownership-neutral vec_swap so non-Copy T (e.g. String) is fine.
            "fn sort<T: Ord>(v: &mut Vec<T>) ! {} {\n"
            "    let n = vec_len(v);\n"
            "    let mut i = 1;\n"
            "    while i < n {\n"
            "        let mut j = i;\n"
            "        let mut go = true;\n"
            "        while go {\n"
            "            if j > 0 {\n"
            "                let c = vec_get_ref(v, j - 1).cmp(vec_get_ref(v, j));\n"
            "                if c > 0 { vec_swap(v, j - 1, j); j = j - 1; }\n"
            "                else { go = false; }\n"
            "            } else { go = false; }\n"
            "        }\n"
            "        i = i + 1;\n"
            "    }\n"
            "}\n";
    }
    // Phase 48 (v8): the `Default` trait — a STATIC (no-self) associated method
    // `default() -> Self`, called as `Type::default()`. Built-in impls for the
    // primitives + String + Vec<T>. Declared `! { alloc }` since constructing a
    // default may allocate (String/Vec); a derived struct default builds its
    // value field-wise (#[derive(Default)] — see deriveImplSource). Guarded so a
    // user-defined `Default` wins.
    if (userSrc.find("trait Default") == std::string::npos) {
        prelude +=
            "trait Default { fn default() -> Self ! { alloc }; }\n"
            "impl Default for i64 { fn default() -> i64 ! { alloc } { 0 } }\n"
            "impl Default for bool { fn default() -> bool ! { alloc } { false } }\n"
            "impl Default for f64 { fn default() -> f64 ! { alloc } { 0.0 } }\n"
            "impl Default for String { fn default() -> String ! { alloc } { \"\" } }\n"
            "impl<T> Default for Vec<T> { fn default() -> Vec<T> ! { alloc } { vec_new() } }\n";
    }
    // Phase 43: runtime string escape decode/encode for JSON-style strings,
    // written in kardashev over str_push_byte / str_char_at. `\\uXXXX` decodes
    // the Latin-1 subset (cp < 256); higher code points become '?' (documented).
    // Guarded so a user-defined `str_unescape` suppresses the whole block.
    if (userSrc.find("fn str_unescape") == std::string::npos) {
        prelude +=
            "fn __kd_hex_alpha(c: i64) -> i64 {\n"
            "    if c >= 97 { if c <= 102 { c - 87 } else { 0 } }\n"
            "    else { if c >= 65 { if c <= 70 { c - 55 } else { 0 } } else { 0 } }\n"
            "}\n"
            "fn __kd_hex_digit(c: i64) -> i64 {\n"
            "    if c >= 48 { if c <= 57 { c - 48 } else { __kd_hex_alpha(c) } }\n"
            "    else { __kd_hex_alpha(c) }\n"
            "}\n"
            "fn str_unescape(s: &String) -> String ! { alloc } {\n"
            "    let mut out = string_new();\n"
            "    let n = str_len(s);\n"
            "    let mut i = 0;\n"
            "    while i < n {\n"
            "        let c = str_char_at(s, i);\n"
            "        if c == 92 {\n"
            "            i = i + 1;\n"
            "            if i < n {\n"
            "                let d = str_char_at(s, i);\n"
            "                if d == 110 { str_push_byte(&mut out, 10); }\n"
            "                else if d == 116 { str_push_byte(&mut out, 9); }\n"
            "                else if d == 114 { str_push_byte(&mut out, 13); }\n"
            "                else if d == 34 { str_push_byte(&mut out, 34); }\n"
            "                else if d == 92 { str_push_byte(&mut out, 92); }\n"
            "                else if d == 47 { str_push_byte(&mut out, 47); }\n"
            "                else if d == 117 {\n"
            "                    let h0 = __kd_hex_digit(str_char_at(s, i + 1));\n"
            "                    let h1 = __kd_hex_digit(str_char_at(s, i + 2));\n"
            "                    let h2 = __kd_hex_digit(str_char_at(s, i + 3));\n"
            "                    let h3 = __kd_hex_digit(str_char_at(s, i + 4));\n"
            "                    let cp = ((h0 * 16 + h1) * 16 + h2) * 16 + h3;\n"
            "                    i = i + 4;\n"
            "                    if cp < 256 { str_push_byte(&mut out, cp); }\n"
            "                    else { str_push_byte(&mut out, 63); }\n"
            "                }\n"
            "                else { str_push_byte(&mut out, d); }\n"
            "                i = i + 1;\n"
            "            } else {}\n"
            "        } else {\n"
            "            str_push_byte(&mut out, c);\n"
            "            i = i + 1;\n"
            "        }\n"
            "    }\n"
            "    out\n"
            "}\n"
            "fn str_escape(s: &String) -> String ! { alloc } {\n"
            "    let mut out = string_new();\n"
            "    let n = str_len(s);\n"
            "    let mut i = 0;\n"
            "    while i < n {\n"
            "        let c = str_char_at(s, i);\n"
            "        if c == 10 { str_push_byte(&mut out, 92); str_push_byte(&mut out, 110); }\n"
            "        else if c == 9 { str_push_byte(&mut out, 92); str_push_byte(&mut out, 116); }\n"
            "        else if c == 13 { str_push_byte(&mut out, 92); str_push_byte(&mut out, 114); }\n"
            "        else if c == 34 { str_push_byte(&mut out, 92); str_push_byte(&mut out, 34); }\n"
            "        else if c == 92 { str_push_byte(&mut out, 92); str_push_byte(&mut out, 92); }\n"
            "        else { str_push_byte(&mut out, c); }\n"
            "        i = i + 1;\n"
            "    }\n"
            "    out\n"
            "}\n";
    }
    // Phase 54 (v9): string tokenizing, written in kardashev over str_char_at /
    // str_substring / str_len. `str_split` splits on a single byte (an empty
    // piece between adjacent separators is kept); `str_trim` strips leading and
    // trailing ASCII whitespace (space/tab/CR/LF). Guarded per-fn.
    if (userSrc.find("fn __kd_is_ws") == std::string::npos &&
        (userSrc.find("fn str_split") == std::string::npos ||
         userSrc.find("fn str_trim") == std::string::npos)) {
        prelude +=
            "fn __kd_is_ws(c: i64) -> bool {\n"
            "    if c == 32 { true } else { if c == 9 { true }"
            " else { if c == 10 { true } else { if c == 13 { true }"
            " else { false } } } }\n"
            "}\n";
    }
    if (userSrc.find("fn str_split") == std::string::npos) {
        prelude +=
            "fn str_split(s: &String, sep: i64) -> Vec<String> ! { alloc } {\n"
            "    let mut out = vec_new();\n"
            "    let n = str_len(s);\n"
            "    let mut start = 0;\n"
            "    let mut i = 0;\n"
            "    while i < n {\n"
            "        if str_char_at(s, i) == sep {\n"
            "            vec_push(&mut out, str_substring(s, start, i - start));\n"
            "            start = i + 1;\n"
            "        } else {}\n"
            "        i = i + 1;\n"
            "    }\n"
            "    vec_push(&mut out, str_substring(s, start, n - start));\n"
            "    out\n"
            "}\n";
    }
    if (userSrc.find("fn str_trim") == std::string::npos) {
        prelude +=
            "fn str_trim(s: &String) -> String ! { alloc } {\n"
            "    let n = str_len(s);\n"
            "    let mut a = 0;\n"
            "    let mut go = true;\n"
            "    while go {\n"
            "        if a >= n { go = false; }\n"
            "        else { if __kd_is_ws(str_char_at(s, a)) { a = a + 1; }"
            " else { go = false; } }\n"
            "    }\n"
            "    let mut b = n;\n"
            "    let mut g2 = true;\n"
            "    while g2 {\n"
            "        if b <= a { g2 = false; }\n"
            "        else { if __kd_is_ws(str_char_at(s, b - 1)) { b = b - 1; }"
            " else { g2 = false; } }\n"
            "    }\n"
            "    str_substring(s, a, b - a)\n"
            "}\n";
    }
    // Phase 30: file I/O + CLI args. The low-level builtins (fs_read_into /
    // fs_write_raw / fs_exists / arg_count / arg_get) return a status category
    // (0=ok, 1=not-found, 2=permission, 4=other); these wrappers present the
    // idiomatic `Result<_, IoError>` and `Vec<String>`. Variant names are
    // `Io`-prefixed so the always-injected enum can't clash with a user's own
    // `NotFound` etc. Guarded per-name so a user redefinition wins.
    // These are added LAZILY — only when the user source mentions the name —
    // so an I/O-free program carries none of the file-I/O machinery (and the
    // codegen, which emits the libc-referencing runtime on demand, stays
    // clean). A mention can't be missed: to call `fs_read_to_string` the name
    // must appear in the source. Each is also guarded against a user
    // redefinition.
    const bool wantsIo = userSrc.find("fs_read_to_string") != std::string::npos ||
                         userSrc.find("fs_write") != std::string::npos ||
                         userSrc.find("fs_exists") != std::string::npos ||
                         userSrc.find("IoError") != std::string::npos;
    if (wantsIo && userSrc.find("enum IoError") == std::string::npos) {
        prelude +=
            "enum IoError { IoNotFound, IoPermissionDenied, IoOther }\n";
    }
    if (wantsIo && userSrc.find("fn io_error_cat") == std::string::npos) {
        prelude +=
            "fn io_error_cat(c: i64) -> IoError {\n"
            "    if c == 1 { IoNotFound }\n"
            "    else if c == 2 { IoPermissionDenied }\n"
            "    else { IoOther }\n"
            "}\n";
    }
    if (userSrc.find("fs_read_to_string") != std::string::npos &&
        userSrc.find("fn fs_read_to_string") == std::string::npos) {
        prelude +=
            "fn fs_read_to_string(path: &String)"
            " -> Result<String, IoError> ! { io, alloc } {\n"
            "    let mut s = string_new();\n"
            "    let c = fs_read_into(path, &mut s);\n"
            "    if c == 0 { Ok(s) } else { Err(io_error_cat(c)) }\n"
            "}\n";
    }
    if (userSrc.find("fs_write") != std::string::npos &&
        userSrc.find("fn fs_write(") == std::string::npos) {
        prelude +=
            "fn fs_write(path: &String, contents: &String)"
            " -> Result<i64, IoError> ! { io } {\n"
            "    let c = fs_write_raw(path, contents);\n"
            "    if c == 0 { Ok(0) } else { Err(io_error_cat(c)) }\n"
            "}\n";
    }
    if (userSrc.find("args") != std::string::npos &&
        userSrc.find("fn args(") == std::string::npos) {
        prelude +=
            "fn args() -> Vec<String> ! { alloc } {\n"
            "    let mut v = vec_new();\n"
            "    let n = arg_count();\n"
            "    let mut i = 0;\n"
            "    while i < n { vec_push(&mut v, arg_get(i)); i = i + 1; }\n"
            "    v\n"
            "}\n";
    }
    // Phase 13b: Option / Result combinators as effect-row-polymorphic
    // prelude functions. They lower like any other generic kardashev fn —
    // closures + `! {e}` rows mean a combinator's call-site inherits its
    // closure's effects (an `io` closure in `option_map` makes the call
    // `io`). i64 payload is the accepted MVP. Each is guarded on its own
    // name so a user redefinition wins without a duplicate-decl error.
    if (userSrc.find("fn option_map") == std::string::npos) {
        prelude +=
            "fn option_map(o: Option<i64>, f: fn(i64) -> i64 ! {e})"
            " -> Option<i64> ! {e} {\n"
            "    match o { Some(x) => Some(f(x)), None => None }\n"
            "}\n";
    }
    // Phase 53 (v9): generic Vec higher-order combinators over closures, each
    // effect-polymorphic in the closure's effect row `e` (so a pure mapper
    // keeps the caller pure, an allocating one adds `alloc`). The closure
    // receives each element BY REFERENCE (`&T`); map/filter return fresh owned
    // Vecs (filter deep-clones the kept elements — the source is only
    // borrowed). NOTE: a closure passed here needs its param type annotated
    // (`|x: &i64| ..`) — closure-param inference from a generic fn-typed param
    // is not yet wired. Guarded per-fn so a user definition wins.
    if (userSrc.find("fn vec_map") == std::string::npos) {
        prelude +=
            "fn vec_map<T, U, e>(v: &Vec<T>, f: fn(&T) -> U ! {e})"
            " -> Vec<U> ! { alloc, e } {\n"
            "    let mut out = vec_new();\n"
            "    let mut i = 0;\n"
            "    while i < vec_len(v) {\n"
            "        vec_push(&mut out, f(vec_get_ref(v, i)));\n"
            "        i = i + 1;\n"
            "    }\n"
            "    out\n"
            "}\n";
    }
    if (userSrc.find("fn vec_filter") == std::string::npos) {
        prelude +=
            "fn vec_filter<T, e>(v: &Vec<T>, pred: fn(&T) -> bool ! {e})"
            " -> Vec<T> ! { alloc, e } {\n"
            "    let mut out = vec_new();\n"
            "    let mut i = 0;\n"
            "    while i < vec_len(v) {\n"
            "        let x = vec_get_ref(v, i);\n"
            "        if pred(x) { vec_push(&mut out, clone(x)); } else {}\n"
            "        i = i + 1;\n"
            "    }\n"
            "    out\n"
            "}\n";
    }
    if (userSrc.find("fn vec_fold") == std::string::npos) {
        prelude +=
            "fn vec_fold<T, A, e>(v: &Vec<T>, init: A, f: fn(A, &T) -> A ! {e})"
            " -> A ! {e} {\n"
            "    let mut acc = init;\n"
            "    let mut i = 0;\n"
            "    while i < vec_len(v) {\n"
            "        acc = f(acc, vec_get_ref(v, i));\n"
            "        i = i + 1;\n"
            "    }\n"
            "    acc\n"
            "}\n";
    }
    // Phase 55 (v9): enumerate a HashMap's entries as a Vec of (key, value)
    // tuples (each deep-cloned, so the Vec owns them), so a map can be ranked /
    // printed without manual key lookups. Bucket-order; the caller sorts (e.g.
    // via a derived-Ord wrapper). Guarded.
    if (userSrc.find("fn hashmap_entries") == std::string::npos) {
        prelude +=
            "fn hashmap_entries<K: Hash + Eq + Clone, V: Clone>"
            "(m: &HashMap<K, V>) -> Vec<(K, V)> ! { alloc } {\n"
            "    let mut out = vec_new();\n"
            "    let ks = hashmap_keys(m);\n"
            "    let mut i = 0;\n"
            "    while i < vec_len(&ks) {\n"
            "        let kref = vec_get_ref(&ks, i);\n"
            "        match hashmap_get_ref(m, kref.clone()) {\n"
            "            Some(vref) => { vec_push(&mut out, (kref.clone(), vref.clone())); },\n"
            "            None => {},\n"
            "        }\n"
            "        i = i + 1;\n"
            "    }\n"
            "    out\n"
            "}\n";
    }
    if (userSrc.find("fn option_unwrap_or") == std::string::npos) {
        prelude +=
            "fn option_unwrap_or(o: Option<i64>, default: i64) -> i64 {\n"
            "    match o { Some(x) => x, None => default }\n"
            "}\n";
    }
    if (userSrc.find("fn option_and_then") == std::string::npos) {
        prelude +=
            "fn option_and_then(o: Option<i64>,"
            " f: fn(i64) -> Option<i64> ! {e}) -> Option<i64> ! {e} {\n"
            "    match o { Some(x) => f(x), None => None }\n"
            "}\n";
    }
    if (userSrc.find("fn result_map") == std::string::npos) {
        prelude +=
            "fn result_map(r: Result<i64, i64>, f: fn(i64) -> i64 ! {e})"
            " -> Result<i64, i64> ! {e} {\n"
            "    match r { Ok(x) => Ok(f(x)), Err(e) => Err(e) }\n"
            "}\n";
    }
    if (userSrc.find("fn result_unwrap_or") == std::string::npos) {
        prelude +=
            "fn result_unwrap_or(r: Result<i64, i64>, default: i64)"
            " -> i64 {\n"
            "    match r { Ok(x) => x, Err(e) => default }\n"
            "}\n";
    }
    return prelude + userSrc;
}

// Phase 42: synthesize `impl` SOURCE for each `#[derive(...)]` on a struct/enum
// (field-/payload-wise Clone, Eq, Display), then re-parse + append. Reuses the
// real impl machinery (incl. generic impls, Phase 40) instead of hand-building
// AST. A generic type's params each gain the derived trait as a bound.
// Phase 46: does a DERIVED `eq` over this field/payload type allocate? True iff
// the type is — or transitively contains — a HashMap/HashSet, whose Eq impl
// enumerates keys (an `alloc`). `allocEqUserTypes` names the user struct/enums
// already known (via the fixpoint below) to derive an allocating `eq`.
static bool typeRefAllocEq(
    const kardashev::ast::TypeRef& tr,
    const std::unordered_set<std::string>& allocEqUserTypes) {
    if (tr.name == "HashMap" || tr.name == "HashSet") return true;
    if (allocEqUserTypes.count(tr.name)) return true;
    for (const auto& a : tr.typeArgs)
        if (typeRefAllocEq(a, allocEqUserTypes)) return true;
    return false;
}

std::string deriveImplSource(const kardashev::ast::Program& prog) {
    auto hasDerive = [](const std::vector<std::string>& ds,
                        const char* d) -> bool {
        for (const auto& x : ds)
            if (x == d) return true;
        return false;
    };
    // Phase 46: the set of derived types whose generated `eq` ALLOCATES, by
    // least-fixpoint: a type allocates if it derives Eq and has a field/payload
    // that is (or transitively contains) a map, OR another already-allocating
    // derived type. Only such an `eq` is annotated `! { alloc }`; a map-free
    // derived `eq` stays pure (so comparing pure structs needs no alloc
    // context). Propagation is restricted to types that derive Eq — the only
    // ones whose `eq` we know to be the generated, allocating form.
    std::unordered_set<std::string> allocEq;
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& s : prog.structs) {
                if (allocEq.count(s.name) || !hasDerive(s.derives, "Eq"))
                    continue;
                for (const auto& f : s.fields)
                    if (typeRefAllocEq(f.type, allocEq)) {
                        allocEq.insert(s.name);
                        changed = true;
                        break;
                    }
            }
            for (const auto& e : prog.enums) {
                if (allocEq.count(e.name) || !hasDerive(e.derives, "Eq"))
                    continue;
                bool hit = false;
                for (const auto& v : e.variants) {
                    for (const auto& pt : v.payloadTypes)
                        if (typeRefAllocEq(pt, allocEq)) {
                            hit = true;
                            break;
                        }
                    if (hit) break;
                }
                if (hit) {
                    allocEq.insert(e.name);
                    changed = true;
                }
            }
        }
    }
    auto eqEffect = [&](const std::string& name) -> const char* {
        return allocEq.count(name) ? " ! { alloc }" : "";
    };
    auto header = [](const std::vector<kardashev::ast::TypeParam>& ps,
                     const char* bound) -> std::string {
        if (ps.empty()) return "";
        std::string s = "<";
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i) s += ", ";
            s += ps[i].name;
            s += ": ";
            s += bound;
        }
        return s + ">";
    };
    auto tyName = [](const std::string& name,
                     const std::vector<kardashev::ast::TypeParam>& ps) -> std::string {
        if (ps.empty()) return name;
        std::string s = name + "<";
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i) s += ", ";
            s += ps[i].name;
        }
        return s + ">";
    };
    // Phase 48: the default-value expression for a field of the given type in a
    // derived `default()`. Leaf primitives + containers are inlined (no static
    // call needed); a concrete nested user type defers to its own
    // `Type::default()`. A generic-param field would need a generic static
    // call (deferred), so derive(Default) is emitted only for non-generic
    // structs below.
    auto defaultExprFor =
        [](const kardashev::ast::TypeRef& tr) -> std::string {
        if (tr.name == "i64") return "0";
        if (tr.name == "bool") return "false";
        if (tr.name == "f64") return "0.0";
        if (tr.name == "String") return "\"\"";
        if (tr.name == "Vec") return "vec_new()";
        if (tr.name == "HashMap") return "hashmap_new()";
        return tr.name + "::default()"; // concrete nested user type
    };
    std::string out;
    for (const auto& s : prog.structs) {
        const std::string TN = tyName(s.name, s.genericParams);
        for (const auto& d : s.derives) {
            if (d == "Clone") {
                out += "impl" + header(s.genericParams, "Clone") + " Clone for " +
                       TN + " { fn clone(&self) -> " + TN + " ! { alloc } { " +
                       s.name + " {";
                for (std::size_t i = 0; i < s.fields.size(); ++i)
                    out += (i ? "," : "") + (" " + s.fields[i].name + ": self." +
                                             s.fields[i].name + ".clone()");
                out += " } } }\n";
            } else if (d == "Eq") {
                out += "impl" + header(s.genericParams, "Eq") + " Eq for " + TN +
                       " { fn eq(&self, other: &" + TN + ") -> bool" +
                       eqEffect(s.name) + " { ";
                if (s.fields.empty()) out += "true";
                for (std::size_t i = 0; i < s.fields.size(); ++i)
                    out += (i ? " && " : "") + ("self." + s.fields[i].name +
                                                ".eq(&other." +
                                                s.fields[i].name + ")");
                out += " } }\n";
            } else if (d == "Display") {
                out += "impl" + header(s.genericParams, "Display") +
                       " Display for " + TN +
                       " { fn to_string(&self) -> String ! { alloc } { let mut "
                       "out = \"" + s.name + " { \";";
                for (std::size_t i = 0; i < s.fields.size(); ++i) {
                    if (i)
                        out += " string_push_str(&mut out, \", \");";
                    out += " string_push_str(&mut out, \"" + s.fields[i].name +
                           ": \"); string_push_str(&mut out, self." +
                           s.fields[i].name + ".to_string());";
                }
                out += " string_push_str(&mut out, \" }\"); out } }\n";
            } else if (d == "Hash") {
                // Phase 48: combine field hashes left-to-right (h = h*31 + fi).
                out += "impl" + header(s.genericParams, "Hash") + " Hash for " +
                       TN + " { fn hash(&self) -> i64 { let mut h = 17;";
                for (const auto& f : s.fields)
                    out += " h = h * 31 + self." + f.name + ".hash();";
                out += " h } }\n";
            } else if (d == "Ord") {
                // Phase 48: lexicographic field compare — first non-equal field
                // decides; all-equal => 0. Built as a nested if/else so each
                // `cmp` is evaluated only until one differs.
                out += "impl" + header(s.genericParams, "Ord") + " Ord for " +
                       TN + " { fn cmp(&self, other: &" + TN + ") -> i64 { ";
                std::string tail;
                for (const auto& f : s.fields) {
                    out += "let c = self." + f.name + ".cmp(&other." + f.name +
                           "); if c != 0 { c } else { ";
                    tail += " }";
                }
                out += "0" + tail + " } }\n";
            } else if (d == "Default" && s.genericParams.empty()) {
                // Phase 48: field-wise default. Leaf/container fields inline
                // their default; a concrete nested user type uses its own
                // Type::default() static call. Non-generic structs only: a
                // generic struct's `Pair::default()` would need a generic-TYPE
                // static call (type args inferred from context + the static impl
                // method monomorphized) — deferred. The Phase-52 bounded
                // `T::default()` form (a generic PARAM) does work.
                out += "impl Default for " + TN +
                       " { fn default() -> " + TN + " ! { alloc } { " + s.name +
                       " {";
                for (std::size_t i = 0; i < s.fields.size(); ++i)
                    out += (i ? "," : "") + (" " + s.fields[i].name + ": " +
                                             defaultExprFor(s.fields[i].type));
                out += " } } }\n";
            }
        }
    }
    for (const auto& e : prog.enums) {
        const std::string TN = tyName(e.name, e.genericParams);
        const bool multi = e.variants.size() > 1;
        for (const auto& d : e.derives) {
            if (d == "Clone") {
                out += "impl" + header(e.genericParams, "Clone") + " Clone for " +
                       TN + " { fn clone(&self) -> " + TN +
                       " ! { alloc } { match self {";
                for (std::size_t v = 0; v < e.variants.size(); ++v) {
                    const auto& var = e.variants[v];
                    out += " " + var.name;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") + ("x" + std::to_string(i));
                        out += ")";
                    }
                    out += " => " + var.name;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") +
                                   ("x" + std::to_string(i) + ".clone()");
                        out += ")";
                    }
                    out += ",";
                }
                out += " } } }\n";
            } else if (d == "Eq") {
                out += "impl" + header(e.genericParams, "Eq") + " Eq for " + TN +
                       " { fn eq(&self, other: &" + TN + ") -> bool" +
                       eqEffect(e.name) + " { match self {";
                for (const auto& var : e.variants) {
                    out += " " + var.name;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") + ("a" + std::to_string(i));
                        out += ")";
                    }
                    out += " => match other { " + var.name;
                    std::string body;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i) {
                            out += (i ? ", " : "") + ("b" + std::to_string(i));
                            body += (i ? " && " : "") +
                                    ("a" + std::to_string(i) + ".eq(b" +
                                     std::to_string(i) + ")");
                        }
                        out += ")";
                    } else {
                        body = "true";
                    }
                    out += " => " + body + ",";
                    if (multi) out += " _ => false,";
                    out += " },";
                }
                out += " } } }\n";
            } else if (d == "Display") {
                out += "impl" + header(e.genericParams, "Display") +
                       " Display for " + TN +
                       " { fn to_string(&self) -> String ! { alloc } { match "
                       "self {";
                for (const auto& var : e.variants) {
                    out += " " + var.name;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") + ("x" + std::to_string(i));
                        out += ")";
                    }
                    if (var.payloadTypes.empty()) {
                        out += " => \"" + var.name + "\",";
                    } else {
                        out += " => { let mut out = \"" + var.name + "(\";";
                        for (std::size_t i = 0; i < var.payloadTypes.size();
                             ++i) {
                            if (i)
                                out += " string_push_str(&mut out, \", \");";
                            out += " string_push_str(&mut out, x" +
                                   std::to_string(i) + ".to_string());";
                        }
                        out += " string_push_str(&mut out, \")\"); out },";
                    }
                }
                out += " } } }\n";
            } else if (d == "Hash") {
                // Phase 48: hash = combine(variant ordinal, payload hashes).
                out += "impl" + header(e.genericParams, "Hash") + " Hash for " +
                       TN + " { fn hash(&self) -> i64 { match self {";
                for (std::size_t v = 0; v < e.variants.size(); ++v) {
                    const auto& var = e.variants[v];
                    const std::string seed = std::to_string(527 + v); // 17*31+v
                    out += " " + var.name;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") + ("x" + std::to_string(i));
                        out += ")";
                    }
                    if (var.payloadTypes.empty()) {
                        out += " => " + seed + ",";
                    } else {
                        out += " => { let mut h = " + seed + ";";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += " h = h * 31 + x" + std::to_string(i) +
                                   ".hash();";
                        out += " h },";
                    }
                }
                out += " } } }\n";
            } else if (d == "Ord") {
                // Phase 48: lexicographic — compare variant ordinals first, then
                // (for the same variant) payloads field-by-field. The two
                // ordinal matches use `_` payload wildcards.
                out += "impl" + header(e.genericParams, "Ord") + " Ord for " +
                       TN + " { fn cmp(&self, other: &" + TN + ") -> i64 {";
                auto ordinalMatch = [&](const char* scrut) {
                    std::string m = std::string(" match ") + scrut + " {";
                    for (std::size_t v = 0; v < e.variants.size(); ++v) {
                        const auto& var = e.variants[v];
                        m += " " + var.name;
                        if (!var.payloadTypes.empty()) {
                            m += "(";
                            for (std::size_t i = 0;
                                 i < var.payloadTypes.size(); ++i)
                                m += (i ? ", " : "") + std::string("_");
                            m += ")";
                        }
                        m += " => " + std::to_string(v) + ",";
                    }
                    return m + " }";
                };
                out += " let so =" + ordinalMatch("self") + ";";
                out += " let oo =" + ordinalMatch("other") + ";";
                out += " if so != oo { if so < oo { 0 - 1 } else { 1 } } else {";
                out += " match self {";
                for (const auto& var : e.variants) {
                    out += " " + var.name;
                    if (!var.payloadTypes.empty()) {
                        out += "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") + ("a" + std::to_string(i));
                        out += ")";
                    }
                    if (var.payloadTypes.empty()) {
                        out += " => 0,";
                    } else {
                        out += " => match other { " + var.name + "(";
                        for (std::size_t i = 0; i < var.payloadTypes.size(); ++i)
                            out += (i ? ", " : "") + ("b" + std::to_string(i));
                        out += ") => { ";
                        std::string tail;
                        for (std::size_t i = 0; i < var.payloadTypes.size();
                             ++i) {
                            out += "let c = a" + std::to_string(i) + ".cmp(b" +
                                   std::to_string(i) + "); if c != 0 { c } else { ";
                            tail += " }";
                        }
                        out += "0" + tail;
                        out += " }, _ => 0 },";
                    }
                }
                out += " } } } }\n";
            }
        }
    }
    return out;
}

// Generate + parse the derive impls and append them to the program.
void expandDerives(kardashev::ast::Program& prog) {
    std::string gen = deriveImplSource(prog);
    if (gen.empty()) return;
    auto pr = kardashev::parse(gen);
    for (auto& impl : pr.program.impls)
        prog.impls.push_back(std::move(impl));
}

// Phase 7.1: resolve `mod foo;` directives by reading sibling `.kd`
// files and merging their declarations into the caller's program. The
// merge is FLAT — module contents become part of the top-level program
// with no namespacing — so duplicate-decl errors surface at typecheck
// as usual. Recursive: a module may itself declare more `mod` entries
// and they're resolved transitively.
//
// `parentDir` is the directory of the file currently being resolved.
// `visited` tracks absolute module paths so a cycle reports an error
// rather than recursing forever.
//
// Returns true on success; on file-read errors the message is appended
// to `errors` and the partial program is left in `out`.
bool resolveModules(const std::string& srcRaw,
                     const std::string& parentDir,
                     std::unordered_set<std::string>& visited,
                     kardashev::ast::Program& out,
                     std::vector<std::string>& errors);

// Read a file fully into a string. Empty optional on I/O failure.
std::optional<std::string> readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Naive `dirname`: returns the substring up to the last '/'. Returns "."
// when the path has no slash. Works for forward-slash paths on Linux /
// macOS (the platforms the build matrix exercises).
std::string dirOf(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

bool resolveModules(const std::string& srcRaw,
                     const std::string& parentDir,
                     std::unordered_set<std::string>& visited,
                     kardashev::ast::Program& out,
                     std::vector<std::string>& errors) {
    auto pr = kardashev::parse(srcRaw);
    if (!pr.ok()) {
        for (const auto& e : pr.errors) {
            errors.push_back("parse error " + std::to_string(e.line) + ":" +
                              std::to_string(e.column) + ": " + e.message);
        }
        return false;
    }
    // Merge this program's decls into `out`. We move so the program's
    // internal pointers / unique_ptrs end up owned by `out`.
    for (auto& fn : pr.program.functions) out.functions.push_back(std::move(fn));
    for (auto& sd : pr.program.structs) out.structs.push_back(std::move(sd));
    for (auto& ed : pr.program.enums) out.enums.push_back(std::move(ed));
    for (auto& td : pr.program.traits) out.traits.push_back(std::move(td));
    for (auto& impl : pr.program.impls) out.impls.push_back(std::move(impl));
    // Phase 24: carry `extern "C"` declarations across the module merge too,
    // so a program (or an imported `mod`) can declare + call C functions.
    for (auto& ef : pr.program.externFns)
        out.externFns.push_back(std::move(ef));
    // Phase 25: carry top-level `const` items across the module merge.
    for (auto& cd : pr.program.consts)
        out.consts.push_back(std::move(cd));

    // Recurse into each `mod foo;` reference.
    for (const auto& m : pr.program.mods) {
        std::string modPath = parentDir + "/" + m.name + ".kd";
        if (!visited.insert(modPath).second) continue; // already merged
        auto content = readFile(modPath);
        if (!content) {
            errors.push_back("mod resolve error: cannot open `" + modPath +
                              "` (declared at " + std::to_string(m.line) +
                              ":" + std::to_string(m.column) + ")");
            continue;
        }
        if (!resolveModules(*content, dirOf(modPath), visited, out, errors)) {
            return false;
        }
    }
    return true;
}

void reportParseErrors(const kardashev::ParseResult& r) {
    for (const auto& e : r.errors) {
        std::cerr << "parse error " << e.line << ":" << e.column << ": "
                  << e.message << '\n';
    }
}

void reportTypeErrors(const kardashev::TypeCheckResult& r) {
    for (const auto& e : r.errors) {
        std::cerr << "type error " << e.line << ":" << e.column << ": "
                  << e.message << '\n';
    }
}

void reportBorrowErrors(const kardashev::BorrowCheckResult& r) {
    for (const auto& e : r.errors) {
        std::cerr << "borrow error " << e.line << ":" << e.column << ": "
                  << e.message << '\n';
    }
}

// Compile `src` through the full pipeline and JIT-call the named entry
// (which must be a no-arg function returning i64). Diagnostics go to
// stderr. Returns the i64 result on success, nullopt otherwise.
// Build a merged Program from `srcRaw`, resolving any `mod foo;`
// references relative to `srcDir`. Prelude is applied to the root source
// (modules are imported as-is — they can reference Option / Result
// without redeclaring them since the merged program inherits the
// prelude declarations once).
//
// On success, returns the merged Program inside the optional. On
// parse / file-read failure, error messages are printed to stderr and
// nullopt is returned.
std::optional<kardashev::ast::Program> buildProgram(
    const std::string& srcRaw, const std::string& srcDir) {
    const std::string src = applyPrelude(srcRaw);
    kardashev::ast::Program merged;
    std::unordered_set<std::string> visited;
    std::vector<std::string> errors;
    if (!resolveModules(src, srcDir, visited, merged, errors)) {
        for (const auto& e : errors) std::cerr << e << '\n';
        return std::nullopt;
    }
    if (!errors.empty()) {
        for (const auto& e : errors) std::cerr << e << '\n';
        return std::nullopt;
    }
    return merged;
}

std::optional<std::int64_t> compileAndRun(const std::string& srcRaw,
                                          const std::string& entry,
                                          const std::string& srcDir = ".",
                                          bool emitDebug = false,
                                          const std::string& sourceFile =
                                              "<kardashev>",
                                          kardashev::OptLevel optLevel =
                                              kardashev::OptLevel::O2) {
    auto progOpt = buildProgram(srcRaw, srcDir);
    if (!progOpt) return std::nullopt;
    auto& program = *progOpt;
    expandDerives(program); // Phase 42: synthesize #[derive(...)] impls
    // PR#26 (ABI safety): the JIT calls `entry` through an i64()-typed
    // function pointer, so reject any entry whose signature doesn't match —
    // a non-empty parameter list or a non-integer return would corrupt the
    // ABI. i64 and bool (i1, zero-extended by the caller since Phase 15) are
    // the supported entry return widths.
    bool entryOk = false;
    for (const auto& fn : program.functions) {
        if (fn.name != entry) continue;
        entryOk = fn.params.empty() && (fn.returnType.name == "i64" ||
                                        fn.returnType.name == "bool");
        break;
    }
    if (!entryOk) {
        std::cerr << "kardc: entry `" << entry << "` must have signature `fn "
                  << entry << "() -> i64` (or `-> bool`) for JIT execution\n";
        return std::nullopt;
    }
    auto tcr = kardashev::typecheck(program);
    if (!tcr.ok()) {
        reportTypeErrors(tcr);
        return std::nullopt;
    }
    auto bcr = kardashev::borrow_check(program, tcr);
    if (!bcr.ok()) {
        reportBorrowErrors(bcr);
        return std::nullopt;
    }
    auto cgr = kardashev::codegen(program, tcr, emitDebug, sourceFile,
                                  optLevel);
    if (!cgr.ok()) {
        for (const auto& msg : cgr.errors) {
            std::cerr << "codegen error: " << msg << '\n';
        }
        return std::nullopt;
    }

    // Capture the entry's return-type width before the module is moved into
    // the JIT. The JIT pointer is typed `int64_t(*)()`, but Phase 15 lets the
    // entry return `bool` (an i1); calling an i1-returning fn through an i64
    // pointer leaves the upper 63 bits undefined. We mask the raw result to
    // the entry's real width below so `fn main() -> bool` prints 0/1, not
    // garbage. Default to 64 (the common i64 case, and any non-integer ret).
    unsigned entryRetBits = 64;
    if (auto* entryFn = cgr.module->getFunction(entry)) {
        if (auto* rt = entryFn->getReturnType(); rt && rt->isIntegerTy()) {
            entryRetBits = rt->getIntegerBitWidth();
        }
    }

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        llvm::errs() << "LLJIT create failed: "
                     << llvm::toString(jitOrErr.takeError()) << '\n';
        return std::nullopt;
    }
    auto jit = std::move(*jitOrErr);

    auto tsm = llvm::orc::ThreadSafeModule(std::move(cgr.module),
                                           std::move(cgr.context));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::errs() << "addIRModule failed: "
                     << llvm::toString(std::move(err)) << '\n';
        return std::nullopt;
    }
    auto symOrErr = jit->lookup(entry);
    if (!symOrErr) {
        llvm::errs() << "lookup '" << entry << "' failed: "
                     << llvm::toString(symOrErr.takeError()) << '\n';
        return std::nullopt;
    }

    using EntryFn = std::int64_t (*)();
    auto fn = symOrErr->toPtr<EntryFn>();
    std::int64_t raw = fn();
    // Mask off bits above the entry's real return width (see entryRetBits).
    if (entryRetBits < 64) {
        raw &= (static_cast<std::int64_t>(1) << entryRetBits) - 1;
    }
    return raw;
}

// Heuristic: a line whose first non-whitespace token is `fn `,
// `struct `, `enum `, or `extern ` (Phase 24 FFI) is treated as a top-level
// decl to add to the accumulator. Anything else is parsed as an expression
// and evaluated.
bool looksLikeTopLevelDecl(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const auto rest = std::string_view(line).substr(i);
    return rest.substr(0, 3) == "fn " ||
           rest.substr(0, 7) == "struct " ||
           rest.substr(0, 5) == "enum " ||
           rest.substr(0, 7) == "extern ";
}

int runREPL() {
    const bool interactive = isatty(STDIN_FILENO) != 0;
    if (interactive) {
        std::cout << "kardashev v0.0 (Phase 1 MVP)\n"
                     "type `fn name(...) -> T { ... }` to define, "
                     "or a bare expression to evaluate. Ctrl-D to exit.\n";
    }

    std::string accumulated; // current fn-definition environment
    std::string line;

    while (true) {
        if (interactive) std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (looksLikeTopLevelDecl(line)) {
            // Validate trial = accumulated + line. Only commit on success.
            // applyPrelude is applied per-evaluation in compileAndRun, so
            // here we just validate the accumulated buffer as-is plus the
            // implicit prelude on top.
            std::string trial = accumulated + line + "\n";
            auto pr = kardashev::parse(applyPrelude(trial));
            if (!pr.ok()) {
                reportParseErrors(pr);
                continue;
            }
            auto tcr = kardashev::typecheck(pr.program);
            if (!tcr.ok()) {
                reportTypeErrors(tcr);
                continue;
            }
            auto bcr = kardashev::borrow_check(pr.program, tcr);
            if (!bcr.ok()) {
                reportBorrowErrors(bcr);
                continue;
            }
            accumulated = std::move(trial);
            if (interactive) std::cout << "ok\n";
            continue;
        }

        // Bare expression. Wrap in a synthetic __eval and JIT-run.
        std::string src =
            accumulated + "fn __eval() -> i64 { " + line + " }\n";
        if (auto result = compileAndRun(src, "__eval")) {
            std::cout << *result << '\n';
        }
    }

    return 0;
}

int runFile(const char* path, bool emitDebug = false,
            kardashev::OptLevel optLevel = kardashev::OptLevel::O2) {
    auto src = readFile(path);
    if (!src) {
        std::cerr << "kardc: cannot open file: " << path << '\n';
        return 1;
    }
    if (auto result =
            compileAndRun(*src, "main", dirOf(path), emitDebug, path,
                          optLevel)) {
        std::cout << *result << '\n';
        return 0;
    }
    return 1;
}

// Phase 5a AOT: rewrap the user's `main` so the produced object exposes
// a standard C entry point. The user's i64 main is renamed to
// `__kd_main` and a new `int main(void)` wrapper calls it and returns
// the truncated i64. The C wrapper is what the OS jumps into via the
// standard CRT bootstrap (added by clang during linking).
void addCMainWrapper(llvm::Module& mod) {
    auto* userMain = mod.getFunction("main");
    if (!userMain) {
        llvm::errs() << "kardc: AOT mode requires the program to define "
                        "`fn main() -> i64`\n";
        return;
    }
    userMain->setName("__kd_main");

    auto& ctx = mod.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
    // Phase 30: take (int argc, char** argv) so the `arg_count` / `arg_get`
    // builtins can see the CLI args (their `__kd_argc` / `__kd_argv` globals
    // default to 0/null, which is what the JIT — with no real argv — uses).
    auto* mainTy =
        llvm::FunctionType::get(i32Ty, {i32Ty, i8PtrTy}, /*isVarArg=*/false);
    auto* cmain = llvm::Function::Create(
        mainTy, llvm::Function::ExternalLinkage, "main", &mod);
    auto* entry = llvm::BasicBlock::Create(ctx, "entry", cmain);
    llvm::IRBuilder<> b(entry);
    if (auto* argcG = mod.getNamedGlobal("__kd_argc"))
        b.CreateStore(b.CreateZExt(cmain->getArg(0), i64Ty), argcG);
    if (auto* argvG = mod.getNamedGlobal("__kd_argv"))
        b.CreateStore(cmain->getArg(1), argvG);
    auto* result = b.CreateCall(userMain, {}, "kdresult");
    // The user's `main` is usually `-> i64`, but Phase 15 allows `-> bool`
    // (an i1). Adapt to the i32 exit code by integer width: truncate a wider
    // result (i64 -> i32) or zero-extend a narrower one (i1 -> i32). A trunc
    // from i1 would be invalid IR, so the width check is load-bearing.
    llvm::Value* exitCode = result;
    unsigned bits = result->getType()->getIntegerBitWidth();
    if (bits > 32) {
        exitCode = b.CreateTrunc(result, i32Ty, "exitcode");
    } else if (bits < 32) {
        exitCode = b.CreateZExt(result, i32Ty, "exitcode");
    }
    b.CreateRet(exitCode);
}

// Emit `module` to an object file at `outObjPath`. Returns true on
// success; logs to stderr on failure.
bool emitObject(llvm::Module& module, const std::string& outObjPath) {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    // LLVM 21+ switched setTargetTriple / createTargetMachine to take a
    // `Triple` value; older LLVMs (Ubuntu CI's apt-installed 18/19/20)
    // only accept `StringRef`. Branch on LLVM_VERSION_MAJOR so the same
    // source compiles against both ends of the matrix.
    std::string tripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 21
    llvm::Triple triple(tripleStr);
    module.setTargetTriple(triple);
#else
    module.setTargetTriple(tripleStr);
#endif
    std::string err;
    const auto* target = llvm::TargetRegistry::lookupTarget(tripleStr, err);
    if (!target) {
        llvm::errs() << "kardc: target lookup failed: " << err << '\n';
        return false;
    }
    llvm::TargetOptions opts;
    // Phase 6.0 added a global string constant for `print`'s format
    // literal. Linux distros' default linker config builds PIE
    // executables; non-PIC object code with absolute relocations is
    // rejected. Compiling the emitted object as PIC fixes the link
    // without forcing `-no-pie` on the clang invocation (which would
    // also affect platforms — like macOS — that don't need the flag).
#if LLVM_VERSION_MAJOR >= 21
    auto* tm = target->createTargetMachine(
        triple, "generic", "", opts, llvm::Reloc::PIC_);
#else
    auto* tm = target->createTargetMachine(
        tripleStr, "generic", "", opts, llvm::Reloc::PIC_);
#endif
    module.setDataLayout(tm->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream out(outObjPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        llvm::errs() << "kardc: cannot open '" << outObjPath
                     << "': " << ec.message() << '\n';
        return false;
    }
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, out, nullptr,
                                  llvm::CodeGenFileType::ObjectFile)) {
        llvm::errs() << "kardc: TargetMachine cannot emit objects\n";
        return false;
    }
    pm.run(module);
    out.flush();
    return true;
}

// Link `objPath` into a native executable at `outExePath` via the system
// `clang`. We use `clang` from PATH; the CI image has it, and Bazel's
// LLVM-detection step already pulled it in for the build. -fuse-ld is left
// to clang's default so it picks the right linker per platform (lld on
// Linux if installed, ld64 on macOS). Returns true on success.
bool linkObject(const std::string& objPath, const std::string& outExePath) {
    // Phase 19: link libpthread for the OS-thread / Mutex builtins
    // (pthread_create/join/mutex_*). On modern glibc (>= 2.34) these symbols
    // are folded into libc and `-lpthread` is a harmless no-op; on older glibc
    // and other POSIX targets it pulls in the real library. POSIX, so it's the
    // same flag on Linux and macOS — no platform branching needed.
    // PR#23 (security): exec clang directly via llvm::sys::ExecuteAndWait
    // with an argv vector instead of std::system on a concatenated string,
    // so a crafted object/output path can't inject shell commands. Resolve
    // clang on PATH explicitly (ExecuteAndWait does not search PATH).
    auto clangPath = llvm::sys::findProgramByName("clang");
    if (!clangPath) {
        std::cerr << "kardc: cannot find 'clang' on PATH for linking\n";
        return false;
    }
    llvm::SmallVector<llvm::StringRef, 8> argv{
        *clangPath, objPath, "-o", outExePath, "-lpthread"};
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(*clangPath, argv, std::nullopt, {}, 0, 0,
                                       &errMsg);
    if (rc != 0) {
        std::cerr << "kardc: linker (clang) failed with exit code " << rc;
        if (!errMsg.empty()) std::cerr << " (" << errMsg << ")";
        std::cerr << '\n';
        return false;
    }
    return true;
}

// ====================================================================
// Phase 14b: content-addressed incremental compile cache (AOT path)
// ====================================================================
//
// Recompiling unchanged source is wasteful: lex → parse → typecheck →
// borrow → codegen → object-emit all reproduce a byte-identical object.
// We content-address that object by a hash over the FULLY-RESOLVED source
// (the root file + every `mod foo;` it transitively pulls in) plus the
// flags that change codegen (`-g`) plus a cache-format version tag. On a
// hit we skip the entire front+middle end and link the cached object
// straight away; on a miss we compile as usual, then deposit the object in
// the cache for next time.
//
// The cache is purely an optimization: a non-writable cache dir, a missing
// HOME, or any I/O hiccup falls back to a normal compile. It never changes
// the bytes of the produced object (so output stays identical to the
// no-cache path) and never crashes the compiler.

// Bump when the object-file format / codegen ABI changes in a way that
// must invalidate every existing cache entry. Combined into the key so an
// upgraded kardc never reuses a stale object.
constexpr const char* kCacheFormatVersion = "kardashev-cache-v4";

// FNV-1a 64-bit over a byte string. Small, dependency-free, and good
// enough for a content-addressed local cache (no adversarial inputs).
std::uint64_t fnv1a(const std::string& data, std::uint64_t seed) {
    std::uint64_t h = seed;
    for (unsigned char c : data) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL; // FNV prime
    }
    return h;
}

// Render a 64-bit hash as 16 lowercase hex digits (the cache filename stem).
std::string hexKey(std::uint64_t h) {
    static const char* digits = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[i] = digits[h & 0xF];
        h >>= 4;
    }
    return s;
}

// Concatenate the root source with the contents of every `mod foo;` file it
// transitively references, in resolution order, so the cache key reflects a
// change in ANY input file — not just the root. Mirrors resolveModules'
// path logic. Best-effort: a module that fails to parse / read simply
// contributes its raw bytes (or nothing) to the hash; correctness of the
// build itself is still enforced later by the real pipeline.
void collectFullSource(const std::string& srcRaw, const std::string& parentDir,
                       std::unordered_set<std::string>& visited,
                       std::string& acc) {
    acc += srcRaw;
    acc += "\0\0"; // separator so file-boundary shifts change the hash
    auto pr = kardashev::parse(srcRaw);
    if (!pr.ok()) return; // can't enumerate mods on a broken parse
    for (const auto& m : pr.program.mods) {
        std::string modPath = parentDir + "/" + m.name + ".kd";
        if (!visited.insert(modPath).second) continue;
        auto content = readFile(modPath);
        if (!content) continue;
        collectFullSource(*content, dirOf(modPath), visited, acc);
    }
}

// Compute the cache key for an AOT build of `srcRaw` (resolved against
// `srcDir`) with the given flags. Phase 20a folds the optimization level into
// the key so `-O0` and `-O2` objects (which differ in their bytes) never
// collide in the content-addressed cache.
std::string computeCacheKey(const std::string& srcRaw,
                            const std::string& srcDir, bool emitDebug,
                            kardashev::OptLevel optLevel) {
    std::string material;
    material += kCacheFormatVersion;
    material += emitDebug ? "|g=1|" : "|g=0|";
    material += "|O" + std::to_string(static_cast<int>(optLevel)) + "|";
    std::unordered_set<std::string> visited;
    collectFullSource(srcRaw, srcDir, visited, material);
    return hexKey(fnv1a(material, 1469598103934665603ULL /* FNV offset */));
}

// Resolve the cache directory: ${XDG_CACHE_HOME:-$HOME/.cache}/kardashev.
// Returns empty if neither env var is usable (caller then skips caching).
std::string cacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string base;
    if (xdg && xdg[0]) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && home[0]) {
        base = std::string(home) + "/.cache";
    } else {
        return "";
    }
    return base + "/kardashev";
}

// Create `dir` (and parents) if absent. Returns true if it exists and is
// usable afterwards. Failure is non-fatal (caller falls back to no cache).
bool ensureDir(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    llvm::sys::fs::create_directories(dir, /*IgnoreExisting=*/true);
    return llvm::sys::fs::is_directory(dir);
}

// Copy a file byte-for-byte. Returns true on success.
bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return out.good();
}

// Compile `srcRaw` through the full pipeline and emit a native object to
// `objPath`. Returns true on success; diagnostics go to stderr. Factored
// out of runAot so both the cache-miss path and the no-cache path share it.
bool compileToObject(const std::string& srcRaw, const std::string& srcDir,
                     bool emitDebug, const std::string& sourceFile,
                     const std::string& objPath,
                     kardashev::OptLevel optLevel) {
    auto progOpt = buildProgram(srcRaw, srcDir);
    if (!progOpt) return false;
    auto& program = *progOpt;
    expandDerives(program); // Phase 42: synthesize #[derive(...)] impls
    auto tcr = kardashev::typecheck(program);
    if (!tcr.ok()) {
        reportTypeErrors(tcr);
        return false;
    }
    auto bcr = kardashev::borrow_check(program, tcr);
    if (!bcr.ok()) {
        reportBorrowErrors(bcr);
        return false;
    }
    auto cgr = kardashev::codegen(program, tcr, emitDebug, sourceFile,
                                  optLevel);
    if (!cgr.ok()) {
        for (const auto& msg : cgr.errors) {
            std::cerr << "codegen error: " << msg << '\n';
        }
        return false;
    }
    addCMainWrapper(*cgr.module);
    return emitObject(*cgr.module, objPath);
}

// Phase 14a: run the front-end and print the module's textual LLVM IR to
// stdout (honoring `-g`, so debug metadata like !llvm.dbg.cu / DISubprogram
// is visible). Platform-independent way to inspect codegen — no external
// dwarf tooling required. Returns a process exit code.
int emitLlvmIr(const std::string& srcRaw, const std::string& srcDir,
               bool emitDebug, const std::string& sourceFile,
               kardashev::OptLevel optLevel) {
    auto progOpt = buildProgram(srcRaw, srcDir);
    if (!progOpt) return 1;
    auto& program = *progOpt;
    expandDerives(program); // Phase 42: synthesize #[derive(...)] impls
    auto tcr = kardashev::typecheck(program);
    if (!tcr.ok()) {
        reportTypeErrors(tcr);
        return 1;
    }
    auto bcr = kardashev::borrow_check(program, tcr);
    if (!bcr.ok()) {
        reportBorrowErrors(bcr);
        return 1;
    }
    auto cgr = kardashev::codegen(program, tcr, emitDebug, sourceFile,
                                  optLevel);
    if (!cgr.ok()) {
        for (const auto& msg : cgr.errors) {
            std::cerr << "codegen error: " << msg << '\n';
        }
        return 1;
    }
    cgr.module->print(llvm::outs(), nullptr);
    return 0;
}

// Compile + link `src` into a native executable at `outExePath`. When
// `useCache` is set we content-address the emitted object: a cache hit skips
// the whole compile and links the cached object; a miss compiles, deposits
// the object into the cache, then links. Cache state is logged to stderr so
// tests (and curious users) can observe hit/miss; program stdout stays clean.
int runAot(const std::string& srcRaw, const std::string& outExePath,
            const std::string& srcDir = ".", bool emitDebug = false,
            const std::string& sourceFile = "<kardashev>",
            bool useCache = true,
            kardashev::OptLevel optLevel = kardashev::OptLevel::O2) {
    // Intermediate object next to the output exe; always cleaned up.
    std::string objPath = outExePath + ".o";

    std::string dir = useCache ? cacheDir() : "";
    bool cacheUsable = useCache && ensureDir(dir);

    if (cacheUsable) {
        std::string key = computeCacheKey(srcRaw, srcDir, emitDebug, optLevel);
        std::string cachedObj = dir + "/" + key + ".o";
        if (llvm::sys::fs::exists(cachedObj)) {
            // HIT: reuse the cached object, skipping the entire pipeline.
            std::cerr << "kardc: cache hit " << key << '\n';
            if (!copyFile(cachedObj, objPath)) {
                // Cache read failed unexpectedly — fall back to a fresh
                // compile rather than aborting the build.
                std::cerr << "kardc: cache read failed; recompiling\n";
                if (!compileToObject(srcRaw, srcDir, emitDebug, sourceFile,
                                     objPath, optLevel))
                    return 1;
            }
            bool linked = linkObject(objPath, outExePath);
            std::remove(objPath.c_str());
            return linked ? 0 : 1;
        }
        // MISS: compile, then deposit the object into the cache.
        std::cerr << "kardc: cache miss " << key << '\n';
        if (!compileToObject(srcRaw, srcDir, emitDebug, sourceFile, objPath,
                             optLevel))
            return 1;
        // Populate the cache atomically-ish: write to a temp then rename so a
        // concurrent reader never sees a half-written object. Both steps are
        // best-effort — a failure here is non-fatal (the build still links
        // from objPath). `rename` can fail across filesystems, so on error
        // fall back to a direct copy into place.
        std::string tmp = cachedObj + ".tmp" + std::to_string(::getpid());
        if (copyFile(objPath, tmp)) {
            std::error_code ec = llvm::sys::fs::rename(tmp, cachedObj);
            if (ec) {
                copyFile(objPath, cachedObj); // rename failed; copy directly
            }
            if (llvm::sys::fs::exists(tmp)) std::remove(tmp.c_str());
        }
        bool linked = linkObject(objPath, outExePath);
        std::remove(objPath.c_str());
        return linked ? 0 : 1;
    }

    // No cache (disabled, or dir not usable): compile + link as before.
    if (!compileToObject(srcRaw, srcDir, emitDebug, sourceFile, objPath,
                         optLevel))
        return 1;
    bool linked = linkObject(objPath, outExePath);
    std::remove(objPath.c_str());
    return linked ? 0 : 1;
}

// ====================================================================
// Phase 20a: `kardc --test <file.kd>` — discover + run unit tests
// ====================================================================
//
// Convention: a test is a `fn test_*() -> i64` (name begins with `test_`, no
// params, no generic params). Returning 0 means PASS; any nonzero return means
// FAIL (the exit-code model — a test can `return 1` or propagate a nonzero
// assertion result). The file need NOT define `main()` (a test file may hold
// only `test_*` fns), and the front-end already tolerates a missing `main`.
//
// We compile the whole program ONCE into a single JIT module, then look up and
// call each discovered test by name (generalizing the REPL/file mode's
// single-lookup machinery). Output mirrors common test runners:
//
//   running N tests
//   test test_foo ... ok
//   test test_bar ... FAILED (returned 3)
//   test result: X passed, Y failed
//
// Returns a process exit code: 0 if every test passed, 1 if any failed (or on a
// compile error / no tests found, which is treated as a failure to surface
// mistakes rather than silently "succeed").

// True if `fn` matches the test convention: name starts with `test_`, takes no
// parameters, and is non-generic. (Return type is enforced to be i64 by the
// typechecker via the call below; we additionally require a plain i64 return so
// the JIT call through an i64(*)() pointer is well-defined.)
bool isTestFn(const kardashev::ast::FnDecl& fn) {
    if (fn.name.rfind("test_", 0) != 0) return false; // must start with test_
    if (!fn.params.empty()) return false;
    if (!fn.genericParams.empty()) return false;
    // Return type must be i64 (no refs / type-args). The exit-code model.
    if (fn.returnType.name != "i64") return false;
    if (fn.returnType.isRef || !fn.returnType.typeArgs.empty()) return false;
    return true;
}

int runTests(const std::string& srcRaw, const std::string& srcDir,
             bool emitDebug, const std::string& sourceFile,
             kardashev::OptLevel optLevel) {
    auto progOpt = buildProgram(srcRaw, srcDir);
    if (!progOpt) return 1;
    auto& program = *progOpt;
    expandDerives(program); // Phase 42: synthesize #[derive(...)] impls
    auto tcr = kardashev::typecheck(program);
    if (!tcr.ok()) {
        reportTypeErrors(tcr);
        return 1;
    }
    auto bcr = kardashev::borrow_check(program, tcr);
    if (!bcr.ok()) {
        reportBorrowErrors(bcr);
        return 1;
    }

    // Discover test fns from the merged program. Prelude fns (option_map, …)
    // don't start with `test_`, so they're naturally excluded.
    std::vector<std::string> testNames;
    for (const auto& fn : program.functions) {
        if (isTestFn(fn)) testNames.push_back(fn.name);
    }
    if (testNames.empty()) {
        std::cerr << "kardc: no `test_*() -> i64` functions found in "
                  << sourceFile << '\n';
        return 1;
    }

    auto cgr = kardashev::codegen(program, tcr, emitDebug, sourceFile,
                                  optLevel);
    if (!cgr.ok()) {
        for (const auto& msg : cgr.errors) {
            std::cerr << "codegen error: " << msg << '\n';
        }
        return 1;
    }

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        llvm::errs() << "LLJIT create failed: "
                     << llvm::toString(jitOrErr.takeError()) << '\n';
        return 1;
    }
    auto jit = std::move(*jitOrErr);
    auto tsm = llvm::orc::ThreadSafeModule(std::move(cgr.module),
                                           std::move(cgr.context));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::errs() << "addIRModule failed: "
                     << llvm::toString(std::move(err)) << '\n';
        return 1;
    }

    std::cout << "running " << testNames.size() << " test"
              << (testNames.size() == 1 ? "" : "s") << '\n';
    using TestFn = std::int64_t (*)();
    int passed = 0;
    int failed = 0;
    for (const auto& name : testNames) {
        auto symOrErr = jit->lookup(name);
        if (!symOrErr) {
            // Should not happen (the fn typechecked + codegen'd), but treat a
            // lookup failure as a hard failure rather than crashing.
            std::cout << "test " << name << " ... FAILED (lookup error)\n";
            llvm::consumeError(symOrErr.takeError());
            ++failed;
            continue;
        }
        auto fn = symOrErr->toPtr<TestFn>();
        std::int64_t rc = fn();
        if (rc == 0) {
            std::cout << "test " << name << " ... ok\n";
            ++passed;
        } else {
            std::cout << "test " << name << " ... FAILED (returned " << rc
                      << ")\n";
            ++failed;
        }
    }
    std::cout << "test result: " << passed << " passed, " << failed
              << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // Minimal CLI: `-o <out> <file.kd>` switches into AOT mode; otherwise
    // we keep the historic positional `<file.kd>` (JIT) or REPL behaviour.
    std::string outPath;
    std::string inputPath;
    // Phase 14a: `-g` emits DWARF debug info (compile unit + per-fn
    // subprograms + line tables). Off by default so the historic codegen
    // path is byte-for-byte unchanged.
    bool emitDebug = false;
    // Phase 14b: `--no-cache` bypasses the incremental compile cache (always
    // recompiles + relinks; never reads or writes the cache dir).
    bool useCache = true;
    // Phase 14a: `--emit-llvm` prints textual LLVM IR (with debug metadata
    // when `-g` is also given) instead of JITing or emitting an object.
    bool emitIr = false;
    // Phase 20a: `--test` discovers + JIT-runs `test_*() -> i64` fns.
    bool testMode = false;
    // Phase 20a: optimization level for the post-codegen LLVM pipeline.
    // Default O2 — byte-for-byte the historic behavior when no `-O` is passed.
    kardashev::OptLevel optLevel = kardashev::OptLevel::O2;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a == "-g") {
            emitDebug = true;
        } else if (a == "--no-cache") {
            useCache = false;
        } else if (a == "--emit-llvm") {
            emitIr = true;
        } else if (a == "--test") {
            testMode = true;
        } else if (a == "-O0") {
            optLevel = kardashev::OptLevel::O0;
        } else if (a == "-O1") {
            optLevel = kardashev::OptLevel::O1;
        } else if (a == "-O2") {
            optLevel = kardashev::OptLevel::O2;
        } else if (a == "-O3") {
            optLevel = kardashev::OptLevel::O3;
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: kardc                     # interactive REPL\n"
                         "       kardc <file.kd>            # JIT-run main()\n"
                         "       kardc -o <out> <file.kd>   # AOT-compile to native exe\n"
                         "       kardc --test <file.kd>     # discover + run test_* fns\n"
                         "       kardc -O0|-O1|-O2|-O3 ...   # optimization level (default -O2)\n"
                         "       kardc -g ...               # emit DWARF debug info\n"
                         "       kardc --emit-llvm <file.kd> # print LLVM IR to stdout\n"
                         "       kardc --no-cache ...       # bypass the AOT compile cache\n";
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "kardc: unknown option `" << a << "`\n";
            return 2;
        } else {
            if (!inputPath.empty()) {
                std::cerr << "kardc: too many positional arguments\n";
                return 2;
            }
            inputPath = std::move(a);
        }
    }

    if (testMode) {
        if (inputPath.empty()) {
            std::cerr << "kardc: --test requires an input file\n";
            return 2;
        }
        auto src = readFile(inputPath);
        if (!src) {
            std::cerr << "kardc: cannot open file: " << inputPath << '\n';
            return 1;
        }
        return runTests(*src, dirOf(inputPath), emitDebug, inputPath,
                        optLevel);
    }
    if (emitIr) {
        if (inputPath.empty()) {
            std::cerr << "kardc: --emit-llvm requires an input file\n";
            return 2;
        }
        auto src = readFile(inputPath);
        if (!src) {
            std::cerr << "kardc: cannot open file: " << inputPath << '\n';
            return 1;
        }
        return emitLlvmIr(*src, dirOf(inputPath), emitDebug, inputPath,
                          optLevel);
    }
    if (!outPath.empty()) {
        if (inputPath.empty()) {
            std::cerr << "kardc: -o requires an input file\n";
            return 2;
        }
        auto src = readFile(inputPath);
        if (!src) {
            std::cerr << "kardc: cannot open file: " << inputPath << '\n';
            return 1;
        }
        return runAot(*src, outPath, dirOf(inputPath), emitDebug, inputPath,
                      useCache, optLevel);
    }
    if (!inputPath.empty()) {
        return runFile(inputPath.c_str(), emitDebug, optLevel);
    }
    return runREPL();
}
