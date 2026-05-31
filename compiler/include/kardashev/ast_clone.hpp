// v25 Phase 135: deep-clone AST expression/statement/pattern subtrees. The AST
// is built from move-only `unique_ptr`s, so synthesizing a node from an existing
// one (e.g. filling a trait default-method body into each impl that doesn't
// override it) needs a structural copy. Clones the SOURCE fields only — the
// typechecker-populated fields (ClosureExpr::captures, ForExpr desugar) are left
// empty to be refilled by the next typecheck.
#pragma once

#include "kardashev/ast.hpp"

namespace kardashev::ast {

ExprPtr cloneExpr(const Expr& e);
StmtPtr cloneStmt(const Stmt& s);
std::unique_ptr<BlockExpr> cloneBlock(const BlockExpr& b);
PatternPtr clonePattern(const Pattern& p);

} // namespace kardashev::ast
