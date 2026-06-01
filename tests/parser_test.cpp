// Unit tests for kardashev::parse. Uses <cassert>; passing == exit 0.

#include "kardashev/ast.hpp"
#include "kardashev/parser.hpp"

#include <cassert>
#include <iostream>
#include <string>

using kardashev::parse;
using kardashev::ParseResult;
namespace ast = kardashev::ast;

namespace {

// Helper: parse `expr` wrapped in a one-fn body, return the body's tail.
const ast::Expr* tailExprOf(const ParseResult& r) {
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        }
        std::abort();
    }
    if (r.program.functions.size() != 1) {
        std::cerr << "expected 1 fn, got " << r.program.functions.size() << '\n';
        std::abort();
    }
    const ast::FnDecl& fn = r.program.functions[0];
    if (!fn.body) {
        std::cerr << "fn has no body\n";
        std::abort();
    }
    if (!fn.body->tail) {
        std::cerr << "block has no tail expr\n";
        std::abort();
    }
    return fn.body->tail.get();
}

ParseResult parseWrapped(const std::string& expr) {
    return parse("fn f() -> i64 { " + expr + " }");
}

void test_int_literal() {
    auto r = parseWrapped("42");
    const auto* lit = dynamic_cast<const ast::IntLitExpr*>(tailExprOf(r));
    assert(lit != nullptr);
    assert(lit->value == 42);
}

void test_identifier() {
    auto r = parseWrapped("x");
    const auto* id = dynamic_cast<const ast::IdentExpr*>(tailExprOf(r));
    assert(id != nullptr);
    assert(id->name == "x");
}

void test_binary_precedence() {
    // `1 + 2 * 3` should parse as Add(1, Mul(2, 3)).
    auto r = parseWrapped("1 + 2 * 3");
    const auto* root = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
    assert(root != nullptr);
    assert(root->op == ast::BinOp::Add);
    const auto* l = dynamic_cast<const ast::IntLitExpr*>(root->lhs.get());
    assert(l && l->value == 1);
    const auto* rhs = dynamic_cast<const ast::BinaryExpr*>(root->rhs.get());
    assert(rhs && rhs->op == ast::BinOp::Mul);
    const auto* rl = dynamic_cast<const ast::IntLitExpr*>(rhs->lhs.get());
    const auto* rr = dynamic_cast<const ast::IntLitExpr*>(rhs->rhs.get());
    assert(rl && rl->value == 2);
    assert(rr && rr->value == 3);
}

void test_logical_or_precedence() {
    // Phase 124: `||` binds looser than `&&`, so `a || b && c` is
    // `Or(a, And(b, c))` and `a && b || c` is `Or(And(a, b), c)`.
    {
        auto r = parseWrapped("a || b && c");
        const auto* root = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
        assert(root && root->op == ast::BinOp::Or);
        assert(dynamic_cast<const ast::IdentExpr*>(root->lhs.get()));
        const auto* rhs = dynamic_cast<const ast::BinaryExpr*>(root->rhs.get());
        assert(rhs && rhs->op == ast::BinOp::And);
    }
    {
        auto r = parseWrapped("a && b || c");
        const auto* root = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
        assert(root && root->op == ast::BinOp::Or);
        const auto* lhs = dynamic_cast<const ast::BinaryExpr*>(root->lhs.get());
        assert(lhs && lhs->op == ast::BinOp::And);
    }
    {
        // A leading `||` in PRIMARY position is still a zero-param closure,
        // not a malformed logical-or — disambiguation is positional.
        auto r = parseWrapped("|| 42");
        assert(dynamic_cast<const ast::ClosureExpr*>(tailExprOf(r)));
    }
}

void test_left_associativity() {
    // `1 - 2 - 3` should parse as Sub(Sub(1, 2), 3).
    auto r = parseWrapped("1 - 2 - 3");
    const auto* root = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
    assert(root && root->op == ast::BinOp::Sub);
    const auto* lhs = dynamic_cast<const ast::BinaryExpr*>(root->lhs.get());
    assert(lhs && lhs->op == ast::BinOp::Sub);
    const auto* rr = dynamic_cast<const ast::IntLitExpr*>(root->rhs.get());
    assert(rr && rr->value == 3);
}

void test_parenthesized() {
    auto r = parseWrapped("(1 + 2) * 3");
    const auto* root = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
    assert(root && root->op == ast::BinOp::Mul);
    const auto* lhs = dynamic_cast<const ast::BinaryExpr*>(root->lhs.get());
    assert(lhs && lhs->op == ast::BinOp::Add);
}

void test_function_call() {
    auto r = parseWrapped("f(1, x, g(2))");
    const auto* call = dynamic_cast<const ast::CallExpr*>(tailExprOf(r));
    assert(call != nullptr);
    assert(call->callee == "f");
    assert(call->args.size() == 3);
    assert(dynamic_cast<ast::IntLitExpr*>(call->args[0].get()));
    assert(dynamic_cast<ast::IdentExpr*>(call->args[1].get()));
    const auto* inner = dynamic_cast<ast::CallExpr*>(call->args[2].get());
    assert(inner && inner->callee == "g");
}

void test_call_no_args() {
    auto r = parseWrapped("f()");
    const auto* call = dynamic_cast<const ast::CallExpr*>(tailExprOf(r));
    assert(call && call->callee == "f");
    assert(call->args.empty());
}

void test_if_expression() {
    auto r = parseWrapped("if x < 2 { x } else { x + 1 }");
    const auto* ie = dynamic_cast<const ast::IfExpr*>(tailExprOf(r));
    assert(ie != nullptr);
    const auto* cond = dynamic_cast<const ast::BinaryExpr*>(ie->cond.get());
    assert(cond && cond->op == ast::BinOp::Lt);
    const auto* thenB = dynamic_cast<const ast::BlockExpr*>(ie->thenBranch.get());
    const auto* elseB = dynamic_cast<const ast::BlockExpr*>(ie->elseBranch.get());
    assert(thenB && thenB->tail);
    assert(elseB && elseB->tail);
    const auto* thenTail = dynamic_cast<const ast::IdentExpr*>(thenB->tail.get());
    assert(thenTail && thenTail->name == "x");
}

void test_let_stmt() {
    auto r = parse("fn f() -> i64 { let x = 5; x }");
    assert(r.ok());
    const auto& body = *r.program.functions[0].body;
    assert(body.stmts.size() == 1);
    const auto* let = dynamic_cast<const ast::LetStmt*>(body.stmts[0].get());
    assert(let != nullptr);
    assert(let->name == "x");
    const auto* val = dynamic_cast<const ast::IntLitExpr*>(let->value.get());
    assert(val && val->value == 5);
    const auto* tail = dynamic_cast<const ast::IdentExpr*>(body.tail.get());
    assert(tail && tail->name == "x");
}

void test_return_stmt() {
    auto r = parse("fn f() -> i64 { return 42; }");
    assert(r.ok());
    const auto& body = *r.program.functions[0].body;
    assert(body.stmts.size() == 1);
    assert(body.tail == nullptr);
    const auto* ret = dynamic_cast<const ast::ReturnStmt*>(body.stmts[0].get());
    assert(ret != nullptr);
    const auto* val = dynamic_cast<const ast::IntLitExpr*>(ret->value.get());
    assert(val && val->value == 42);
}

void test_fn_decl() {
    auto r = parse("fn add(a: i64, b: i64) -> i64 { a + b }");
    assert(r.ok());
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "add");
    assert(fn.params.size() == 2);
    assert(fn.params[0].name == "a" && fn.params[0].type.name == "i64");
    assert(fn.params[1].name == "b" && fn.params[1].type.name == "i64");
    assert(fn.returnType.name == "i64");
    assert(fn.body && fn.body->tail);
    const auto* bin = dynamic_cast<const ast::BinaryExpr*>(fn.body->tail.get());
    assert(bin && bin->op == ast::BinOp::Add);
}

