// Canonical AST pretty-printer for kardashev (see ast_print.hpp).
//
// The printer walks the AST and appends to a std::string. Indentation is
// tracked as an integer depth multiplied by 4 spaces. Every grammar node in
// ast.hpp has a corresponding emit path here; unknown node kinds (which
// would indicate a grammar addition that skipped the formatter) fall through
// to a clearly-marked placeholder rather than crashing, but the test suite
// exercises every node so this should never fire in practice.

#include "kardashev/ast_print.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace kardashev {
namespace {

using namespace kardashev::ast;

class Printer {
public:
    std::string run(const Program& p) {
        bool first = true;
        // Stable, deterministic ordering of top-level items. `mod` first so
        // imports stay at the top (matches idiomatic source); then types,
        // traits, impls, and finally the functions.
        for (const auto& m : p.mods) {
            sep(first);
            printMod(m);
        }
        for (const auto& s : p.structs) {
            sep(first);
            printStruct(s);
        }
        for (const auto& e : p.enums) {
            sep(first);
            printEnum(e);
        }
        for (const auto& t : p.traits) {
            sep(first);
            printTrait(t);
        }
        for (const auto& i : p.impls) {
            sep(first);
            printImpl(i);
        }
        for (const auto& ef : p.externFns) {
            sep(first);
            printExtern(ef);
        }
        // Phase 25: top-level `const` items.
        for (const auto& cd : p.consts) {
            sep(first);
            printConst(cd);
        }
        for (const auto& f : p.functions) {
            sep(first);
            printFn(f, /*indent=*/0);
        }
        return out_;
    }

private:
    std::string out_;

    // Emit a single blank-line separator between top-level items (but never
    // before the first item or after the last).
    void sep(bool& first) {
        if (!first) out_ += "\n";
        first = false;
    }

    void indent(int depth) { out_.append(static_cast<std::size_t>(depth) * 4, ' '); }

    // --- Types ---

    std::string typeToString(const TypeRef& t) {
        std::string s;
        if (t.isFn) {
            // `fn(P0, P1) -> R ! { e }`
            s += "fn(";
            for (std::size_t i = 0; i < t.fnParams.size(); ++i) {
                if (i) s += ", ";
                s += typeToString(t.fnParams[i]);
            }
            s += ") -> ";
            s += t.fnRet ? typeToString(*t.fnRet) : std::string("()");
            s += effectsToString(t.fnEffects);
            // A function type can itself be borrowed (`&fn(...) -> T`), though
            // that is rare; honor isRef for completeness.
            if (t.isRef) s = std::string(t.refIsMut ? "&mut " : "&") + s;
            return s;
        }
        if (t.isSlice) {
            // `&[T]` / `&mut [T]` — a slice is always spelled behind `&`.
            std::string elem = t.typeArgs.empty() ? std::string()
                                                  : typeToString(t.typeArgs[0]);
            std::string ref = t.refIsMut ? "&mut " : "&";
            return ref + "[" + elem + "]";
        }
        // Phase 22 / 25: a fixed-size array type `[T; N]` (optionally
        // `&[T; N]`). N is either a literal length (Phase 22, in `arrayLen`)
        // or a const-expr (Phase 25, in `arrayLenExpr`) — print whichever the
        // source used so the formatter round-trips a `[i64; sq(2)]` faithfully.
        if (t.isArray) {
            std::string elem = t.typeArgs.empty() ? std::string()
                                                  : typeToString(t.typeArgs[0]);
            std::string len = t.arrayLenExpr ? exprToString(*t.arrayLenExpr)
                                             : std::to_string(t.arrayLen);
            std::string s = "[" + elem + "; " + len + "]";
            if (t.isRef) s = std::string(t.refIsMut ? "&mut " : "&") + s;
            return s;
        }
        // Phase 22: a tuple type `(A, B, ...)`.
        if (t.isTuple) {
            std::string s = "(";
            for (std::size_t i = 0; i < t.tupleElems.size(); ++i) {
                if (i) s += ", ";
                s += typeToString(t.tupleElems[i]);
            }
            s += ")";
            if (t.isRef) s = std::string(t.refIsMut ? "&mut " : "&") + s;
            return s;
        }
        std::string ref;
        if (t.isRef) ref = t.refIsMut ? "&mut " : "&";
        std::string core;
        if (t.isDyn) {
            core = "dyn " + t.name;
        } else {
            core = t.name;
            // Phase 21b: an associated-type projection `Base::Assoc`.
            if (!t.assocName.empty()) core += "::" + t.assocName;
            if (!t.typeArgs.empty()) {
                core += "<";
                for (std::size_t i = 0; i < t.typeArgs.size(); ++i) {
                    if (i) core += ", ";
                    core += typeToString(t.typeArgs[i]);
                }
                core += ">";
            }
        }
        return ref + core;
    }

