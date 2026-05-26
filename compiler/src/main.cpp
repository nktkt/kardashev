// kardc — kardashev V1 driver.
//
// Two modes:
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
// The REPL prompt is only emitted when stdin is a TTY, so piped input
// works cleanly (smoke_test exercises this).

#include "kardashev/codegen.hpp"
#include "kardashev/parser.hpp"
#include "kardashev/typecheck.hpp"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h> // isatty
#include <utility>

namespace {

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

// Compile `src` through the full pipeline and JIT-call the named entry
// (which must be a no-arg function returning i64). Diagnostics go to
// stderr. Returns the i64 result on success, nullopt otherwise.
std::optional<std::int64_t> compileAndRun(const std::string& src,
                                          const std::string& entry) {
    auto pr = kardashev::parse(src);
    if (!pr.ok()) {
        reportParseErrors(pr);
        return std::nullopt;
    }
    auto tcr = kardashev::typecheck(pr.program);
    if (!tcr.ok()) {
        reportTypeErrors(tcr);
        return std::nullopt;
    }
    auto cgr = kardashev::codegen(pr.program, tcr);
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

// Heuristic: a line whose first non-whitespace token is `fn ` or
// `struct ` is treated as a top-level decl to add to the accumulator.
// Anything else is parsed as an expression and evaluated.
bool looksLikeTopLevelDecl(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const auto rest = std::string_view(line).substr(i);
    return rest.substr(0, 3) == "fn " || rest.substr(0, 7) == "struct ";
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
            std::string trial = accumulated + line + "\n";
            auto pr = kardashev::parse(trial);
            if (!pr.ok()) {
                reportParseErrors(pr);
                continue;
            }
            auto tcr = kardashev::typecheck(pr.program);
            if (!tcr.ok()) {
                reportTypeErrors(tcr);
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
    std::ifstream f(path);
    if (!f) {
        std::cerr << "kardc: cannot open file: " << path << '\n';
        return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (auto result = compileAndRun(ss.str(), "main")) {
        std::cout << *result << '\n';
        return 0;
    }
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    if (argc == 2) {
        return runFile(argv[1]);
    }
    if (argc > 2) {
        std::cerr << "usage: kardc [path-to-kd-file]\n"
                     "       (no args = interactive REPL on stdin)\n";
        return 2;
    }
    return runREPL();
}