void test_fib_full() {
    // The Phase 1 MVP target — this exact program must parse cleanly.
    auto r = parse(
        "fn fib(n: i64) -> i64 {\n"
        "    if n < 2 { n } else { fib(n-1) + fib(n-2) }\n"
        "}");
    if (!r.ok()) {
        std::cerr << "fib parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fib = r.program.functions[0];
    assert(fib.name == "fib");
    assert(fib.params.size() == 1 && fib.params[0].name == "n");
    assert(fib.returnType.name == "i64");
    // Body's tail is an `if`.
    const auto* ie =
        dynamic_cast<const ast::IfExpr*>(fib.body->tail.get());
    assert(ie != nullptr);
    // Condition: n < 2
    const auto* cond = dynamic_cast<const ast::BinaryExpr*>(ie->cond.get());
    assert(cond && cond->op == ast::BinOp::Lt);
    // else-branch tail: fib(n-1) + fib(n-2)
    const auto* elseB =
        dynamic_cast<const ast::BlockExpr*>(ie->elseBranch.get());
    assert(elseB && elseB->tail);
    const auto* sum =
        dynamic_cast<const ast::BinaryExpr*>(elseB->tail.get());
    assert(sum && sum->op == ast::BinOp::Add);
    const auto* lcall = dynamic_cast<const ast::CallExpr*>(sum->lhs.get());
    const auto* rcall = dynamic_cast<const ast::CallExpr*>(sum->rhs.get());
    assert(lcall && lcall->callee == "fib" && lcall->args.size() == 1);
    assert(rcall && rcall->callee == "fib" && rcall->args.size() == 1);
}

void test_parse_error_missing_arrow() {
    // Missing `->`.
    auto r = parse("fn f() i64 { 1 }");
    assert(!r.ok());
    assert(!r.errors.empty());
}

void test_multiple_fns() {
    auto r = parse("fn a() -> i64 { 1 } fn b() -> i64 { 2 }");
    assert(r.ok());
    assert(r.program.functions.size() == 2);
    assert(r.program.functions[0].name == "a");
    assert(r.program.functions[1].name == "b");
}

void test_struct_decl_empty() {
    auto r = parse("struct Empty {}");
    assert(r.ok());
    assert(r.program.structs.size() == 1);
    assert(r.program.functions.empty());
    const auto& s = r.program.structs[0];
    assert(s.name == "Empty");
    assert(s.fields.empty());
}

void test_struct_decl_one_field() {
    auto r = parse("struct Point { x: i64 }");
    assert(r.ok());
    assert(r.program.structs.size() == 1);
    const auto& s = r.program.structs[0];
    assert(s.name == "Point");
    assert(s.fields.size() == 1);
    assert(s.fields[0].name == "x");
    assert(s.fields[0].type.name == "i64");
}

void test_struct_decl_trailing_comma() {
    auto r = parse("struct Vec3 { x: i64, y: i64, z: i64, }");
    assert(r.ok());
    assert(r.program.structs.size() == 1);
    const auto& s = r.program.structs[0];
    assert(s.name == "Vec3");
    assert(s.fields.size() == 3);
    assert(s.fields[0].name == "x");
    assert(s.fields[1].name == "y");
    assert(s.fields[2].name == "z");
}

void test_struct_literal() {
    auto r = parseWrapped("Point { x: 1, y: 2 }");
    const auto* lit = dynamic_cast<const ast::StructLitExpr*>(tailExprOf(r));
    assert(lit != nullptr);
    assert(lit->structName == "Point");
    assert(lit->fields.size() == 2);
    assert(lit->fields[0].first == "x");
    const auto* v0 = dynamic_cast<const ast::IntLitExpr*>(lit->fields[0].second.get());
    assert(v0 && v0->value == 1);
    assert(lit->fields[1].first == "y");
    const auto* v1 = dynamic_cast<const ast::IntLitExpr*>(lit->fields[1].second.get());
    assert(v1 && v1->value == 2);
}

void test_struct_literal_empty() {
    auto r = parseWrapped("Foo {}");
    const auto* lit = dynamic_cast<const ast::StructLitExpr*>(tailExprOf(r));
    assert(lit != nullptr);
    assert(lit->structName == "Foo");
    assert(lit->fields.empty());
}

void test_field_access() {
    auto r = parseWrapped("p.x");
    const auto* fe = dynamic_cast<const ast::FieldExpr*>(tailExprOf(r));
    assert(fe != nullptr);
    assert(fe->fieldName == "x");
    const auto* obj = dynamic_cast<const ast::IdentExpr*>(fe->object.get());
    assert(obj && obj->name == "p");
}

void test_chained_field_access() {
    auto r = parseWrapped("a.b.c");
    const auto* outer = dynamic_cast<const ast::FieldExpr*>(tailExprOf(r));
    assert(outer != nullptr);
    assert(outer->fieldName == "c");
    const auto* inner = dynamic_cast<const ast::FieldExpr*>(outer->object.get());
    assert(inner != nullptr);
    assert(inner->fieldName == "b");
    const auto* base = dynamic_cast<const ast::IdentExpr*>(inner->object.get());
    assert(base && base->name == "a");
}

void test_field_access_precedence() {
    // `a.x + b.y` parses as Binary(Add, FieldExpr(a,x), FieldExpr(b,y)).
    auto r = parseWrapped("a.x + b.y");
    const auto* bin = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
    assert(bin != nullptr);
    assert(bin->op == ast::BinOp::Add);
    const auto* lhs = dynamic_cast<const ast::FieldExpr*>(bin->lhs.get());
    assert(lhs != nullptr);
    assert(lhs->fieldName == "x");
    const auto* lobj = dynamic_cast<const ast::IdentExpr*>(lhs->object.get());
    assert(lobj && lobj->name == "a");
    const auto* rhs = dynamic_cast<const ast::FieldExpr*>(bin->rhs.get());
    assert(rhs != nullptr);
    assert(rhs->fieldName == "y");
    const auto* robj = dynamic_cast<const ast::IdentExpr*>(rhs->object.get());
    assert(robj && robj->name == "b");
}

void test_if_cond_no_struct_lit() {
    // `if cond { 1 } else { 2 }` — the cond must be an IdentExpr, not a
    // struct literal swallowing the then-block braces. Exercises
    // restrictStructLit.
    auto r = parse("fn f() -> i64 { if cond { 1 } else { 2 } }");
    assert(r.ok());
    const auto& body = *r.program.functions[0].body;
    const auto* ie = dynamic_cast<const ast::IfExpr*>(body.tail.get());
    assert(ie != nullptr);
    const auto* cond = dynamic_cast<const ast::IdentExpr*>(ie->cond.get());
    assert(cond != nullptr);
    assert(cond->name == "cond");
    const auto* thenB = dynamic_cast<const ast::BlockExpr*>(ie->thenBranch.get());
    assert(thenB && thenB->tail);
    const auto* thenLit = dynamic_cast<const ast::IntLitExpr*>(thenB->tail.get());
    assert(thenLit && thenLit->value == 1);
    const auto* elseB = dynamic_cast<const ast::BlockExpr*>(ie->elseBranch.get());
    assert(elseB && elseB->tail);
    const auto* elseLit = dynamic_cast<const ast::IntLitExpr*>(elseB->tail.get());
    assert(elseLit && elseLit->value == 2);
}

void test_mixed_program_struct_then_fn() {
    auto r = parse("struct Point { x: i64, y: i64 } fn f() -> i64 { 1 }");
    assert(r.ok());
    assert(r.program.structs.size() == 1);
    assert(r.program.functions.size() == 1);
    assert(r.program.structs[0].name == "Point");
    assert(r.program.functions[0].name == "f");
}

void test_mixed_program_fn_then_struct() {
    auto r = parse("fn f() -> i64 { 1 } struct Point { x: i64, y: i64 }");
    assert(r.ok());
    assert(r.program.structs.size() == 1);
    assert(r.program.functions.size() == 1);
    assert(r.program.structs[0].name == "Point");
    assert(r.program.functions[0].name == "f");
}

void test_enum_decl_empty() {
    auto r = parse("enum Empty {}");
    assert(r.ok());
    assert(r.program.enums.size() == 1);
    assert(r.program.functions.empty());
    assert(r.program.structs.empty());
    const auto& e = r.program.enums[0];
    assert(e.name == "Empty");
    assert(e.variants.empty());
}

void test_enum_decl_single_unit_variant() {
    auto r = parse("enum E { A }");
    assert(r.ok());
    assert(r.program.enums.size() == 1);
    const auto& e = r.program.enums[0];
    assert(e.name == "E");
    assert(e.variants.size() == 1);
    assert(e.variants[0].name == "A");
    assert(e.variants[0].payloadTypes.empty());
}

void test_enum_decl_multi_unit_trailing_comma() {
    auto r = parse("enum E { A, B, C, }");
    assert(r.ok());
    assert(r.program.enums.size() == 1);
    const auto& e = r.program.enums[0];
    assert(e.variants.size() == 3);
    assert(e.variants[0].name == "A");
    assert(e.variants[1].name == "B");
    assert(e.variants[2].name == "C");
    for (const auto& v : e.variants) assert(v.payloadTypes.empty());
}

void test_enum_decl_tuple_payload() {
    auto r = parse("enum Maybe { Some(i64), None }");
    assert(r.ok());
    assert(r.program.enums.size() == 1);
    const auto& e = r.program.enums[0];
    assert(e.name == "Maybe");
    assert(e.variants.size() == 2);
    assert(e.variants[0].name == "Some");
    assert(e.variants[0].payloadTypes.size() == 1);
    assert(e.variants[0].payloadTypes[0].name == "i64");
    assert(e.variants[1].name == "None");
    assert(e.variants[1].payloadTypes.empty());
}

void test_enum_decl_multi_arg_tuple_variant() {
    auto r = parse("enum Pair { P(i64, bool) }");
    assert(r.ok());
    const auto& e = r.program.enums[0];
    assert(e.variants.size() == 1);
    assert(e.variants[0].name == "P");
    assert(e.variants[0].payloadTypes.size() == 2);
    assert(e.variants[0].payloadTypes[0].name == "i64");
    assert(e.variants[0].payloadTypes[1].name == "bool");
}

void test_match_literal_pats() {
    auto r = parseWrapped("match x { 0 => 1, 1 => 2, _ => 3 }");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(tailExprOf(r));
    assert(me != nullptr);
    const auto* sc = dynamic_cast<const ast::IdentExpr*>(me->scrutinee.get());
    assert(sc && sc->name == "x");
    assert(me->arms.size() == 3);
    const auto* p0 = dynamic_cast<const ast::LitIntPat*>(me->arms[0].pattern.get());
    assert(p0 && p0->value == 0);
    const auto* b0 = dynamic_cast<const ast::IntLitExpr*>(me->arms[0].body.get());
    assert(b0 && b0->value == 1);
    const auto* p1 = dynamic_cast<const ast::LitIntPat*>(me->arms[1].pattern.get());
    assert(p1 && p1->value == 1);
    const auto* p2 = dynamic_cast<const ast::WildPat*>(me->arms[2].pattern.get());
    assert(p2 != nullptr);
    const auto* b2 = dynamic_cast<const ast::IntLitExpr*>(me->arms[2].body.get());
    assert(b2 && b2->value == 3);
}

void test_match_wildcard_only() {
    auto r = parseWrapped("match x { _ => 0 }");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(tailExprOf(r));
    assert(me != nullptr);
    assert(me->arms.size() == 1);
    assert(dynamic_cast<const ast::WildPat*>(me->arms[0].pattern.get()) != nullptr);
    const auto* b = dynamic_cast<const ast::IntLitExpr*>(me->arms[0].body.get());
    assert(b && b->value == 0);
}

void test_match_var_binding() {
    auto r = parseWrapped("match x { y => y + 1 }");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(tailExprOf(r));
    assert(me != nullptr);
    assert(me->arms.size() == 1);
    const auto* vp = dynamic_cast<const ast::VarPat*>(me->arms[0].pattern.get());
    assert(vp && vp->name == "y");
    const auto* bin = dynamic_cast<const ast::BinaryExpr*>(me->arms[0].body.get());
    assert(bin && bin->op == ast::BinOp::Add);
}

void test_match_ctor_pat_unit() {
    auto r = parseWrapped("match m { None => 0, Some(v) => v }");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(tailExprOf(r));
    assert(me != nullptr);
    assert(me->arms.size() == 2);
    // Bare Ident in pattern position is always VarPat at parse time.
    const auto* p0 = dynamic_cast<const ast::VarPat*>(me->arms[0].pattern.get());
    assert(p0 && p0->name == "None");
    const auto* p1 = dynamic_cast<const ast::CtorPat*>(me->arms[1].pattern.get());
    assert(p1 && p1->ctorName == "Some");
    assert(p1->subpatterns.size() == 1);
    const auto* inner = dynamic_cast<const ast::VarPat*>(p1->subpatterns[0].get());
    assert(inner && inner->name == "v");
    const auto* b1 = dynamic_cast<const ast::IdentExpr*>(me->arms[1].body.get());
    assert(b1 && b1->name == "v");
}

void test_match_ctor_pat_nested() {
    auto r = parseWrapped("match m { Some(Some(x)) => x, _ => 0 }");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(tailExprOf(r));
    assert(me != nullptr);
    assert(me->arms.size() == 2);
    const auto* outer = dynamic_cast<const ast::CtorPat*>(me->arms[0].pattern.get());
    assert(outer && outer->ctorName == "Some");
    assert(outer->subpatterns.size() == 1);
    const auto* inner = dynamic_cast<const ast::CtorPat*>(outer->subpatterns[0].get());
    assert(inner && inner->ctorName == "Some");
    assert(inner->subpatterns.size() == 1);
    const auto* innerVar = dynamic_cast<const ast::VarPat*>(inner->subpatterns[0].get());
    assert(innerVar && innerVar->name == "x");
    assert(dynamic_cast<const ast::WildPat*>(me->arms[1].pattern.get()) != nullptr);
}

void test_match_scrutinee_no_struct_lit_without_parens() {
    // Following Rust: an unparenthesized struct literal in match scrutinee
    // would be ambiguous with the match arm braces, so it's disallowed.
    auto r = parse("fn f() -> i64 { match Point { x: 1, y: 2 } { _ => 0 } }");
    assert(!r.ok());
    assert(!r.errors.empty());
}

void test_match_scrutinee_struct_lit_parenthesized() {
    // Parenthesizing makes the struct literal unambiguous.
    auto r = parse("fn f() -> i64 { match (Point { x: 1, y: 2 }) { _ => 0 } }");
    assert(r.ok());
    const auto& body = *r.program.functions[0].body;
    const auto* me = dynamic_cast<const ast::MatchExpr*>(body.tail.get());
    assert(me != nullptr);
    const auto* sc = dynamic_cast<const ast::StructLitExpr*>(me->scrutinee.get());
    assert(sc && sc->structName == "Point");
    assert(sc->fields.size() == 2);
    assert(me->arms.size() == 1);
    assert(dynamic_cast<const ast::WildPat*>(me->arms[0].pattern.get()) != nullptr);
}

void test_match_trailing_comma() {
    auto r = parseWrapped("match x { 0 => 1, _ => 2, }");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(tailExprOf(r));
    assert(me != nullptr);
    assert(me->arms.size() == 2);
}

void test_mixed_program_enum_and_fn() {
    auto r = parse(
        "enum Maybe { Some(i64), None } "
        "struct S { x: i64 } "
        "fn f(m: Maybe) -> i64 { match m { Some(v) => v, None => 0 } }");
    assert(r.ok());
    assert(r.program.enums.size() == 1);
    assert(r.program.structs.size() == 1);
    assert(r.program.functions.size() == 1);
    assert(r.program.enums[0].name == "Maybe");
    assert(r.program.enums[0].variants.size() == 2);
    assert(r.program.structs[0].name == "S");
    const auto& fn = r.program.functions[0];
    assert(fn.name == "f");
    assert(fn.params.size() == 1 && fn.params[0].type.name == "Maybe");
    const auto* me = dynamic_cast<const ast::MatchExpr*>(fn.body->tail.get());
    assert(me != nullptr);
    assert(me->arms.size() == 2);
}

void test_fn_decl_single_generic() {
    auto r = parse("fn id<T>(x: T) -> T { x }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "id");
    assert(fn.genericParams.size() == 1);
    assert(fn.genericParams[0].name == "T");
    assert(fn.params.size() == 1);
    assert(fn.params[0].name == "x");
    assert(fn.params[0].type.name == "T");
    assert(fn.returnType.name == "T");
}

void test_fn_decl_multi_generic() {
    auto r = parse("fn pair<A, B>(a: A, b: B) -> A { a }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "pair");
    assert(fn.genericParams.size() == 2);
    assert(fn.genericParams[0].name == "A");
    assert(fn.genericParams[1].name == "B");
    assert(fn.params.size() == 2);
    assert(fn.params[0].name == "a" && fn.params[0].type.name == "A");
    assert(fn.params[1].name == "b" && fn.params[1].type.name == "B");
    assert(fn.returnType.name == "A");
}

void test_fn_decl_no_generic_still_works() {
    auto r = parse("fn add(a: i64, b: i64) -> i64 { a + b }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "add");
    assert(fn.genericParams.empty());
    assert(fn.params.size() == 2);
    assert(fn.returnType.name == "i64");
}

void test_fn_decl_generic_trailing_comma() {
    auto r = parse("fn id<T,>(x: T) -> T { x }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "id");
    assert(fn.genericParams.size() == 1);
    assert(fn.genericParams[0].name == "T");
}

void test_struct_decl_with_generic_param() {
    auto r = parse("struct Box<T> { v: T }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.structs.size() == 1);
    const auto& s = r.program.structs[0];
    assert(s.name == "Box");
    assert(s.genericParams.size() == 1);
    assert(s.genericParams[0].name == "T");
    assert(s.fields.size() == 1);
    assert(s.fields[0].name == "v");
    assert(s.fields[0].type.name == "T");
    assert(s.fields[0].type.typeArgs.empty());
}

void test_enum_decl_with_generic_params() {
    auto r = parse("enum Result<T, E> { Ok(T), Err(E) }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.enums.size() == 1);
    const auto& e = r.program.enums[0];
    assert(e.name == "Result");
    assert(e.genericParams.size() == 2);
    assert(e.genericParams[0].name == "T");
    assert(e.genericParams[1].name == "E");
    assert(e.variants.size() == 2);
    assert(e.variants[0].name == "Ok");
    assert(e.variants[0].payloadTypes.size() == 1);
    assert(e.variants[0].payloadTypes[0].name == "T");
    assert(e.variants[1].name == "Err");
    assert(e.variants[1].payloadTypes.size() == 1);
    assert(e.variants[1].payloadTypes[0].name == "E");
}

void test_typeref_with_typeargs() {
    auto r = parse("fn f(x: Box<i64>) -> Box<i64> { x }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.params.size() == 1);
    const auto& pt = fn.params[0].type;
    assert(pt.name == "Box");
    assert(pt.typeArgs.size() == 1);
    assert(pt.typeArgs[0].name == "i64");
    assert(pt.typeArgs[0].typeArgs.empty());
    const auto& rt = fn.returnType;
    assert(rt.name == "Box");
    assert(rt.typeArgs.size() == 1);
    assert(rt.typeArgs[0].name == "i64");
}

void test_nested_typeref_typeargs() {
    auto r = parse("fn f(x: Pair<Box<i64>, bool>) -> i64 { 0 }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.params.size() == 1);
    const auto& pt = fn.params[0].type;
    assert(pt.name == "Pair");
    assert(pt.typeArgs.size() == 2);
    // typeArgs[0] is Box<i64>
    const auto& a0 = pt.typeArgs[0];
    assert(a0.name == "Box");
    assert(a0.typeArgs.size() == 1);
    assert(a0.typeArgs[0].name == "i64");
    assert(a0.typeArgs[0].typeArgs.empty());
    // typeArgs[1] is bool
    const auto& a1 = pt.typeArgs[1];
    assert(a1.name == "bool");
    assert(a1.typeArgs.empty());
}

// ---- Phase 3.3: traits, impl blocks, method calls, bounded generics ----

void test_trait_decl_one_method() {
    auto r = parse("trait Show { fn show(self) -> i64; }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.traits.size() == 1);
    const auto& tr = r.program.traits[0];
    assert(tr.name == "Show");
    assert(tr.methods.size() == 1);
    const auto& m = tr.methods[0];
    assert(m.name == "show");
    assert(m.params.size() == 1);
    assert(m.params[0].name == "self");
    assert(m.params[0].type.name == "Self");
    assert(m.returnType.name == "i64");
}

void test_impl_decl_basic() {
    auto r = parse(
        "struct P{x:i64} trait Show{fn show(self)->i64;} "
        "impl Show for P{fn show(self)->i64{self.x}}");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.impls.size() == 1);
    const auto& im = r.program.impls[0];
    assert(im.traitName == "Show");
    assert(im.forType.name == "P");
    assert(im.methods.size() == 1);
    assert(im.methods[0].name == "show");
}

void test_typeparam_with_bound() {
    auto r = parse(
        "trait T{fn x(self)->i64;} "
        "fn use_t<T: T>(t: T) -> i64 { 0 }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "use_t");
    assert(fn.genericParams.size() == 1);
    assert(fn.genericParams[0].name == "T");
    assert(fn.genericParams[0].bound == "T");
}

void test_method_call_distinguishes_field() {
    // `p.x.foo() + p.y` — assert the tail expression's structure exactly:
    //   BinaryExpr(Add,
    //              MethodCallExpr(FieldExpr(p,"x"), "foo"),
    //              FieldExpr(p, "y"))
    auto r = parse("fn f(p: P) -> i64 { p.x.foo() + p.y }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.body && fn.body->tail);
    const auto* bin = dynamic_cast<const ast::BinaryExpr*>(fn.body->tail.get());
    assert(bin != nullptr);
    assert(bin->op == ast::BinOp::Add);
    // LHS: p.x.foo()  -> MethodCallExpr(FieldExpr(p,"x"), "foo")
    const auto* mcall = dynamic_cast<const ast::MethodCallExpr*>(bin->lhs.get());
    assert(mcall != nullptr);
    assert(mcall->methodName == "foo");
    assert(mcall->args.empty());
    const auto* recv = dynamic_cast<const ast::FieldExpr*>(mcall->receiver.get());
    assert(recv != nullptr);
    assert(recv->fieldName == "x");
    const auto* recvObj = dynamic_cast<const ast::IdentExpr*>(recv->object.get());
    assert(recvObj && recvObj->name == "p");
    // RHS: p.y  -> FieldExpr(p, "y") (NOT a MethodCallExpr)
    const auto* rfe = dynamic_cast<const ast::FieldExpr*>(bin->rhs.get());
    assert(rfe != nullptr);
    assert(rfe->fieldName == "y");
    const auto* rfeObj = dynamic_cast<const ast::IdentExpr*>(rfe->object.get());
    assert(rfeObj && rfeObj->name == "p");
    // Confirm it isn't accidentally a MethodCallExpr.
    assert(dynamic_cast<const ast::MethodCallExpr*>(bin->rhs.get()) == nullptr);
}

// ---- Phase 3.4: postfix `?` (try) operator ----

void test_try_postfix() {
    // `foo()?` parses as TryExpr(CallExpr(foo)).
    auto r = parseWrapped("foo()?");
    const auto* tryE = dynamic_cast<const ast::TryExpr*>(tailExprOf(r));
    assert(tryE != nullptr);
    const auto* call = dynamic_cast<const ast::CallExpr*>(tryE->operand.get());
    assert(call != nullptr);
    assert(call->callee == "foo");
    assert(call->args.empty());
}

void test_try_chained_after_dot() {
    // `foo().bar?` parses as TryExpr(FieldExpr(CallExpr(foo), "bar")).
    auto r = parseWrapped("foo().bar?");
    const auto* tryE = dynamic_cast<const ast::TryExpr*>(tailExprOf(r));
    assert(tryE != nullptr);
    const auto* field = dynamic_cast<const ast::FieldExpr*>(tryE->operand.get());
    assert(field != nullptr);
    assert(field->fieldName == "bar");
    const auto* call = dynamic_cast<const ast::CallExpr*>(field->object.get());
    assert(call != nullptr);
    assert(call->callee == "foo");
    assert(call->args.empty());
}

void test_effect_row_on_fn_decl() {
    auto r = parse("fn f() -> i64 ! { io, alloc } { 0 }");
    if (!r.ok()) {
        std::cerr << "parse failed\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": "
                      << e.message << '\n';
        }
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.effects.labels.size() == 2);
    assert(fn.effects.labels[0] == "io");
    assert(fn.effects.labels[1] == "alloc");
}

void test_effect_row_empty_braces() {
    // `! { }` is allowed and means an empty effect row (same as omitted).
    auto r = parse("fn f() -> i64 ! { } { 0 }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.effects.labels.empty());
}

void test_no_effect_row_defaults_pure() {
    auto r = parse("fn f() -> i64 { 0 }");
    assert(r.ok());
    assert(r.program.functions[0].effects.labels.empty());
}

void test_fn_type_param_with_effect_row() {
    // Phase 10a: a function type in parameter position carries an effect
    // row. `fn(i64) -> i64 ! {io}` parses into TypeRef.isFn with fnParams,
    // fnRet, and fnEffects populated.
    auto r = parse("fn takes(f: fn(i64) -> i64 ! {io}) -> i64 { f(1) }");
    if (!r.ok()) {
        std::cerr << "parse failed\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": "
                      << e.message << '\n';
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.params.size() == 1);
    const auto& pty = fn.params[0].type;
    assert(pty.isFn);
    assert(pty.fnParams.size() == 1);
    assert(pty.fnParams[0].name == "i64");
    assert(pty.fnRet && pty.fnRet->name == "i64");
    assert(pty.fnEffects.size() == 1);
    assert(pty.fnEffects[0] == "io");
}

void test_fn_type_param_row_var_no_effects() {
    // `fn(T) -> U ! {e}` with a row var, plus a fn type with no effect row.
    auto r = parse(
        "fn map(f: fn(i64) -> i64 ! {e}, g: fn(i64) -> i64) -> i64 { 0 }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.params.size() == 2);
    assert(fn.params[0].type.isFn);
    assert(fn.params[0].type.fnEffects.size() == 1);
    assert(fn.params[0].type.fnEffects[0] == "e");
    assert(fn.params[1].type.isFn);
    assert(fn.params[1].type.fnEffects.empty()); // pure fn type
}

// --- Phase 10b: closure parsing ---

void test_closure_single_param() {
    // `|x| x + n` parses to a ClosureExpr with one (unannotated) param and a
    // BinaryExpr body.
    auto r = parseWrapped("|x| x + 1");
    auto* cl = dynamic_cast<const ast::ClosureExpr*>(tailExprOf(r));
    assert(cl);
    assert(cl->params.size() == 1);
    assert(cl->params[0].name == "x");
    assert(!cl->params[0].hasAnnotation);
    assert(dynamic_cast<const ast::BinaryExpr*>(cl->body.get()));
}

void test_closure_multi_param_with_annotation() {
    // `|x: i64, y| ...` — first param annotated, second inferred.
    auto r = parseWrapped("|x: i64, y| x + y");
    auto* cl = dynamic_cast<const ast::ClosureExpr*>(tailExprOf(r));
    assert(cl);
    assert(cl->params.size() == 2);
    assert(cl->params[0].name == "x");
    assert(cl->params[0].hasAnnotation);
    assert(cl->params[0].type.name == "i64");
    assert(cl->params[1].name == "y");
    assert(!cl->params[1].hasAnnotation);
}

void test_closure_zero_param() {
    // `|| 7` (the `||` token) is a zero-param closure.
    auto r = parseWrapped("|| 7");
    auto* cl = dynamic_cast<const ast::ClosureExpr*>(tailExprOf(r));
    assert(cl);
    assert(cl->params.empty());
    assert(dynamic_cast<const ast::IntLitExpr*>(cl->body.get()));
}

void test_closure_block_body() {
    // `|x| { ... }` keeps a BlockExpr body.
    auto r = parseWrapped("|x| { let y = x; y }");
    auto* cl = dynamic_cast<const ast::ClosureExpr*>(tailExprOf(r));
    assert(cl);
    assert(cl->params.size() == 1);
    assert(dynamic_cast<const ast::BlockExpr*>(cl->body.get()));
}

void test_closure_as_call_argument() {
    // A closure passed directly as a call argument parses (no parens needed).
    auto r = parse("fn apply(f: fn(i64) -> i64) -> i64 { f(1) }\n"
                   "fn main() -> i64 { apply(|x| x + 2) }");
    assert(r.ok());
    const auto& mainFn = r.program.functions.back();
    auto* call = dynamic_cast<const ast::CallExpr*>(mainFn.body->tail.get());
    assert(call && call->callee == "apply");
    assert(call->args.size() == 1);
    assert(dynamic_cast<const ast::ClosureExpr*>(call->args[0].get()));
}

// --- Phase 17a: call a fn VALUE produced by an expression ---

void test_call_value_field() {
    // `(s.f)(x)` parses to a CallValueExpr whose callee is a FieldExpr.
    auto r = parseWrapped("(s.f)(10)");
    auto* cv = dynamic_cast<const ast::CallValueExpr*>(tailExprOf(r));
    assert(cv);
    assert(cv->args.size() == 1);
    auto* fe = dynamic_cast<const ast::FieldExpr*>(cv->callee.get());
    assert(fe && fe->fieldName == "f");
    auto* obj = dynamic_cast<const ast::IdentExpr*>(fe->object.get());
    assert(obj && obj->name == "s");
}

void test_call_value_field_through_self() {
    // `(self.f)(self.base)` — callee is a field of `self`, arg is a field too.
    auto r = parseWrapped("(self.f)(self.base)");
    auto* cv = dynamic_cast<const ast::CallValueExpr*>(tailExprOf(r));
    assert(cv);
    assert(cv->args.size() == 1);
    assert(dynamic_cast<const ast::FieldExpr*>(cv->callee.get()));
    assert(dynamic_cast<const ast::FieldExpr*>(cv->args[0].get()));
}

void test_call_value_of_call_result() {
    // `(getCb())(41)` — callee is itself a CallExpr; outer is a CallValueExpr.
    auto r = parseWrapped("(getCb())(41)");
    auto* cv = dynamic_cast<const ast::CallValueExpr*>(tailExprOf(r));
    assert(cv);
    assert(cv->args.size() == 1);
    auto* inner = dynamic_cast<const ast::CallExpr*>(cv->callee.get());
    assert(inner && inner->callee == "getCb");
}

void test_bare_call_still_callexpr() {
    // Regression: a plain `foo(1)` must stay a CallExpr, NOT a CallValueExpr.
    auto r = parseWrapped("foo(1)");
    assert(dynamic_cast<const ast::CallExpr*>(tailExprOf(r)));
    assert(!dynamic_cast<const ast::CallValueExpr*>(tailExprOf(r)));
}

void test_mod_decl_basic() {
    auto r = parse("mod util;\nfn main() -> i64 { 0 }");
    if (!r.ok()) {
        std::cerr << "parse failed\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": "
                      << e.message << '\n';
        }
        std::abort();
    }
    assert(r.program.mods.size() == 1);
    assert(r.program.mods[0].name == "util");
    assert(r.program.functions.size() == 1);
}

void test_mod_multiple() {
    auto r = parse("mod a;\nmod b;\nmod c;\nfn main() -> i64 { 0 }");
    assert(r.ok());
    assert(r.program.mods.size() == 3);
    assert(r.program.mods[0].name == "a");
    assert(r.program.mods[1].name == "b");
    assert(r.program.mods[2].name == "c");
}

// --- Phase 9: loops, ranges, assignment ---

void test_while_expr() {
    auto r = parseWrapped("while x < 3 { 0 }");
    auto* we = dynamic_cast<const ast::WhileExpr*>(tailExprOf(r));
    assert(we != nullptr);
    auto* cond = dynamic_cast<const ast::BinaryExpr*>(we->cond.get());
    assert(cond && cond->op == ast::BinOp::Lt);
    assert(dynamic_cast<const ast::BlockExpr*>(we->body.get()));
}

void test_loop_expr() {
    auto r = parseWrapped("loop { 0 }");
    auto* le = dynamic_cast<const ast::LoopExpr*>(tailExprOf(r));
    assert(le != nullptr);
    assert(dynamic_cast<const ast::BlockExpr*>(le->body.get()));
}

void test_break_with_value() {
    auto r = parseWrapped("loop { break 42 }");
    auto* le = dynamic_cast<const ast::LoopExpr*>(tailExprOf(r));
    assert(le != nullptr);
    auto* body = dynamic_cast<const ast::BlockExpr*>(le->body.get());
    assert(body && body->tail);
    auto* be = dynamic_cast<const ast::BreakExpr*>(body->tail.get());
    assert(be != nullptr && be->value != nullptr);
    auto* lit = dynamic_cast<const ast::IntLitExpr*>(be->value.get());
    assert(lit && lit->value == 42);
}

void test_break_bare_and_continue() {
    auto r = parseWrapped("loop { continue }");
    auto* le = dynamic_cast<const ast::LoopExpr*>(tailExprOf(r));
    assert(le != nullptr);
    auto* body = dynamic_cast<const ast::BlockExpr*>(le->body.get());
    assert(body && dynamic_cast<const ast::ContinueExpr*>(body->tail.get()));

    auto r2 = parseWrapped("loop { break }");
    auto* le2 = dynamic_cast<const ast::LoopExpr*>(tailExprOf(r2));
    auto* body2 = dynamic_cast<const ast::BlockExpr*>(le2->body.get());
    auto* be = dynamic_cast<const ast::BreakExpr*>(body2->tail.get());
    assert(be != nullptr && be->value == nullptr);
}

void test_for_exclusive_range() {
    auto r = parseWrapped("for x in 0..10 { 0 }");
    auto* fe = dynamic_cast<const ast::ForExpr*>(tailExprOf(r));
    assert(fe != nullptr);
    auto* vp = dynamic_cast<const ast::VarPat*>(fe->pattern.get());
    assert(vp && vp->name == "x");
    auto* re = dynamic_cast<const ast::RangeExpr*>(fe->iter.get());
    assert(re != nullptr && re->inclusive == false);
    auto* lo = dynamic_cast<const ast::IntLitExpr*>(re->start.get());
    auto* hi = dynamic_cast<const ast::IntLitExpr*>(re->end.get());
    assert(lo && lo->value == 0 && hi && hi->value == 10);
}

void test_for_inclusive_range() {
    auto r = parseWrapped("for x in 1..=10 { 0 }");
    auto* fe = dynamic_cast<const ast::ForExpr*>(tailExprOf(r));
    assert(fe != nullptr);
    auto* re = dynamic_cast<const ast::RangeExpr*>(fe->iter.get());
    assert(re != nullptr && re->inclusive == true);
}

void test_range_binds_looser_than_arith() {
    // `1 + 1 .. 2 * 5` => RangeExpr(start = 1+1, end = 2*5).
    auto r = parseWrapped("1 + 1 .. 2 * 5");
    auto* re = dynamic_cast<const ast::RangeExpr*>(tailExprOf(r));
    assert(re != nullptr);
    assert(dynamic_cast<const ast::BinaryExpr*>(re->start.get()));
    assert(dynamic_cast<const ast::BinaryExpr*>(re->end.get()));
}

void test_let_mut_flag() {
    auto r = parseWrapped("let mut x = 0; x");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.body->stmts.size() == 1);
    auto* let = dynamic_cast<const ast::LetStmt*>(fn.body->stmts[0].get());
    assert(let != nullptr && let->isMut == true && let->name == "x");
}

void test_assign_stmt() {
    auto r = parseWrapped("let mut x = 0; x = 5; x");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.body->stmts.size() == 2);
    auto* as = dynamic_cast<const ast::AssignStmt*>(fn.body->stmts[1].get());
    assert(as != nullptr);
    assert(dynamic_cast<const ast::IdentExpr*>(as->target.get()));
    auto* rhs = dynamic_cast<const ast::IntLitExpr*>(as->value.get());
    assert(rhs && rhs->value == 5);
}

void test_field_assign_stmt() {
    auto r = parseWrapped("p.x = 5; 0");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    auto* as = dynamic_cast<const ast::AssignStmt*>(fn.body->stmts[0].get());
    assert(as != nullptr);
    auto* fe = dynamic_cast<const ast::FieldExpr*>(as->target.get());
    assert(fe != nullptr && fe->fieldName == "x");
}

void test_while_as_statement_then_tail() {
    // A while in statement position (no `;`) followed by a tail value.
    auto r = parseWrapped("while x < 3 { 0 } 7");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.body->stmts.size() == 1);
    assert(dynamic_cast<const ast::ExprStmt*>(fn.body->stmts[0].get()));
    auto* tail = dynamic_cast<const ast::IntLitExpr*>(fn.body->tail.get());
    assert(tail && tail->value == 7);
}

// --- Phase 11: dyn Trait, &self, Box ---

void test_impl_ref_self_param() {
    // `&self` parses into a `self` Param whose type is a reference to `Self`.
    auto r = parse(
        "trait Shape { fn area(&self) -> i64; }\n"
        "struct Sq { side: i64 }\n"
        "impl Shape for Sq { fn area(&self) -> i64 { self.side } }");
    assert(r.ok());
    assert(r.program.traits.size() == 1);
    const auto& sig = r.program.traits[0].methods[0];
    assert(sig.params.size() == 1);
    assert(sig.params[0].name == "self");
    assert(sig.params[0].type.name == "Self");
    assert(sig.params[0].type.isRef);
    const auto& m = r.program.impls[0].methods[0];
    assert(m.params[0].name == "self");
    assert(m.params[0].type.isRef);
}

void test_dyn_ref_param_type() {
    // A `&dyn Trait` parameter type: isRef + isDyn, trait name in `name`.
    auto r = parse("fn f(s: &dyn Shape) -> i64 { 0 }");
    assert(r.ok());
    const auto& p = r.program.functions[0].params[0];
    assert(p.type.isRef);
    assert(p.type.isDyn);
    assert(p.type.name == "Shape");
}

void test_box_dyn_let_annotation() {
    // `let b: Box<dyn Shape> = ...;` — the annotation is a Box with one
    // dyn-Trait type arg.
    auto r = parse(
        "fn f() -> i64 { let b: Box<dyn Shape> = make(); 0 }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    const auto* let =
        dynamic_cast<const ast::LetStmt*>(fn.body->stmts[0].get());
    assert(let != nullptr);
    assert(let->annotation != nullptr);
    assert(let->annotation->name == "Box");
    assert(let->annotation->typeArgs.size() == 1);
    assert(let->annotation->typeArgs[0].isDyn);
    assert(let->annotation->typeArgs[0].name == "Shape");
}

void test_box_new_expr() {
    // `Box::new(v)` parses to a BoxNewExpr (not a CallExpr).
    auto r = parseWrapped("Box::new(7)");
    const auto* bn = dynamic_cast<const ast::BoxNewExpr*>(tailExprOf(r));
    assert(bn != nullptr);
    const auto* v = dynamic_cast<const ast::IntLitExpr*>(bn->value.get());
    assert(v && v->value == 7);
}

// --- Phase 15: bool literals, unary ops, else-if, inherent impls, pub ---

void test_bool_literal_true() {
    auto r = parseWrapped("true");
    const auto* bl = dynamic_cast<const ast::BoolLitExpr*>(tailExprOf(r));
    assert(bl != nullptr);
    assert(bl->value == true);
}

void test_bool_literal_false() {
    auto r = parseWrapped("false");
    const auto* bl = dynamic_cast<const ast::BoolLitExpr*>(tailExprOf(r));
    assert(bl != nullptr);
    assert(bl->value == false);
}

void test_unary_neg_literal() {
    auto r = parseWrapped("-5");
    const auto* un = dynamic_cast<const ast::UnaryExpr*>(tailExprOf(r));
    assert(un != nullptr);
    assert(un->op == ast::UnaryOp::Neg);
    const auto* v = dynamic_cast<const ast::IntLitExpr*>(un->operand.get());
    assert(v && v->value == 5);
}

void test_unary_not() {
    auto r = parseWrapped("!true");
    const auto* un = dynamic_cast<const ast::UnaryExpr*>(tailExprOf(r));
    assert(un != nullptr);
    assert(un->op == ast::UnaryOp::Not);
    const auto* v = dynamic_cast<const ast::BoolLitExpr*>(un->operand.get());
    assert(v && v->value == true);
}

void test_unary_double_neg() {
    // `-(-3)` => Neg(Neg(3)) (the inner parens nest but collapse to the
    // operand expression).
    auto r = parseWrapped("-(-3)");
    const auto* outer = dynamic_cast<const ast::UnaryExpr*>(tailExprOf(r));
    assert(outer != nullptr && outer->op == ast::UnaryOp::Neg);
    const auto* inner =
        dynamic_cast<const ast::UnaryExpr*>(outer->operand.get());
    assert(inner != nullptr && inner->op == ast::UnaryOp::Neg);
    const auto* v = dynamic_cast<const ast::IntLitExpr*>(inner->operand.get());
    assert(v && v->value == 3);
}

void test_unary_binds_tighter_than_mul() {
    // `-a * b` parses as `(-a) * b`: a Mul whose lhs is a UnaryExpr.
    auto r = parseWrapped("-a * b");
    const auto* bin = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
    assert(bin != nullptr && bin->op == ast::BinOp::Mul);
    const auto* lhs = dynamic_cast<const ast::UnaryExpr*>(bin->lhs.get());
    assert(lhs != nullptr && lhs->op == ast::UnaryOp::Neg);
    const auto* rhs = dynamic_cast<const ast::IdentExpr*>(bin->rhs.get());
    assert(rhs != nullptr && rhs->name == "b");
}

void test_unary_not_binds_tighter_than_eq() {
    // `!a == b` parses as `(!a) == b`: an Eq whose lhs is a UnaryExpr.
    auto r = parseWrapped("!a == b");
    const auto* bin = dynamic_cast<const ast::BinaryExpr*>(tailExprOf(r));
    assert(bin != nullptr && bin->op == ast::BinOp::Eq);
    const auto* lhs = dynamic_cast<const ast::UnaryExpr*>(bin->lhs.get());
    assert(lhs != nullptr && lhs->op == ast::UnaryOp::Not);
}

void test_unary_neg_binds_looser_than_field() {
    // `-a.b` parses as `-(a.b)`: a UnaryExpr whose operand is a FieldExpr.
    auto r = parseWrapped("-a.b");
    const auto* un = dynamic_cast<const ast::UnaryExpr*>(tailExprOf(r));
    assert(un != nullptr && un->op == ast::UnaryOp::Neg);
    const auto* fe = dynamic_cast<const ast::FieldExpr*>(un->operand.get());
    assert(fe != nullptr && fe->fieldName == "b");
}

void test_else_if_chain() {
    // `if a { 1 } else if b { 2 } else { 3 }` desugars to a nested IfExpr in
    // the else branch.
    auto r = parseWrapped("if a < 0 { 1 } else if a < 5 { 2 } else { 3 }");
    const auto* ie = dynamic_cast<const ast::IfExpr*>(tailExprOf(r));
    assert(ie != nullptr);
    // The else branch is itself an IfExpr (not a BlockExpr).
    const auto* elseIf = dynamic_cast<const ast::IfExpr*>(ie->elseBranch.get());
    assert(elseIf != nullptr);
    // Its else branch is the final bare block.
    const auto* finalBlock =
        dynamic_cast<const ast::BlockExpr*>(elseIf->elseBranch.get());
    assert(finalBlock != nullptr && finalBlock->tail);
}

void test_inherent_impl_no_trait() {
    // `impl Type { ... }` parses with an empty traitName (inherent impl).
    auto r = parse(
        "struct P { x: i64 }\n"
        "impl P { fn get(&self) -> i64 { self.x } }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.impls.size() == 1);
    const auto& im = r.program.impls[0];
    assert(im.traitName.empty());
    assert(im.isInherent());
    assert(im.forType.name == "P");
    assert(im.methods.size() == 1 && im.methods[0].name == "get");
}

void test_trait_impl_still_has_trait_name() {
    // Regression: `impl Trait for Type` keeps its trait name (not inherent).
    auto r = parse(
        "struct P { x: i64 } trait Show { fn show(&self) -> i64; }\n"
        "impl Show for P { fn show(&self) -> i64 { self.x } }");
    assert(r.ok());
    assert(r.program.impls.size() == 1);
    assert(!r.program.impls[0].isInherent());
    assert(r.program.impls[0].traitName == "Show");
}

void test_pub_struct_enum_trait() {
    auto r = parse(
        "pub struct S { v: i64 }\n"
        "pub enum E { A, B(i64) }\n"
        "pub trait T { fn m(&self) -> i64; }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors) {
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message
                      << '\n';
        }
        std::abort();
    }
    assert(r.program.structs.size() == 1 && r.program.structs[0].isPub);
    assert(r.program.enums.size() == 1 && r.program.enums[0].isPub);
    assert(r.program.traits.size() == 1 && r.program.traits[0].isPub);
}

void test_pub_inherent_impl() {
    auto r = parse(
        "struct P { x: i64 }\n"
        "pub impl P { fn get(&self) -> i64 { self.x } }");
    assert(r.ok());
    assert(r.program.impls.size() == 1);
    assert(r.program.impls[0].isPub);
    assert(r.program.impls[0].isInherent());
}

// Phase 16: the return type is optional — omitting `-> T` means `unit`. This
// is the surface the `Drop` trait method uses (`fn drop(&mut self);`).
void test_optional_return_type_fn() {
    auto r = parse("fn noop() { let x = 1; }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    assert(r.program.functions[0].returnType.name == "unit");
}

void test_optional_return_type_drop_trait() {
    // The exact spec spelling: `trait Drop { fn drop(&mut self); }` plus an
    // `impl Drop for T` whose method also omits the return type.
    auto r = parse(
        "trait Drop { fn drop(&mut self); }\n"
        "struct N { id: i64 }\n"
        "impl Drop for N { fn drop(&mut self) { let x = self.id; } }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.traits.size() == 1);
    assert(r.program.traits[0].name == "Drop");
    assert(r.program.traits[0].methods.size() == 1);
    assert(r.program.traits[0].methods[0].name == "drop");
    assert(r.program.traits[0].methods[0].returnType.name == "unit");
    assert(r.program.impls.size() == 1);
    assert(r.program.impls[0].traitName == "Drop");
    assert(r.program.impls[0].methods[0].returnType.name == "unit");
}

// --- Phase 21a: generic trait parameters ---

void test_generic_trait_decl() {
    // `trait Iterator<T> { fn next(&mut self) -> Option<T>; }` parses with a
    // single generic param; the method's return type names that param.
    auto r = parse("trait Iterator<T> { fn next(&mut self) -> Option<T>; }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.traits.size() == 1);
    const auto& t = r.program.traits[0];
    assert(t.name == "Iterator");
    assert(t.genericParams.size() == 1);
    assert(t.genericParams[0].name == "T");
    assert(t.methods.size() == 1 && t.methods[0].name == "next");
    // return type `Option<T>` mentions the trait param by name.
    assert(t.methods[0].returnType.name == "Option");
    assert(t.methods[0].returnType.typeArgs.size() == 1);
    assert(t.methods[0].returnType.typeArgs[0].name == "T");
}

void test_generic_trait_multi_params() {
    // Two trait params + trailing comma.
    auto r = parse("trait Map<K, V,> { fn get(&self, k: K) -> V; }");
    assert(r.ok());
    assert(r.program.traits.size() == 1);
    const auto& t = r.program.traits[0];
    assert(t.genericParams.size() == 2);
    assert(t.genericParams[0].name == "K");
    assert(t.genericParams[1].name == "V");
}

void test_generic_trait_impl_with_args() {
    // `impl Iterator<i64> for Range { ... }` keeps the trait name AND records
    // the supplied trait type-args; it is NOT an inherent impl.
    auto r = parse(
        "struct Range { start: i64, end: i64, inclusive: i64 }\n"
        "impl Iterator<i64> for Range {\n"
        "    fn next(&mut self) -> Option<i64> { None }\n"
        "}");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.impls.size() == 1);
    const auto& im = r.program.impls[0];
    assert(!im.isInherent());
    assert(im.traitName == "Iterator");
    assert(im.traitTypeArgs.size() == 1);
    assert(im.traitTypeArgs[0].name == "i64");
    assert(im.forType.name == "Range");
}

void test_generic_inherent_impl_unaffected() {
    // Regression: `impl Pair<T> { ... }` (a generic-type inherent impl) must
    // still parse as inherent with the type-args on forType, NOT as a trait
    // impl. The disambiguation (no `for`) keeps the leading `<...>` on the
    // implementing type.
    auto r = parse("impl Pair<T> { fn fst(&self) -> i64 { 0 } }");
    assert(r.ok());
    assert(r.program.impls.size() == 1);
    const auto& im = r.program.impls[0];
    assert(im.isInherent());
    assert(im.traitName.empty());
    assert(im.traitTypeArgs.empty());
    assert(im.forType.name == "Pair");
    assert(im.forType.typeArgs.size() == 1 && im.forType.typeArgs[0].name == "T");
}

void test_parameterized_trait_bound() {
    // `fn head<T, C: Container<T>>(c: C) -> T` — the bound carries the trait's
    // type args, which name the fn's own generic param.
    auto r = parse("fn head<T, C: Container<T>>(c: C) -> T { c.first() }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.genericParams.size() == 2);
    assert(fn.genericParams[0].name == "T" && fn.genericParams[0].bound.empty());
    assert(fn.genericParams[1].name == "C");
    assert(fn.genericParams[1].bound == "Container");
    assert(fn.genericParams[1].boundTypeArgs.size() == 1);
    assert(fn.genericParams[1].boundTypeArgs[0].name == "T");
}

void test_unparameterized_bound_has_no_args() {
    // Regression: a plain `<I: Iterator>` bound has an empty boundTypeArgs.
    auto r = parse("fn f<I: Iterator>(it: I) -> i64 { 0 }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.genericParams.size() == 1);
    assert(fn.genericParams[0].bound == "Iterator");
    assert(fn.genericParams[0].boundTypeArgs.empty());
}

// --- Phase 21b: where clauses ---

void test_where_clause_desugars_onto_param() {
    // A `where C: Container<T>` clause must land on the matching generic param's
    // bound / boundTypeArgs, identically to the inline `<C: Container<T>>` form.
    auto r = parse("fn head<T, C>(c: C) -> T where C: Container<T> { c.first() }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.genericParams.size() == 2);
    assert(fn.genericParams[0].name == "T" && fn.genericParams[0].bound.empty());
    assert(fn.genericParams[1].name == "C");
    assert(fn.genericParams[1].bound == "Container");
    assert(fn.genericParams[1].boundTypeArgs.size() == 1);
    assert(fn.genericParams[1].boundTypeArgs[0].name == "T");
}

void test_where_clause_multi_constraint() {
    // Two comma-separated constraints, each onto its own param.
    auto r = parse("fn f<G, S>(g: G, s: S) -> i64 where G: Getter, S: Show { 0 }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.genericParams.size() == 2);
    assert(fn.genericParams[0].bound == "Getter");
    assert(fn.genericParams[1].bound == "Show");
}

void test_where_clause_equals_inline() {
    // The where form and the inline form must produce byte-identical generic
    // params (the desugar is exact).
    auto inl = parse("fn f<T, C: Container<T>>(c: C) -> T { c.first() }");
    auto whr = parse("fn f<T, C>(c: C) -> T where C: Container<T> { c.first() }");
    assert(inl.ok() && whr.ok());
    const auto& a = inl.program.functions[0].genericParams;
    const auto& b = whr.program.functions[0].genericParams;
    assert(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        assert(a[i].name == b[i].name);
        assert(a[i].bound == b[i].bound);
        assert(a[i].boundTypeArgs.size() == b[i].boundTypeArgs.size());
    }
}

void test_where_clause_unknown_param_errors() {
    // A `where` constraint naming an unknown generic param is a parse error.
    auto r = parse("fn f<T>(x: T) -> i64 where U: Show { 0 }");
    assert(!r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.message.find("unknown generic parameter") != std::string::npos)
            found = true;
    assert(found);
}

void test_where_clause_on_impl_method() {
    // `where` works on impl methods too.
    auto r = parse(
        "impl Foo for Bar { fn m<T, C>(&self, c: C) -> T where C: Container<T> "
        "{ c.first() } }");
    assert(r.ok());
    assert(r.program.impls.size() == 1);
    const auto& m = r.program.impls[0].methods[0];
    assert(m.genericParams.size() == 2);
    assert(m.genericParams[1].bound == "Container");
    assert(m.genericParams[1].boundTypeArgs.size() == 1);
}

// --- Phase 21b: associated types ---

void test_trait_assoc_type_decl() {
    // `trait Container { type Item; fn get(&self) -> Self::Item; }`.
    auto r = parse(
        "trait Container { type Item; fn get(&self) -> Self::Item; }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.traits.size() == 1);
    const auto& t = r.program.traits[0];
    assert(t.assocTypes.size() == 1);
    assert(t.assocTypes[0].name == "Item");
    assert(t.methods.size() == 1 && t.methods[0].name == "get");
    // The method return type is the projection `Self::Item`.
    assert(t.methods[0].returnType.name == "Self");
    assert(t.methods[0].returnType.assocName == "Item");
}

void test_gat_decl_and_projection() {
    // v28 Phase 155: a trait GAT `type Out<T>;`, an impl binding `type Out<T> =
    // Pair<T, T>;`, and a projection `Self::Out<i64>` parse with their params.
    auto r = parse("trait Wrap { type Out<T>; fn make(&self) -> Self::Out<i64>; }");
    assert(r.ok());
    const auto& t = r.program.traits[0];
    assert(t.assocTypes.size() == 1);
    assert(t.assocTypes[0].name == "Out");
    assert(t.assocTypes[0].typeParams.size() == 1);
    assert(t.assocTypes[0].typeParams[0].name == "T");
    // the method return type is the projection Self::Out<i64>
    const auto& rt = t.methods[0].returnType;
    assert(rt.name == "Self" && rt.assocName == "Out");
    assert(rt.assocTypeArgs.size() == 1 && rt.assocTypeArgs[0].name == "i64");
    auto ri = parse("struct P<A,B>{a:A,b:B} impl Wrap for G { type Out<T> = P<T,T>; }");
    assert(ri.ok());
    const auto& im = ri.program.impls[0];
    assert(im.assocTypes.size() == 1 && im.assocTypes[0].name == "Out");
    assert(im.assocTypes[0].typeParams.size() == 1);
}

void test_impl_assoc_type_def() {
    // `impl Container for IntBox { type Item = i64; fn get(...) ... }`.
    auto r = parse(
        "impl Container for IntBox { type Item = i64; fn get(&self) -> "
        "Self::Item { self.v } }");
    if (!r.ok()) {
        std::cerr << "parse failed:\n";
        for (const auto& e : r.errors)
            std::cerr << "  " << e.line << ":" << e.column << ": " << e.message << '\n';
        std::abort();
    }
    assert(r.program.impls.size() == 1);
    const auto& im = r.program.impls[0];
    assert(im.assocTypes.size() == 1);
    assert(im.assocTypes[0].name == "Item");
    assert(im.assocTypes[0].type.name == "i64");
    assert(im.methods.size() == 1);
}

void test_assoc_projection_on_generic_param() {
    // `fn first<C: Container>(c: C) -> C::Item { c.get() }` — the return type is
    // the projection `C::Item`.
    auto r = parse("fn first<C: Container>(c: C) -> C::Item { c.get() }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    assert(fn.returnType.name == "C");
    assert(fn.returnType.assocName == "Item");
}

// --- Phase 24: extern "C" FFI declarations ---

void test_extern_c_per_decl() {
    auto r = parse("extern \"C\" fn abs(x: i64) -> i64;");
    assert(r.ok());
    assert(r.program.externFns.size() == 1);
    const auto& ef = r.program.externFns[0];
    assert(ef.name == "abs");
    assert(ef.abi == "C");
    assert(ef.params.size() == 1);
    assert(ef.params[0].name == "x");
    assert(ef.params[0].type.name == "i64");
    assert(ef.returnType.name == "i64");
    assert(!ef.hasExplicitEffects); // no `! { ... }` row
}

void test_extern_c_no_args_no_ret() {
    auto r = parse("extern \"C\" fn getpid() -> i64;");
    assert(r.ok());
    assert(r.program.externFns.size() == 1);
    assert(r.program.externFns[0].params.empty());
}

void test_extern_c_i32_and_ref_param() {
    auto r = parse("extern \"C\" fn strlen(s: &String) -> i64;\n"
                   "extern \"C\" fn putchar(c: i32) -> i32;");
    assert(r.ok());
    assert(r.program.externFns.size() == 2);
    // strlen: &String param
    const auto& sl = r.program.externFns[0];
    assert(sl.name == "strlen");
    assert(sl.params[0].type.isRef);
    assert(sl.params[0].type.name == "String");
    // putchar: the FFI-only i32 spelling survives to the AST verbatim.
    const auto& pc = r.program.externFns[1];
    assert(pc.name == "putchar");
    assert(pc.params[0].type.name == "i32");
    assert(pc.returnType.name == "i32");
}

void test_extern_c_block_form() {
    auto r = parse("extern \"C\" {\n"
                   "  fn abs(x: i32) -> i32;\n"
                   "  fn getpid() -> i32;\n"
                   "}");
    assert(r.ok());
    assert(r.program.externFns.size() == 2);
    assert(r.program.externFns[0].name == "abs");
    assert(r.program.externFns[1].name == "getpid");
    assert(r.program.externFns[0].abi == "C");
    assert(r.program.externFns[1].abi == "C");
}

void test_extern_c_explicit_effect_row() {
    // An explicit `! { }` is recorded so the typechecker can treat the extern
    // as pure (vs. the default `io`).
    auto r = parse("extern \"C\" fn abs(x: i32) -> i32 ! { };");
    assert(r.ok());
    assert(r.program.externFns.size() == 1);
    assert(r.program.externFns[0].hasExplicitEffects);
    assert(r.program.externFns[0].effects.labels.empty());
}

void test_extern_c_preserves_abi_string() {
    // The parser records WHATEVER ABI string was written (the typechecker, not
    // the parser, rejects a non-"C" ABI) so the error carries the offender.
    auto r = parse("extern \"Rust\" fn abs(x: i64) -> i64;");
    assert(r.ok());
    assert(r.program.externFns.size() == 1);
    assert(r.program.externFns[0].abi == "Rust");
}

void test_extern_c_body_is_error() {
    // An extern decl is bodyless; a `{ body }` is a parse error.
    auto r = parse("extern \"C\" fn abs(x: i64) -> i64 { x }");
    assert(!r.ok());
}

void test_extern_and_fn_coexist() {
    // An extern decl alongside a normal fn: both land in the program.
    auto r = parse("extern \"C\" fn abs(x: i64) -> i64;\n"
                   "fn main() -> i64 ! { io } { abs(0 - 7) }");
    assert(r.ok());
    assert(r.program.externFns.size() == 1);
    assert(r.program.functions.size() == 1);
    assert(r.program.functions[0].name == "main");
}

// --- Phase 25: const items + const fn + const-expr array lengths -----------

void test_const_item() {
    auto r = parse("const SIZE: i64 = 3 + 2;");
    assert(r.ok());
    assert(r.program.consts.size() == 1);
    const auto& cd = r.program.consts[0];
    assert(cd.name == "SIZE");
    assert(cd.type.name == "i64");
    assert(!cd.isPub);
    assert(dynamic_cast<const ast::BinaryExpr*>(cd.value.get()) != nullptr);
}

void test_pub_const_item() {
    auto r = parse("pub const A: i64 = 10;");
    assert(r.ok());
    assert(r.program.consts.size() == 1);
    assert(r.program.consts[0].isPub);
    assert(r.program.consts[0].name == "A");
}

void test_const_bool_item() {
    auto r = parse("const FLAG: bool = true;");
    assert(r.ok());
    assert(r.program.consts.size() == 1);
    assert(r.program.consts[0].type.name == "bool");
    assert(dynamic_cast<const ast::BoolLitExpr*>(
               r.program.consts[0].value.get()) != nullptr);
}

void test_const_fn_decl() {
    // `const fn` lands in functions with isConst set; otherwise a normal fn.
    auto r = parse("const fn sq(x: i64) -> i64 { x * x }");
    assert(r.ok());
    assert(r.program.consts.empty());
    assert(r.program.functions.size() == 1);
    const auto& fn = r.program.functions[0];
    assert(fn.name == "sq");
    assert(fn.isConst);
    assert(fn.params.size() == 1);
}

void test_pub_const_fn_decl() {
    auto r = parse("pub const fn sq(x: i64) -> i64 { x * x }");
    assert(r.ok());
    assert(r.program.functions.size() == 1);
    assert(r.program.functions[0].isConst);
    assert(r.program.functions[0].isPub);
}

void test_plain_fn_is_not_const() {
    auto r = parse("fn f(x: i64) -> i64 { x }");
    assert(r.ok());
    assert(r.program.functions.size() == 1);
    assert(!r.program.functions[0].isConst);
}

void test_array_len_literal_still_literal() {
    // Phase 22 path unchanged: a bare integer literal length goes straight to
    // `arrayLen` with no arrayLenExpr.
    auto r = parse("fn f() -> i64 { let a: [i64; 3] = [1, 2, 3]; a[0] }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    const auto* let =
        dynamic_cast<const ast::LetStmt*>(fn.body->stmts[0].get());
    assert(let != nullptr && let->annotation);
    assert(let->annotation->isArray);
    assert(let->annotation->arrayLen == 3);
    assert(let->annotation->arrayLenExpr == nullptr);
}

void test_array_len_const_expr() {
    // Phase 25: a non-literal length is stashed in arrayLenExpr for the
    // typechecker to evaluate (arrayLen stays 0 at parse time).
    auto r = parse("const N: i64 = 3;\n"
                   "fn f() -> i64 { let a: [i64; N] = [1, 2, 3]; a[0] }");
    assert(r.ok());
    const ast::FnDecl* f = nullptr;
    for (const auto& fn : r.program.functions)
        if (fn.name == "f") f = &fn;
    assert(f != nullptr);
    const auto* let =
        dynamic_cast<const ast::LetStmt*>(f->body->stmts[0].get());
    assert(let != nullptr && let->annotation);
    assert(let->annotation->isArray);
    assert(let->annotation->arrayLenExpr != nullptr);
    assert(dynamic_cast<const ast::IdentExpr*>(
               let->annotation->arrayLenExpr.get()) != nullptr);
}

void test_array_len_const_fn_call() {
    auto r = parse("fn f() -> i64 { let a: [i64; sq(2)] = [1,2,3,4]; a[0] }");
    assert(r.ok());
    const auto& fn = r.program.functions[0];
    const auto* let =
        dynamic_cast<const ast::LetStmt*>(fn.body->stmts[0].get());
    assert(let != nullptr && let->annotation && let->annotation->isArray);
    assert(let->annotation->arrayLenExpr != nullptr);
    assert(dynamic_cast<const ast::CallExpr*>(
               let->annotation->arrayLenExpr.get()) != nullptr);
}

// Phase 57 (v10): const-generic params + symbolic array length + tuple-let
// annotation.
void test_const_generic_param() {
    auto r = parse("struct Mat<const N: i64> { data: [i64; N] }");
    assert(r.ok());
    const auto& s = r.program.structs[0];
    assert(s.genericParams.size() == 1);
    assert(s.genericParams[0].isConst);
    assert(s.genericParams[0].name == "N");
    // mixed type + const params: `<T, const N: i64>`
    auto r2 = parse("struct Buf<T, const CAP: i64> { x: T }");
    assert(r2.ok());
    const auto& gp = r2.program.structs[0].genericParams;
    assert(gp.size() == 2 && !gp[0].isConst && gp[1].isConst &&
           gp[1].name == "CAP");
    // `[i64; N]` keeps N as the length expr (an identifier).
    assert(s.fields[0].type.isArray);
    assert(dynamic_cast<const ast::IdentExpr*>(
               s.fields[0].type.arrayLenExpr.get()) != nullptr);
}

void test_const_generic_param_non_i64_rejected() {
    // v28 Phase 153: i64/bool/char const params are now ACCEPTED; only an
    // unsupported type (e.g. f64) is rejected at the declaration.
    assert(parse("struct Mat<const N: bool> { x: i64 }").ok());
    assert(parse("struct Mat<const N: char> { x: i64 }").ok());
    auto r = parse("struct Mat<const N: f64> { x: i64 }");
    assert(!r.ok()); // must be i64 / bool / char
}

// Phase 58 (v10): a const-generic VALUE argument `Mat<3>` parses as a const-arg
// TypeRef (not a type) and round-trips through ast_print.
void test_const_generic_arg() {
    auto r = parse("fn f(m: Mat<3>) -> i64 { 0 }");
    assert(r.ok());
    const auto& pty = r.program.functions[0].params[0].type;
    assert(pty.name == "Mat");
    assert(pty.typeArgs.size() == 1);
    assert(pty.typeArgs[0].isConstArg);
    assert(pty.typeArgs[0].constArgValue == 3);
    // mixed: a type then a const value, `Matrix<i64, 4>`.
    auto r2 = parse("fn g(m: Matrix<i64, 4>) -> i64 { 0 }");
    assert(r2.ok());
    const auto& g = r2.program.functions[0].params[0].type;
    assert(g.typeArgs.size() == 2);
    assert(!g.typeArgs[0].isConstArg && g.typeArgs[0].name == "i64");
    assert(g.typeArgs[1].isConstArg && g.typeArgs[1].constArgValue == 4);
}

void test_tuple_let_annotation() {
    auto r = parse("fn f() -> i64 { let (a, b): (i64, i64) = (3, 4); a }");
    assert(r.ok());
    const auto* let = dynamic_cast<const ast::LetStmt*>(
        r.program.functions[0].body->stmts[0].get());
    assert(let && let->tupleNames.size() == 2);
    assert(let->tupleNames[0] == "a" && let->tupleNames[1] == "b");
    assert(let->annotation && let->annotation->isTuple);
    assert(let->annotation->tupleElems.size() == 2);
    // the no-annotation tuple-let still parses (regression).
    auto r2 = parse("fn f() -> i64 { let (a, b) = (3, 4); a }");
    assert(r2.ok());
    const auto* let2 = dynamic_cast<const ast::LetStmt*>(
        r2.program.functions[0].body->stmts[0].get());
    assert(let2 && let2->tupleNames.size() == 2 && !let2->annotation);
}

// Regression (v10): a nested tuple field access `n.0.0` must NOT lex `0.0` as
// a float (which swallowed the second `.0`). `1.0` elsewhere stays a float.
void test_nested_tuple_field_access() {
    auto r = parse("fn f() -> i64 { let n = ((1, 2), 3); n.0.0 + n.0.1 + n.1 }");
    assert(r.ok());
    // a float literal in value position is unaffected.
    auto r2 = parse("fn f() -> f64 { let x = 3.14; x }");
    assert(r2.ok());
    const auto* let = dynamic_cast<const ast::LetStmt*>(
        r2.program.functions[0].body->stmts[0].get());
    assert(let != nullptr);
    const auto* lit = dynamic_cast<const ast::FloatLitExpr*>(let->value.get());
    assert(lit != nullptr); // 3.14 is still a float
}

// Phase 62 (v10): array-repeat `[value; N]` parses (repeatCount set), distinct
// from an element list `[a, b]`.
void test_array_repeat() {
    auto r = parse("fn f() -> i64 { let a = [7; 3]; a[0] }");
    assert(r.ok());
    const auto* let = dynamic_cast<const ast::LetStmt*>(
        r.program.functions[0].body->stmts[0].get());
    assert(let != nullptr);
    const auto* arr =
        dynamic_cast<const ast::ArrayLitExpr*>(let->value.get());
    assert(arr != nullptr);
    assert(arr->elements.size() == 1);
    assert(arr->repeatCount != nullptr); // it's a repeat, not a list
    // an ordinary element list has no repeatCount.
    auto r2 = parse("fn f() -> i64 { let a = [1, 2, 3]; a[0] }");
    assert(r2.ok());
    const auto* let2 = dynamic_cast<const ast::LetStmt*>(
        r2.program.functions[0].body->stmts[0].get());
    const auto* arr2 =
        dynamic_cast<const ast::ArrayLitExpr*>(let2->value.get());
    assert(arr2 && arr2->elements.size() == 3 && !arr2->repeatCount);
}

// v31 Phase 167: negative marker impl `impl !Send for T {}` parsing.
void test_negative_marker_impl() {
    auto r = parse("impl !Send for W { }");
    assert(r.ok());
    assert(r.program.impls.size() == 1);
    assert(r.program.impls[0].isNegative);
    assert(r.program.impls[0].traitName == "Send");
    assert(r.program.impls[0].forType.name == "W");
    assert(r.program.impls[0].methods.empty());

    // A positive trait impl is not negative.
    auto r2 = parse("impl Send for W { }");
    assert(r2.ok());
    assert(r2.program.impls.size() == 1);
    assert(!r2.program.impls[0].isNegative);
    assert(r2.program.impls[0].traitName == "Send");

    // The parser only records isNegative; the marker-only restriction is a
    // typecheck rule, so a negative non-marker impl still PARSES.
    auto r3 = parse("impl !Clone for W { }");
    assert(r3.ok());
    assert(r3.program.impls[0].isNegative);

    // A negative impl with a method body is a parse error.
    auto r4 = parse("impl !Send for W { fn g(&self) -> i64 { 0 } }");
    assert(!r4.ok());

    // A negative impl must be `for` a type.
    auto r5 = parse("impl !W { }");
    assert(!r5.ok());
}

} // namespace

int main() {
    test_int_literal();
    test_identifier();
    test_binary_precedence();
    test_logical_or_precedence();
    test_left_associativity();
    test_parenthesized();
    test_function_call();
    test_call_no_args();
    test_if_expression();
    test_let_stmt();
    test_return_stmt();
    test_fn_decl();
    test_fib_full();
    test_parse_error_missing_arrow();
    test_multiple_fns();
    test_struct_decl_empty();
    test_struct_decl_one_field();
    test_struct_decl_trailing_comma();
    test_struct_literal();
    test_struct_literal_empty();
    test_field_access();
    test_chained_field_access();
    test_field_access_precedence();
    test_if_cond_no_struct_lit();
    test_mixed_program_struct_then_fn();
    test_mixed_program_fn_then_struct();
    test_enum_decl_empty();
    test_enum_decl_single_unit_variant();
    test_enum_decl_multi_unit_trailing_comma();
    test_enum_decl_tuple_payload();
    test_enum_decl_multi_arg_tuple_variant();
    test_match_literal_pats();
    test_match_wildcard_only();
    test_match_var_binding();
    test_match_ctor_pat_unit();
    test_match_ctor_pat_nested();
    test_match_scrutinee_no_struct_lit_without_parens();
    test_match_scrutinee_struct_lit_parenthesized();
    test_match_trailing_comma();
    test_mixed_program_enum_and_fn();
    test_fn_decl_single_generic();
    test_fn_decl_multi_generic();
    test_fn_decl_no_generic_still_works();
    test_fn_decl_generic_trailing_comma();
    test_struct_decl_with_generic_param();
    test_enum_decl_with_generic_params();
    test_typeref_with_typeargs();
    test_nested_typeref_typeargs();
    // Phase 3.3 traits + impl + bounded generics
    test_trait_decl_one_method();
    test_impl_decl_basic();
    test_typeparam_with_bound();
    test_method_call_distinguishes_field();
    // Phase 3.4 try operator
    test_try_postfix();
    test_try_chained_after_dot();
    // Phase 4 effect labels
    test_effect_row_on_fn_decl();
    test_effect_row_empty_braces();
    test_no_effect_row_defaults_pure();
    // Phase 10a function types with effect rows
    test_fn_type_param_with_effect_row();
    test_fn_type_param_row_var_no_effects();
    // Phase 10b closures
    test_closure_single_param();
    test_closure_multi_param_with_annotation();
    test_closure_zero_param();
    test_closure_block_body();
    test_closure_as_call_argument();
    // Phase 17a call a fn value produced by an expression
    test_call_value_field();
    test_call_value_field_through_self();
    test_call_value_of_call_result();
    test_bare_call_still_callexpr();
    // Phase 7 mod
    test_mod_decl_basic();
    test_mod_multiple();
    // Phase 9 loops + ranges + assignment
    test_while_expr();
    test_loop_expr();
    test_break_with_value();
    test_break_bare_and_continue();
    test_for_exclusive_range();
    test_for_inclusive_range();
    test_range_binds_looser_than_arith();
    test_let_mut_flag();
    test_assign_stmt();
    test_field_assign_stmt();
    test_while_as_statement_then_tail();
    // Phase 11 dyn Trait, &self, Box
    test_impl_ref_self_param();
    test_dyn_ref_param_type();
    test_box_dyn_let_annotation();
    test_box_new_expr();
    // Phase 15: bool literals, unary ops, else-if, inherent impls, pub
    test_bool_literal_true();
    test_bool_literal_false();
    test_unary_neg_literal();
    test_unary_not();
    test_unary_double_neg();
    test_unary_binds_tighter_than_mul();
    test_unary_not_binds_tighter_than_eq();
    test_unary_neg_binds_looser_than_field();
    test_else_if_chain();
    test_inherent_impl_no_trait();
    test_trait_impl_still_has_trait_name();
    test_pub_struct_enum_trait();
    test_pub_inherent_impl();
    // Phase 16: optional return type (the Drop-method surface)
    test_optional_return_type_fn();
    test_optional_return_type_drop_trait();
    // Phase 21a: generic trait parameters
    test_generic_trait_decl();
    test_generic_trait_multi_params();
    test_generic_trait_impl_with_args();
    test_generic_inherent_impl_unaffected();
    test_parameterized_trait_bound();
    test_unparameterized_bound_has_no_args();
    // Phase 21b: where clauses + associated types.
    test_where_clause_desugars_onto_param();
    test_where_clause_multi_constraint();
    test_where_clause_equals_inline();
    test_where_clause_unknown_param_errors();
    test_where_clause_on_impl_method();
    test_trait_assoc_type_decl();
    test_gat_decl_and_projection();
    test_impl_assoc_type_def();
    test_assoc_projection_on_generic_param();
    // Phase 24: extern "C" FFI declarations.
    test_extern_c_per_decl();
    test_extern_c_no_args_no_ret();
    test_extern_c_i32_and_ref_param();
    test_extern_c_block_form();
    test_extern_c_explicit_effect_row();
    test_extern_c_preserves_abi_string();
    test_extern_c_body_is_error();
    test_extern_and_fn_coexist();
    // Phase 25: const items + const fn + const-expr array lengths.
    test_const_item();
    test_pub_const_item();
    test_const_bool_item();
    test_const_fn_decl();
    test_pub_const_fn_decl();
    test_plain_fn_is_not_const();
    test_array_len_literal_still_literal();
    test_array_len_const_expr();
    test_array_len_const_fn_call();
    // Phase 57 (v10): const-generic params + tuple-let annotation.
    test_const_generic_param();
    test_const_generic_param_non_i64_rejected();
    test_const_generic_arg(); // Phase 58
    test_tuple_let_annotation();
    test_nested_tuple_field_access(); // v10 regression
    test_array_repeat(); // Phase 62
    test_negative_marker_impl(); // v31 Phase 167
    std::cout << "All parser tests passed (139 cases)\n";
    return 0;
}
