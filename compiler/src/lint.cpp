// v24 Phase 132: the lint pass. See lint.hpp for scope + soundness rationale.
#include "kardashev/lint.hpp"

#include <algorithm>
#include <set>

namespace kardashev {
namespace {

using namespace ast;

// Per-function collector. To avoid false "unused" positives, a name counts as
// USED if it is referenced anywhere in the function via ANY name-bearing node —
// an IdentExpr, or a CallExpr callee (a call through a fn-pointer local). The
// walk visits every expression/statement kind, so a missed node can only cause
// an under-warning (a real unused var we don't flag), never a false positive.
struct Collector {
    std::set<std::string> uses;
    struct Site {
        std::string name;
        std::size_t line, column;
    };
    std::vector<Site> lets;
    std::vector<Lint> warns;

    static bool ignore(const std::string& n) {
        return n.empty() || n[0] == '_';
    }
    void bindLet(const std::string& n, std::size_t l, std::size_t c) {
        if (!ignore(n)) lets.push_back({n, l, c});
    }

    void expr(const Expr* e) {
        if (!e) return;
        if (auto* x = dynamic_cast<const IdentExpr*>(e)) {
            uses.insert(x->name);
        } else if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
            expr(b->lhs.get());
            expr(b->rhs.get());
        } else if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
            expr(u->operand.get());
        } else if (auto* c = dynamic_cast<const CallExpr*>(e)) {
            uses.insert(c->callee); // a fn-pointer local is called by name
            for (auto& a : c->args) expr(a.get());
        } else if (auto* cv = dynamic_cast<const CallValueExpr*>(e)) {
            expr(cv->callee.get());
            for (auto& a : cv->args) expr(a.get());
        } else if (auto* sl = dynamic_cast<const StructLitExpr*>(e)) {
            for (auto& f : sl->fields) expr(f.second.get());
        } else if (auto* fe = dynamic_cast<const FieldExpr*>(e)) {
            expr(fe->object.get());
        } else if (auto* iff = dynamic_cast<const IfExpr*>(e)) {
            expr(iff->cond.get());
            expr(iff->thenBranch.get());
            expr(iff->elseBranch.get());
        } else if (auto* blk = dynamic_cast<const BlockExpr*>(e)) {
            block(blk);
        } else if (auto* m = dynamic_cast<const MatchExpr*>(e)) {
            expr(m->scrutinee.get());
            for (auto& arm : m->arms) expr(arm.body.get());
        } else if (auto* w = dynamic_cast<const WhileExpr*>(e)) {
            expr(w->cond.get());
            expr(w->body.get());
        } else if (auto* lp = dynamic_cast<const LoopExpr*>(e)) {
            expr(lp->body.get());
        } else if (auto* rg = dynamic_cast<const RangeExpr*>(e)) {
            expr(rg->start.get());
            expr(rg->end.get());
        } else if (auto* fo = dynamic_cast<const ForExpr*>(e)) {
            expr(fo->iter.get());
            expr(fo->body.get());
        } else if (auto* br = dynamic_cast<const BreakExpr*>(e)) {
            expr(br->value.get());
        } else if (dynamic_cast<const ContinueExpr*>(e)) {
            // no children
        } else if (auto* tr = dynamic_cast<const TryExpr*>(e)) {
            expr(tr->operand.get());
        } else if (auto* rf = dynamic_cast<const RefExpr*>(e)) {
            expr(rf->operand.get());
        } else if (auto* sc = dynamic_cast<const SliceExpr*>(e)) {
            expr(sc->operand.get());
            expr(sc->start.get());
            expr(sc->end.get());
        } else if (auto* ar = dynamic_cast<const ArrayLitExpr*>(e)) {
            for (auto& el : ar->elements) expr(el.get());
            expr(ar->repeatCount.get());
        } else if (auto* ix = dynamic_cast<const IndexExpr*>(e)) {
            expr(ix->object.get());
            expr(ix->index.get());
        } else if (auto* tl = dynamic_cast<const TupleLitExpr*>(e)) {
            for (auto& el : tl->elements) expr(el.get());
        } else if (auto* tf = dynamic_cast<const TupleFieldExpr*>(e)) {
            expr(tf->object.get());
        } else if (auto* aw = dynamic_cast<const AwaitExpr*>(e)) {
            expr(aw->operand.get());
        } else if (auto* ca = dynamic_cast<const CastExpr*>(e)) {
            expr(ca->operand.get());
        } else if (auto* cl = dynamic_cast<const ClosureExpr*>(e)) {
            // Closure params are a separate binding scope (no source position on
            // ClosureParam yet, so they aren't lint sites); its body references
            // any captured outer names, which the walk records as uses.
            expr(cl->body.get());
        } else if (auto* mc = dynamic_cast<const MethodCallExpr*>(e)) {
            expr(mc->receiver.get());
            for (auto& a : mc->args) expr(a.get());
        } else if (auto* bx = dynamic_cast<const BoxNewExpr*>(e)) {
            expr(bx->value.get());
        }
        // else: a literal leaf — nothing to collect.
    }

    void stmt(const Stmt* s) {
        if (auto* let = dynamic_cast<const LetStmt*>(s)) {
            expr(let->value.get()); // the RHS is evaluated in the outer scope
            if (!let->tupleNames.empty()) {
                for (auto& n : let->tupleNames) bindLet(n, let->line, let->column);
            } else {
                bindLet(let->name, let->line, let->column);
            }
        } else if (auto* r = dynamic_cast<const ReturnStmt*>(s)) {
            expr(r->value.get());
        } else if (auto* es = dynamic_cast<const ExprStmt*>(s)) {
            expr(es->expr.get());
        } else if (auto* as = dynamic_cast<const AssignStmt*>(s)) {
            expr(as->target.get());
            expr(as->value.get());
        }
    }

    // A statement that unconditionally leaves the block: `return …;` or a bare
    // `break;` / `continue;`.
    static bool diverges(const Stmt* s) {
        if (dynamic_cast<const ReturnStmt*>(s)) return true;
        if (auto* es = dynamic_cast<const ExprStmt*>(s)) {
            const Expr* e = es->expr.get();
            return dynamic_cast<const BreakExpr*>(e) ||
                   dynamic_cast<const ContinueExpr*>(e);
        }
        return false;
    }

    void block(const BlockExpr* b) {
        bool dead = false, warned = false;
        for (const auto& sp : b->stmts) {
            const Stmt* s = sp.get();
            if (dead && !warned) {
                warns.push_back({"unreachable statement (after a `return` / "
                                 "`break` / `continue`)",
                                 s->line, s->column});
                warned = true;
            }
            stmt(s);
            if (diverges(s)) dead = true;
        }
        if (dead && !warned && b->tail) {
            warns.push_back({"unreachable expression (after a `return` / "
                             "`break` / `continue`)",
                             b->tail->line, b->tail->column});
        }
        expr(b->tail.get());
    }
};

} // namespace

std::vector<Lint> lint(const Program& prog) {
    std::vector<Lint> out;
    for (const auto& fn : prog.functions) {
        if (!fn.body) continue;
        Collector c;
        c.block(fn.body.get());
        for (const auto& site : c.lets)
            if (!c.uses.count(site.name))
                out.push_back({"unused variable `" + site.name + "`",
                               site.line, site.column});
        for (auto& w : c.warns) out.push_back(w);
    }
    std::sort(out.begin(), out.end(), [](const Lint& a, const Lint& b) {
        return a.line < b.line || (a.line == b.line && a.column < b.column);
    });
    return out;
}

} // namespace kardashev
