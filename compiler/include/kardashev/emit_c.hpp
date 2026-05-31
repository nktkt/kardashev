// v23 Phase 129: a second backend — emit portable C source from the
// typechecked AST, compiled by the system C compiler. It is the most
// *verifiable* backend in this environment (a C toolchain is present), so it
// is differentially gated against the LLVM backend (smoke_test_phase129).
//
// SCOPE (a deliberate subset, grown phase by phase): i64 / bool, the full
// operator set (arithmetic / comparison / logical `&&` `||` / bitwise / unary
// `-` `!` `~`), `let` (incl. `mut` + assignment), `if`/`else` as a value,
// `while`, blocks, direct function calls + recursion, and top-level `const`.
// Anything outside the subset (structs, enums, match, strings, Vec, closures,
// Drop, references, generics, async, for/loop-with-value, ...) is REFUSED with
// a clear error rather than miscompiled — the backend never emits wrong C.
//
// Everything maps to `int64_t` (bool is 0/1), which is sound for the subset:
// C two's-complement `+ - *` (with `-fwrapv`), truncating-toward-zero `/`,
// dividend-signed `%`, arithmetic `>>` on a signed value, and short-circuit
// `&&`/`||` all match kardashev's i64 semantics. Expression-oriented
// constructs (block / if / while as a value) use GNU statement-expressions
// `({ ... })`, supported by both clang and gcc.
#pragma once

#include "kardashev/ast.hpp"

#include <string>
#include <vector>

namespace kardashev {

struct EmitCResult {
    bool success = false;
    std::string code;                // the generated C source (valid iff success)
    std::vector<std::string> errors; // why emission failed (if !success)
    bool ok() const { return success && errors.empty(); }
};

// Emit C source for `program`. The AST is assumed already typechecked +
// borrow-checked (the caller runs the front-end), so this walk only handles
// lowering and subset enforcement, not type errors.
EmitCResult emit_c(const ast::Program& program);

} // namespace kardashev
