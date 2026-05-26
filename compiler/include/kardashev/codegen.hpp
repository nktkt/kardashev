// LLVM IR code generation for kardashev V1.
//
// Walks a parsed + type-checked `ast::Program` and emits an LLVM
// `Module` containing one function per `ast::FnDecl`. The resulting
// module is in a state suitable for either AOT compilation (Phase 5)
// or direct JIT execution via ORC v2 (the REPL in Phase 1.5).
//
// Strategy:
//   Pass 1 (declareAllFunctions): create LLVM Function declarations for
//   every fn in the program, BEFORE emitting any body — needed so
//   self-recursion and forward references both resolve.
//
//   Pass 2 (emitFunction): for each fn, create the entry basic block,
//   alloca each parameter (then store the SSA value into it), and
//   recursively emit the body. Parameter loads and let-bound locals
//   share the same alloca/load/store treatment so LLVM's mem2reg pass
//   can clean it up uniformly.
//
//   Control flow:
//     - if/else uses three basic blocks (then / else / merge) plus a
//       phi node in the merge block. If exactly one branch terminates
//       (e.g. via `return`), the merge block still works — the value
//       from the live branch flows through directly. If both
//       terminate, the merge is marked `unreachable`.
//     - block expressions emit each stmt in order; a `return` mid-
//       block aborts further emission for that block.
//
// The result is verified via `llvm::verifyModule` before being handed
// back; any verifier complaint is surfaced through `errors`.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "kardashev/ast.hpp"
#include "kardashev/typecheck.hpp"

namespace kardashev {

struct CodegenResult {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::vector<std::string> errors;
    bool ok() const { return errors.empty(); }
};

// Generate an LLVM module from a parsed + type-checked program. The
// returned `context` MUST outlive any use of `module` (move both into
// a `ThreadSafeModule` together when handing to LLJIT).
CodegenResult codegen(const ast::Program& program,
                       const TypeCheckResult& tc);

} // namespace kardashev
