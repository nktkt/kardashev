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

} // namespace

int main() {
    test_int_literal();
    test_identifier();
    test_binary_precedence();
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
    // Phase 7 mod
    test_mod_decl_basic();
    test_mod_multiple();
    std::cout << "All parser tests passed (56 cases)\n";
    return 0;
}
