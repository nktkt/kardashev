// Parser for kardashev V1. Recursive-descent for top-level + statements,
// Pratt-style precedence climbing for binary expressions.
//
// Error policy: errors are collected into ParseResult::errors. Parsing
// continues best-effort but bails after >20 errors. The returned program
// may be partial / contain dummy nodes when errors are present; consumers
// must check `ok()`.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "kardashev/ast.hpp"

namespace kardashev {

struct ParseError {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct ParseResult {
    ast::Program program;
    std::vector<ParseError> errors;
    bool ok() const { return errors.empty(); }
};

ParseResult parse(std::string_view source);

} // namespace kardashev
