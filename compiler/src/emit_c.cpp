// v23 Phase 129: the C-source backend. See emit_c.hpp for scope + rationale.
#include "kardashev/emit_c.hpp"

#include <set>
#include <sstream>

namespace kardashev {
namespace {

using namespace ast;

struct CEmitter {
    std::ostringstream out;
    std::vector<std::string> errors;
    bool hasMain = false;

    void err(const std::string& m) { errors.push_back("emit-c: " + m); }
    bool ok() const { return errors.empty(); }

    // --- identifier hygiene: don't collide with C keywords / our own names ---
    static bool isReserved(const std::string& n) {
        static const std::set<std::string> kw = {
            "auto", "break", "case", "char", "const", "continue", "default",
            "do", "double", "else", "enum", "extern", "float", "for", "goto",
            "if", "inline", "int", "long", "register", "restrict", "return",
            "short", "signed", "sizeof", "static", "struct", "switch",
            "typedef", "union", "unsigned", "void", "volatile", "while",
            "_Bool", "main", "int64_t", "stdint"};
        return kw.count(n) > 0;
    }
    // A value name (param / let / const). Functions live in the `kd_`-prefixed
    // namespace, so a value literally named `kd_<fn>` would otherwise shadow a
    // function; mangle those (and C keywords) to a disjoint `kdv_` prefix.
    // Ordinary names pass through unchanged for readable output.
    static std::string vname(const std::string& n) {
        if (isReserved(n) || n.rfind("kd_", 0) == 0) return "kdv_" + n;
        return n;
    }

    static bool isScalar(const TypeRef& t) {
        return t.name == "i64" || t.name == "bool";
    }

    // --- binary / unary operator spellings (C == kardashev for the subset) ---
    const char* binOp(BinOp op) {
        switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Mod: return "%";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::Eq: return "==";
        case BinOp::NotEq: return "!=";
        case BinOp::And: return "&&";
        case BinOp::Or: return "||";
        case BinOp::BitAnd: return "&";
        case BinOp::BitOr: return "|";
        case BinOp::BitXor: return "^";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        }
        return nullptr;
    }

    // --- expression -> a C expression string (value type int64_t) ---
    std::string expr(const Expr& e) {
        if (auto* il = dynamic_cast<const IntLitExpr*>(&e))
            return "INT64_C(" + std::to_string(il->value) + ")";
        if (auto* bl = dynamic_cast<const BoolLitExpr*>(&e))
            return bl->value ? "INT64_C(1)" : "INT64_C(0)";
        if (auto* id = dynamic_cast<const IdentExpr*>(&e))
            return vname(id->name);
        if (auto* un = dynamic_cast<const UnaryExpr*>(&e)) {
            const char* op = nullptr;
            switch (un->op) {
            case UnaryOp::Neg: op = "-"; break;
            case UnaryOp::Not: op = "!"; break;
            case UnaryOp::BitNot: op = "~"; break;
            case UnaryOp::Deref:
                err("`*` (deref) is outside the C-backend subset");
                return "0";
            }
            return std::string("(") + op + expr(*un->operand) + ")";
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(&e)) {
            const char* op = binOp(bin->op);
            if (!op) { err("unknown binary operator"); return "0"; }
            return "(" + expr(*bin->lhs) + " " + op + " " + expr(*bin->rhs) +
                   ")";
        }
        if (auto* call = dynamic_cast<const CallExpr*>(&e)) {
            std::string s = "kd_" + call->callee + "(";
            for (std::size_t i = 0; i < call->args.size(); ++i) {
                if (i) s += ", ";
                s += expr(*call->args[i]);
            }
            return s + ")";
        }
        if (auto* iff = dynamic_cast<const IfExpr*>(&e)) {
            return "(" + expr(*iff->cond) + " ? " + expr(*iff->thenBranch) +
                   " : " + expr(*iff->elseBranch) + ")";
        }
        if (auto* blk = dynamic_cast<const BlockExpr*>(&e))
            return blockValue(*blk);
        if (auto* wh = dynamic_cast<const WhileExpr*>(&e)) {
            auto* body = dynamic_cast<const BlockExpr*>(wh->body.get());
            if (!body) { err("while body is not a block"); return "0"; }
            // unit-valued: a statement-expression that runs the loop then 0.
            return "({ while (" + expr(*wh->cond) + ") " + blockStmts(*body) +
                   " INT64_C(0); })";
        }
        if (auto* br = dynamic_cast<const BreakExpr*>(&e)) {
            if (br->value) {
                err("`break <value>` is outside the C-backend subset");
                return "0";
            }
            return "({ break; INT64_C(0); })";
        }
        if (dynamic_cast<const ContinueExpr*>(&e))
            return "({ continue; INT64_C(0); })";
        err("unsupported expression in the C backend (outside the i64/bool "
            "subset)");
        return "0";
    }

    // A block used as a VALUE: `({ stmts...; tail; })`. A unit block (no tail)
    // yields 0 so it still type-checks as int64_t in expression position.
    std::string blockValue(const BlockExpr& b) {
        std::string s = "({ ";
        for (const auto& st : b.stmts) s += stmt(*st) + " ";
        s += b.tail ? (expr(*b.tail) + ";") : "INT64_C(0);";
        s += " })";
        return s;
    }

