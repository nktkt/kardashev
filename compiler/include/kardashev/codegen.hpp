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
    // v28 Phase 156: the mangled names of the GENERIC instances monomorphized
    // for this program (each emitted exactly once — codegen dedups). `kardc
    // --mono-report` prints these so the monomorphization footprint (a source of
    // code bloat) is visible.
    std::vector<std::string> monomorphizedInstances;
    bool ok() const { return errors.empty(); }
};

// Phase 20a: optimization level for the post-codegen LLVM pass pipeline.
// O2 is the default (matches the historic hardcoded pipeline, so behavior is
// unchanged when no `-O` flag is passed). O0 runs LLVM's minimal O0 pipeline
// (no inlining / instcombine / GVN), leaving the alloca-heavy, wrapper-laden
// IR largely as codegen emitted it. O1/O2/O3 select the matching
// `llvm::OptimizationLevel` via buildPerModuleDefaultPipeline. The numeric
// values are stable identifiers used in the incremental-cache key so objects
// built at different levels never collide.
enum class OptLevel : int { O0 = 0, O1 = 1, O2 = 2, O3 = 3 };

// Generate an LLVM module from a parsed + type-checked program. The
// returned `context` MUST outlive any use of `module` (move both into
// a `ThreadSafeModule` together when handing to LLJIT).
//
// Phase 14a: when `emitDebugInfo` is true, codegen additionally emits DWARF
// debug info (a DICompileUnit, a DISubprogram per kardashev function, line/
// column DILocations on instructions, and DILocalVariable + dbg.declare for
// parameters / locals), sets the module's "Debug Info Version" + "Dwarf
// Version" flags, and finalizes the DIBuilder. `sourceFile` names the source
// for the DIFile (defaults to a placeholder when unknown). When the flag is
// false the emitted module is byte-for-byte identical to the historic path.
//
// Phase 20a: `optLevel` selects the post-codegen LLVM optimization pipeline
// (default O2 — byte-for-byte the historic behavior). It is appended last so
// existing 2-/3-/4-argument callers (incl. the unit tests) are unaffected.
CodegenResult codegen(const ast::Program& program,
                       const TypeCheckResult& tc,
                       bool emitDebugInfo = false,
                       const std::string& sourceFile = "<kardashev>",
                       OptLevel optLevel = OptLevel::O2);

} // namespace kardashev
