// v24 Phase 132: a non-fatal lint pass, opt-in via `kardc -W`. It never fails
// the build — it returns warnings the driver renders (with the Phase 130 snippet
// renderer) and then compiles as usual.
//
// Current lints (sound — no false positives by construction):
//   - unused `let` bindings (a name never referenced anywhere in its function;
//     `_`-prefixed names are intentionally-unused and skipped);
//   - code unreachable after a `return` / `break` / `continue` in a block.
// Follow-on lints on this same pass (not yet): unused parameters (Param needs a
// source position first), shadowing, non-snake-case, unused imports.
#pragma once

#include "kardashev/ast.hpp"

#include <string>
#include <vector>

namespace kardashev {

struct Lint {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

std::vector<Lint> lint(const ast::Program& program);

} // namespace kardashev