    // A block used for EFFECT (a while body): `{ stmts...; tail-for-effect; }`.
    std::string blockStmts(const BlockExpr& b) {
        std::string s = "{ ";
        for (const auto& st : b.stmts) s += stmt(*st) + " ";
        if (b.tail) s += expr(*b.tail) + ";";
        return s + " }";
    }

    // --- statement -> a C statement string ---
    std::string stmt(const Stmt& s) {
        if (auto* let = dynamic_cast<const LetStmt*>(&s)) {
            if (!let->tupleNames.empty()) {
                err("tuple-destructuring `let` is outside the C-backend subset");
                return ";";
            }
            if (let->annotation && !isScalar(*let->annotation)) {
                err("`let` annotated with a non-scalar type is outside the "
                    "C-backend subset");
                return ";";
            }
            // `const` qualifier omitted: a non-`mut` binding is never reassigned
            // (the type checker enforced that), so plain `int64_t` is faithful.
            return "int64_t " + vname(let->name) + " = " + expr(*let->value) +
                   ";";
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(&s)) {
            // A kardashev `return` returns from the function; a C `return`
            // inside a `({ ... })` statement-expression does the same.
            return ret->value ? ("return " + expr(*ret->value) + ";")
                              : "return INT64_C(0);";
        }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s))
            return expr(*es->expr) + ";";
        if (auto* as = dynamic_cast<const AssignStmt*>(&s)) {
            auto* id = dynamic_cast<const IdentExpr*>(as->target.get());
            if (!id) {
                err("assignment to a non-variable place (field/index) is "
                    "outside the C-backend subset");
                return ";";
            }
            return vname(id->name) + " = " + expr(*as->value) + ";";
        }
        err("unsupported statement in the C backend");
        return ";";
    }

    // --- a function signature `int64_t kd_<name>(<params>)` ---
    std::string signature(const FnDecl& fn) {
        std::string s = "int64_t kd_" + fn.name + "(";
        if (fn.params.empty()) {
            s += "void";
        } else {
            for (std::size_t i = 0; i < fn.params.size(); ++i) {
                if (i) s += ", ";
                s += "int64_t " + vname(fn.params[i].name);
            }
        }
        return s + ")";
    }

    bool fnInSubset(const FnDecl& fn) {
        if (!fn.genericParams.empty()) {
            err("generic fn `" + fn.name + "` is outside the C-backend subset");
            return false;
        }
        if (fn.isAsync) {
            err("async fn `" + fn.name + "` is outside the C-backend subset");
            return false;
        }
        if (!isScalar(fn.returnType)) {
            err("fn `" + fn.name + "` returns `" + fn.returnType.name +
                "` (the C backend handles i64/bool only)");
            return false;
        }
        for (const auto& p : fn.params) {
            if (!isScalar(p.type)) {
                err("param `" + p.name + "` of fn `" + fn.name + "` has type `" +
                    p.type.name + "` (the C backend handles i64/bool only)");
                return false;
            }
        }
        return true;
    }

    void emitProgram(const Program& program) {
        if (!program.structs.empty() || !program.enums.empty() ||
            !program.traits.empty() || !program.impls.empty() ||
            !program.externFns.empty() || !program.mods.empty()) {
            err("the program uses structs/enums/traits/impls/extern/mod — "
                "outside the C-backend subset (i64/bool functions + const only)");
            return;
        }

        out << "/* Generated by kardashev --emit-c (v23 Phase 129). */\n";
        out << "#include <stdint.h>\n\n";

        // Top-level consts (a const may reference an earlier one, so keep order).
        for (const auto& c : program.consts) {
            if (!isScalar(c.type)) {
                err("const `" + c.name + "` has type `" + c.type.name +
                    "` (the C backend handles i64/bool only)");
                continue;
            }
            out << "static const int64_t " << vname(c.name) << " = "
                << expr(*c.value) << ";\n";
        }
        if (!program.consts.empty()) out << "\n";

        // Forward prototypes first — kardashev allows calling a fn defined later
        // (forward references + mutual recursion).
        for (const auto& fn : program.functions) {
            if (!fnInSubset(fn)) return;
            out << signature(fn) << ";\n";
            if (fn.name == "main") hasMain = true;
        }
        out << "\n";

        // Definitions.
        for (const auto& fn : program.functions) {
            if (!fn.body) {
                err("fn `" + fn.name + "` has no body");
                return;
            }
            out << signature(fn) << " {\n  return " << blockValue(*fn.body)
                << ";\n}\n\n";
            if (!ok()) return;
        }

        // The C entry point: kardashev `main` (no params in the subset) becomes
        // the process exit code, matching the LLVM backend's `ret i64` from main
        // truncated to the low byte.
        if (hasMain) {
            out << "int main(void) { return (int)kd_main(); }\n";
        }
    }
};

} // namespace

EmitCResult emit_c(const ast::Program& program) {
    CEmitter em;
    em.emitProgram(program);
    EmitCResult r;
    if (!em.errors.empty()) {
        r.success = false;
        r.errors = std::move(em.errors);
        return r;
    }
    r.success = true;
    r.code = em.out.str();
    return r;
}

} // namespace kardashev
