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
    // Phase 13a: the `Iterator` trait + its impl for the built-in `Range`.
    // Element type is i64 (the accepted MVP). Adding `impl Iterator for
    // Range` lets `for x in <range>` route through `next()` and lets ranges
    // feed the iterator adaptors (fold/map/filter). `for` over literal
    // ranges keeps its Phase 9 fast path in codegen, so this impl is only
    // exercised when a Range is used as a generic `Iterator`.
    if (userSrc.find("trait Iterator") == std::string::npos) {
        prelude +=
            "trait Iterator { fn next(&mut self) -> Option<i64>; }\n"
            "impl Iterator for Range {\n"
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
                                          const std::string& srcDir = ".") {
    auto progOpt = buildProgram(srcRaw, srcDir);
    if (!progOpt) return std::nullopt;
    auto& program = *progOpt;
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
    auto cgr = kardashev::codegen(program, tcr);
    if (!cgr.ok()) {
        for (const auto& msg : cgr.errors) {
            std::cerr << "codegen error: " << msg << '\n';
        }
        return std::nullopt;
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
    return fn();
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

int runFile(const char* path) {
    auto src = readFile(path);
    if (!src) {
        std::cerr << "kardc: cannot open file: " << path << '\n';
        return 1;
    }
    if (auto result = compileAndRun(*src, "main", dirOf(path))) {
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
    auto* truncated = b.CreateTrunc(result, i32Ty, "exitcode");
    b.CreateRet(truncated);
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

// Compile + link `src` into a native executable at `outExePath`. Uses
// the system `clang` to link the emitted object — this is the same
// linker our CI / Bazel build already depends on, so AOT mode adds no
// new system requirement beyond what JIT mode needed.
int runAot(const std::string& srcRaw, const std::string& outExePath,
            const std::string& srcDir = ".") {
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
    auto cgr = kardashev::codegen(program, tcr);
    if (!cgr.ok()) {
        for (const auto& msg : cgr.errors) {
            std::cerr << "codegen error: " << msg << '\n';
        }
        return 1;
    }

    addCMainWrapper(*cgr.module);

    std::string objPath = outExePath + ".o";
    if (!emitObject(*cgr.module, objPath)) return 1;

    // Link with clang. We use `clang` from PATH; the CI image has it,
    // and Bazel's LLVM-detection step already pulled it in for the
    // build. -fuse-ld is left to clang's default so it picks the right
    // linker per platform (lld on Linux if installed, ld64 on macOS).
    std::string cmd = "clang \"" + objPath + "\" -o \"" + outExePath + "\"";
    int rc = std::system(cmd.c_str());
    // Always try to clean up the intermediate object so the working
    // tree stays tidy even when linking fails.
    std::remove(objPath.c_str());
    if (rc != 0) {
        std::cerr << "kardc: linker (clang) failed with exit code "
                  << rc << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // Minimal CLI: `-o <out> <file.kd>` switches into AOT mode; otherwise
    // we keep the historic positional `<file.kd>` (JIT) or REPL behaviour.
    std::string outPath;
    std::string inputPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: kardc                     # interactive REPL\n"
                         "       kardc <file.kd>            # JIT-run main()\n"
                         "       kardc -o <out> <file.kd>   # AOT-compile to native exe\n";
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
        return runAot(*src, outPath, dirOf(inputPath));
    }
    if (!inputPath.empty()) {
        return runFile(inputPath.c_str());
    }
    return runREPL();
}
