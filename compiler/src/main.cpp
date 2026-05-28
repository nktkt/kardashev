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
// `struct `, or `enum ` is treated as a top-level decl to add to
// the accumulator. Anything else is parsed as an expression and
// evaluated.
bool looksLikeTopLevelDecl(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const auto rest = std::string_view(line).substr(i);
    return rest.substr(0, 3) == "fn " ||
           rest.substr(0, 7) == "struct " ||
           rest.substr(0, 5) == "enum ";
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
    auto* mainTy = llvm::FunctionType::get(i32Ty, {}, /*isVarArg=*/false);
    auto* cmain = llvm::Function::Create(
        mainTy, llvm::Function::ExternalLinkage, "main", &mod);
    auto* entry = llvm::BasicBlock::Create(ctx, "entry", cmain);
    llvm::IRBuilder<> b(entry);
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