    std::string effectsToString(const std::vector<std::string>& labels) {
        if (labels.empty()) return "";
        std::string s = " ! { ";
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (i) s += ", ";
            s += labels[i];
        }
        s += " }";
        return s;
    }

    std::string genericParamsToString(const std::vector<TypeParam>& gps) {
        if (gps.empty()) return "";
        std::string s = "<";
        for (std::size_t i = 0; i < gps.size(); ++i) {
            if (i) s += ", ";
            s += gps[i].name;
            if (!gps[i].bound.empty()) {
                s += ": " + gps[i].bound;
                // Phase 21a: parameterized bound `I: Iterator<T>`.
                if (!gps[i].boundTypeArgs.empty()) {
                    s += "<";
                    for (std::size_t j = 0; j < gps[i].boundTypeArgs.size();
                         ++j) {
                        if (j) s += ", ";
                        s += typeToString(gps[i].boundTypeArgs[j]);
                    }
                    s += ">";
                }
                // Phase 28: additional bounds `T: A + B`.
                for (const auto& eb : gps[i].extraBounds) s += " + " + eb;
            }
        }
        s += ">";
        return s;
    }

    std::string paramsToString(const std::vector<Param>& params) {
        std::string s;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i) s += ", ";
            // The conventional `self` / `&self` receiver is stored as a Param
            // named "self" whose TypeRef name is "Self". Render it as the bare
            // receiver form the parser accepts.
            if (params[i].name == "self") {
                const TypeRef& ty = params[i].type;
                if (ty.name == "Self" && !ty.isDyn && ty.typeArgs.empty()) {
                    if (ty.isRef) {
                        s += ty.refIsMut ? "&mut self" : "&self";
                    } else {
                        s += "self";
                    }
                    continue;
                }
            }
            s += params[i].name + ": " + typeToString(params[i].type);
        }
        return s;
    }

    // --- Top-level decls ---

    void printMod(const ModDecl& m) {
        out_ += "mod " + m.name + ";\n";
    }

    // Phase 25: `[pub ]const NAME: T = <expr>;`.
    void printConst(const ConstDecl& cd) {
        if (cd.isPub) out_ += "pub ";
        out_ += "const " + cd.name + ": " + typeToString(cd.type) + " = ";
        if (cd.value) printExpr(*cd.value, /*depth=*/0, /*parentPrec=*/0);
        out_ += ";\n";
    }

    // Phase 24: `extern "C" fn name(params) -> ret [! { ... }];` — a bodyless
    // FFI declaration. Rendered in the per-decl form (round-trips through the
    // parser, which accepts both the per-decl and block forms). The return
    // type is omitted from the surface when it's `unit` (mirroring printFn).
    void printExtern(const ExternFn& ef) {
        out_ += "extern \"" + ef.abi + "\" fn " + ef.name + "(";
        out_ += paramsToString(ef.params);
        out_ += ")";
        bool retUnit = !ef.returnType.isRef && !ef.returnType.isFn &&
                       !ef.returnType.isSlice && !ef.returnType.isArray &&
                       !ef.returnType.isTuple && ef.returnType.assocName.empty() &&
                       !ef.returnType.isDyn && ef.returnType.name == "unit";
        if (!retUnit) out_ += " -> " + typeToString(ef.returnType);
        // An explicit effect row (even an empty `! { }`) is preserved; the
        // default (no row -> io) is left implicit.
        if (ef.hasExplicitEffects) {
            if (ef.effects.labels.empty()) out_ += " ! { }";
            else out_ += effectsToString(ef.effects.labels);
        }
        out_ += ";\n";
    }

    void printStruct(const StructDecl& s) {
        if (s.isPub) out_ += "pub ";
        out_ += "struct " + s.name + genericParamsToString(s.genericParams);
        if (s.fields.empty()) {
            out_ += " {}\n";
            return;
        }
        out_ += " {\n";
        for (std::size_t i = 0; i < s.fields.size(); ++i) {
            indent(1);
            out_ += s.fields[i].name + ": " + typeToString(s.fields[i].type);
            out_ += ",\n";
        }
        out_ += "}\n";
    }

    void printEnum(const EnumDecl& e) {
        if (e.isPub) out_ += "pub ";
        out_ += "enum " + e.name + genericParamsToString(e.genericParams);
        if (e.variants.empty()) {
            out_ += " {}\n";
            return;
        }
        out_ += " {\n";
        for (const auto& v : e.variants) {
            indent(1);
            out_ += v.name;
            if (!v.payloadTypes.empty()) {
                out_ += "(";
                for (std::size_t i = 0; i < v.payloadTypes.size(); ++i) {
                    if (i) out_ += ", ";
                    out_ += typeToString(v.payloadTypes[i]);
                }
                out_ += ")";
            }
            out_ += ",\n";
        }
        out_ += "}\n";
    }

    void printTrait(const TraitDecl& t) {
        if (t.isPub) out_ += "pub ";
        out_ += "trait " + t.name + genericParamsToString(t.genericParams);
        if (t.methods.empty() && t.assocTypes.empty()) {
            out_ += " {}\n";
            return;
        }
        out_ += " {\n";
        // Phase 21b: associated-type declarations precede the methods.
        for (const auto& at : t.assocTypes) {
            indent(1);
            out_ += "type " + at.name + ";\n";
        }
        for (const auto& m : t.methods) {
            indent(1);
            out_ += "fn " + m.name + "(" + paramsToString(m.params) + ") -> " +
                    typeToString(m.returnType) + effectsToString(m.effects.labels) +
                    ";\n";
        }
        out_ += "}\n";
    }

    void printImpl(const ImplDecl& im) {
        if (im.isPub) out_ += "pub ";
        // Phase 15: inherent impls print as `impl Type { ... }` (no trait).
        if (im.isInherent()) {
            out_ += "impl " + typeToString(im.forType);
        } else {
            std::string traitRef = im.traitName;
            // Phase 21a: render the trait's type args (`impl Iterator<i64> for
            // Range`). Build a TypeRef so typeToString formats the `<...>`.
            if (!im.traitTypeArgs.empty()) {
                TypeRef tr;
                tr.name = im.traitName;
                tr.typeArgs = im.traitTypeArgs;
                traitRef = typeToString(tr);
            }
            out_ += "impl " + traitRef + " for " + typeToString(im.forType);
        }
        if (im.methods.empty() && im.assocTypes.empty()) {
            out_ += " {}\n";
            return;
        }
        out_ += " {\n";
        // Phase 21b: associated-type definitions precede the methods.
        for (const auto& at : im.assocTypes) {
            indent(1);
            out_ += "type " + at.name + " = " + typeToString(at.type) + ";\n";
        }
        bool firstMethod = true;
        for (const auto& m : im.methods) {
            if (!firstMethod) out_ += "\n";
            firstMethod = false;
            printFn(m, /*indent=*/1);
        }
        out_ += "}\n";
    }

    void printFn(const FnDecl& fn, int depth) {
        indent(depth);
        if (fn.isPub) out_ += "pub ";
        if (fn.isConst) out_ += "const "; // Phase 25
        if (fn.isAsync) out_ += "async ";
        out_ += "fn " + fn.name + genericParamsToString(fn.genericParams) + "(" +
                paramsToString(fn.params) + ") -> " + typeToString(fn.returnType) +
                effectsToString(fn.effects.labels) + " ";
        // Body is always a block; emit it inline with the header.
        printBlock(fn.body.get(), depth);
        out_ += "\n";
    }

    // --- Statements & blocks ---

    void printBlock(const BlockExpr* bp, int depth) {
        if (!bp) {
            // A control-flow body that wasn't a BlockExpr (malformed AST).
            // Emit an empty block so output stays syntactically valid.
            out_ += "{}";
            return;
        }
        const BlockExpr& b = *bp;
        if (b.stmts.empty() && !b.tail) {
            out_ += "{}";
            return;
        }
        out_ += "{\n";
        for (const auto& s : b.stmts) {
            indent(depth + 1);
            printStmt(*s, depth + 1);
            out_ += "\n";
        }
        if (b.tail) {
            indent(depth + 1);
            printExpr(*b.tail, depth + 1, /*parentPrec=*/0);
            out_ += "\n";
        }
        indent(depth);
        out_ += "}";
    }

    void printStmt(const Stmt& s, int depth) {
        if (auto* let = dynamic_cast<const LetStmt*>(&s)) {
            out_ += "let ";
            if (let->isMut) out_ += "mut ";
            // Phase 22: tuple-destructuring `let (x, y) = ...;`.
            if (!let->tupleNames.empty()) {
                out_ += "(";
                for (std::size_t i = 0; i < let->tupleNames.size(); ++i) {
                    if (i) out_ += ", ";
                    out_ += let->tupleNames[i];
                }
                out_ += ")";
            } else {
                out_ += let->name;
                if (let->annotation)
                    out_ += ": " + typeToString(*let->annotation);
            }
            out_ += " = ";
            printExpr(*let->value, depth, /*parentPrec=*/0);
            out_ += ";";
            return;
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(&s)) {
            out_ += "return";
            if (ret->value) {
                out_ += " ";
                printExpr(*ret->value, depth, /*parentPrec=*/0);
            }
            out_ += ";";
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(&s)) {
            printExpr(*as->target, depth, /*parentPrec=*/0);
            out_ += " = ";
            printExpr(*as->value, depth, /*parentPrec=*/0);
            out_ += ";";
            return;
        }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) {
            printExpr(*es->expr, depth, /*parentPrec=*/0);
            out_ += ";";
            return;
        }
        out_ += "/* <unknown stmt> */";
    }

    // --- Expressions ---

    // Binary-operator precedence mirrors the parser (parser.cpp binPrec):
    //   comparisons = 1, additive = 2, multiplicative = 3.
    static int binOpPrec(BinOp op) {
        switch (op) {
        case BinOp::And:   // Phase 33: `&&` binds loosest
            return 1;
        case BinOp::Lt:
        case BinOp::Le:
        case BinOp::Gt:
        case BinOp::Ge:
        case BinOp::Eq:
        case BinOp::NotEq:
            return 2;
        case BinOp::Add:
        case BinOp::Sub:
            return 3;
        case BinOp::Mul:
        case BinOp::Div:
        case BinOp::Mod:
            return 4;
        }
        return 1;
    }

    static const char* binOpSpelling(BinOp op) {
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
        }
        return "+";
    }

    static std::string escapeString(const std::string& v) {
        // Inverse of the lexer's decode: it understands \n \\ \" \t.
        std::string s;
        s.reserve(v.size() + 2);
        for (char c : v) {
            switch (c) {
            case '\n': s += "\\n"; break;
            case '\t': s += "\\t"; break;
            case '\\': s += "\\\\"; break;
            case '"': s += "\\\""; break;
            default: s.push_back(c);
            }
        }
        return s;
    }

    // `parentPrec` is the precedence the surrounding context demands: a child
    // binary expr is wrapped in parens when its own precedence is lower than
    // what the parent needs to preserve the parse tree. Non-binary primaries
    // ignore it.
    // Phase 25: render an expression to a standalone string (used by
    // typeToString for a const-expr array length). Captures printExpr's
    // output into a temporary buffer so it can be embedded mid-type.
    std::string exprToString(const Expr& e) {
        std::string saved = std::move(out_);
        out_.clear();
        printExpr(e, /*depth=*/0, /*parentPrec=*/0);
        std::string result = std::move(out_);
        out_ = std::move(saved);
        return result;
    }

    void printExpr(const Expr& e, int depth, int parentPrec) {
        if (auto* lit = dynamic_cast<const IntLitExpr*>(&e)) {
            out_ += std::to_string(lit->value);
            return;
        }
        // Phase 15: boolean literal.
        if (auto* bl = dynamic_cast<const BoolLitExpr*>(&e)) {
            out_ += bl->value ? "true" : "false";
            return;
        }
        // Phase 15: prefix unary operator. It binds tighter than every binary
        // operator, so wrap a binary operand in parens (`-(a + b)`); atoms and
        // postfix chains print bare (`-x`, `!a.b`, `- -3`).
        if (auto* un = dynamic_cast<const UnaryExpr*>(&e)) {
            out_ += un->op == UnaryOp::Neg ? "-" : "!";
            printExpr(*un->operand, depth, /*parentPrec=*/100);
            return;
        }
        if (auto* sl = dynamic_cast<const StringLitExpr*>(&e)) {
            out_ += "\"" + escapeString(sl->value) + "\"";
            return;
        }
        if (auto* id = dynamic_cast<const IdentExpr*>(&e)) {
            out_ += id->name;
            return;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(&e)) {
            int myPrec = binOpPrec(bin->op);
            bool needParens = myPrec < parentPrec;
            if (needParens) out_ += "(";
            // Left child shares precedence (left-assoc); right child needs the
            // next level up so `a - (b - c)` keeps its parens.
            printExpr(*bin->lhs, depth, myPrec);
            out_ += std::string(" ") + binOpSpelling(bin->op) + " ";
            printExpr(*bin->rhs, depth, myPrec + 1);
            if (needParens) out_ += ")";
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(&e)) {
            out_ += call->callee + "(";
            for (std::size_t i = 0; i < call->args.size(); ++i) {
                if (i) out_ += ", ";
                printExpr(*call->args[i], depth, 0);
            }
            out_ += ")";
            return;
        }
        // Phase 17a: a call through a fn-value expression — `(callee)(args)`.
        // Parenthesize the callee so a field access / parenthesized expr keeps
        // the call grammar unambiguous when re-parsed.
        if (auto* cv = dynamic_cast<const CallValueExpr*>(&e)) {
            out_ += "(";
            printExpr(*cv->callee, depth, 0);
            out_ += ")(";
            for (std::size_t i = 0; i < cv->args.size(); ++i) {
                if (i) out_ += ", ";
                printExpr(*cv->args[i], depth, 0);
            }
            out_ += ")";
            return;
        }
        if (auto* mc = dynamic_cast<const MethodCallExpr*>(&e)) {
            printExpr(*mc->receiver, depth, /*parentPrec=*/100); // bind tight
            out_ += "." + mc->methodName + "(";
            for (std::size_t i = 0; i < mc->args.size(); ++i) {
                if (i) out_ += ", ";
                printExpr(*mc->args[i], depth, 0);
            }
            out_ += ")";
            return;
        }
        if (auto* sl = dynamic_cast<const StructLitExpr*>(&e)) {
            out_ += sl->structName;
            if (sl->fields.empty()) {
                out_ += " {}";
                return;
            }
            out_ += " { ";
            for (std::size_t i = 0; i < sl->fields.size(); ++i) {
                if (i) out_ += ", ";
                out_ += sl->fields[i].first + ": ";
                printExpr(*sl->fields[i].second, depth, 0);
            }
            out_ += " }";
            return;
        }
        if (auto* fe = dynamic_cast<const FieldExpr*>(&e)) {
            printExpr(*fe->object, depth, /*parentPrec=*/100);
            out_ += "." + fe->fieldName;
            return;
        }
        if (auto* ie = dynamic_cast<const IfExpr*>(&e)) {
            out_ += "if ";
            printExpr(*ie->cond, depth, 0);
            out_ += " ";
            printBlock(asBlock(ie->thenBranch), depth);
            if (ie->elseBranch) {
                out_ += " else ";
                // `else if` chains: an else branch that is itself an IfExpr is
                // printed inline (no extra braces) for the idiomatic ladder.
                if (dynamic_cast<const IfExpr*>(ie->elseBranch.get())) {
                    printExpr(*ie->elseBranch, depth, 0);
                } else {
                    printBlock(asBlock(ie->elseBranch), depth);
                }
            }
            return;
        }
        if (auto* be = dynamic_cast<const BlockExpr*>(&e)) {
            printBlock(be, depth);
            return;
        }
        if (auto* me = dynamic_cast<const MatchExpr*>(&e)) {
            out_ += "match ";
            printExpr(*me->scrutinee, depth, 0);
            if (me->arms.empty()) {
                out_ += " {}";
                return;
            }
            out_ += " {\n";
            for (const auto& arm : me->arms) {
                indent(depth + 1);
                printPattern(*arm.pattern);
                out_ += " => ";
                printExpr(*arm.body, depth + 1, 0);
                out_ += ",\n";
            }
            indent(depth);
            out_ += "}";
            return;
        }
        if (auto* we = dynamic_cast<const WhileExpr*>(&e)) {
            out_ += "while ";
            printExpr(*we->cond, depth, 0);
            out_ += " ";
            printBlock(asBlock(we->body), depth);
            return;
        }
        if (auto* le = dynamic_cast<const LoopExpr*>(&e)) {
            out_ += "loop ";
            printBlock(asBlock(le->body), depth);
            return;
        }
        if (auto* fe = dynamic_cast<const ForExpr*>(&e)) {
            out_ += "for ";
            printPattern(*fe->pattern);
            out_ += " in ";
            printExpr(*fe->iter, depth, 0);
            out_ += " ";
            printBlock(asBlock(fe->body), depth);
            return;
        }
        if (auto* re = dynamic_cast<const RangeExpr*>(&e)) {
            printExpr(*re->start, depth, 1);
            out_ += re->inclusive ? "..=" : "..";
            printExpr(*re->end, depth, 1);
            return;
        }
        if (auto* br = dynamic_cast<const BreakExpr*>(&e)) {
            out_ += "break";
            if (br->value) {
                out_ += " ";
                printExpr(*br->value, depth, 0);
            }
            return;
        }
        if (dynamic_cast<const ContinueExpr*>(&e)) {
            out_ += "continue";
            return;
        }
        if (auto* te = dynamic_cast<const TryExpr*>(&e)) {
            printExpr(*te->operand, depth, /*parentPrec=*/100);
            out_ += "?";
            return;
        }
        if (auto* ae = dynamic_cast<const AwaitExpr*>(&e)) {
            printExpr(*ae->operand, depth, /*parentPrec=*/100);
            out_ += ".await";
            return;
        }
        if (auto* re = dynamic_cast<const RefExpr*>(&e)) {
            out_ += re->isMut ? "&mut " : "&";
            printExpr(*re->operand, depth, /*parentPrec=*/100);
            return;
        }
        if (auto* se = dynamic_cast<const SliceExpr*>(&e)) {
            // `&v[a..b]` — the `&` and the `[a..b]` are a single node.
            out_ += "&";
            printExpr(*se->operand, depth, /*parentPrec=*/100);
            out_ += "[";
            printExpr(*se->start, depth, 1);
            out_ += "..";
            printExpr(*se->end, depth, 1);
            out_ += "]";
            return;
        }
        if (auto* bn = dynamic_cast<const BoxNewExpr*>(&e)) {
            out_ += "Box::new(";
            printExpr(*bn->value, depth, 0);
            out_ += ")";
            return;
        }
        if (auto* cl = dynamic_cast<const ClosureExpr*>(&e)) {
            if (cl->params.empty()) {
                out_ += "||";
            } else {
                out_ += "|";
                for (std::size_t i = 0; i < cl->params.size(); ++i) {
                    if (i) out_ += ", ";
                    out_ += cl->params[i].name;
                    if (cl->params[i].hasAnnotation) {
                        out_ += ": " + typeToString(cl->params[i].type);
                    }
                }
                out_ += "|";
            }
            out_ += " ";
            // A block body prints inline; a bare-expression body stays bare.
            if (auto* blk = dynamic_cast<const BlockExpr*>(cl->body.get())) {
                printBlock(blk, depth);
            } else {
                printExpr(*cl->body, depth, 0);
            }
            return;
        }
        // Phase 22: array literal `[a, b, c]`.
        if (auto* al = dynamic_cast<const ArrayLitExpr*>(&e)) {
            out_ += "[";
            for (std::size_t i = 0; i < al->elements.size(); ++i) {
                if (i) out_ += ", ";
                printExpr(*al->elements[i], depth, 0);
            }
            out_ += "]";
            return;
        }
        // Phase 22: array index `arr[i]` (postfix, binds tight).
        if (auto* ix = dynamic_cast<const IndexExpr*>(&e)) {
            printExpr(*ix->object, depth, /*parentPrec=*/100);
            out_ += "[";
            printExpr(*ix->index, depth, 0);
            out_ += "]";
            return;
        }
        // Phase 22: tuple literal `(a, b)`. A trailing comma on the 1-tuple is
        // unnecessary here (the parser only builds tuples of >= 2 elements).
        if (auto* tl = dynamic_cast<const TupleLitExpr*>(&e)) {
            out_ += "(";
            for (std::size_t i = 0; i < tl->elements.size(); ++i) {
                if (i) out_ += ", ";
                printExpr(*tl->elements[i], depth, 0);
            }
            out_ += ")";
            return;
        }
        // Phase 22: tuple field access `t.0` (postfix, binds tight).
        if (auto* tf = dynamic_cast<const TupleFieldExpr*>(&e)) {
            printExpr(*tf->object, depth, /*parentPrec=*/100);
            out_ += "." + std::to_string(tf->index);
            return;
        }
        out_ += "/* <unknown expr> */";
    }

    // Helper: every control-flow body is grammatically a BlockExpr, but the
    // AST stores it behind ExprPtr. Recover the BlockExpr; if it somehow is
    // not one (shouldn't happen for valid programs) fall back to a synthetic
    // single-tail block so printing never dereferences a bad cast.
    const BlockExpr* asBlock(const ExprPtr& e) {
        if (auto* b = dynamic_cast<const BlockExpr*>(e.get())) return b;
        return nullptr;
    }

    void printPattern(const Pattern& p) {
        if (auto* lit = dynamic_cast<const LitIntPat*>(&p)) {
            out_ += std::to_string(lit->value);
            return;
        }
        if (dynamic_cast<const WildPat*>(&p)) {
            out_ += "_";
            return;
        }
        if (auto* vp = dynamic_cast<const VarPat*>(&p)) {
            out_ += vp->name;
            return;
        }
        if (auto* cp = dynamic_cast<const CtorPat*>(&p)) {
            out_ += cp->ctorName;
            if (!cp->subpatterns.empty()) {
                out_ += "(";
                for (std::size_t i = 0; i < cp->subpatterns.size(); ++i) {
                    if (i) out_ += ", ";
                    printPattern(*cp->subpatterns[i]);
                }
                out_ += ")";
            }
            return;
        }
        if (auto* tp = dynamic_cast<const TuplePat*>(&p)) {
            out_ += "(";
            for (std::size_t i = 0; i < tp->elements.size(); ++i) {
                if (i) out_ += ", ";
                printPattern(*tp->elements[i]);
            }
            out_ += ")";
            return;
        }
        out_ += "/* <unknown pattern> */";
    }
};

} // namespace

std::string formatProgram(const ast::Program& program) {
    Printer p;
    return p.run(program);
}

} // namespace kardashev
