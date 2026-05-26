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
    std::cout << "All parser tests passed (24 cases)\n";
    return 0;
}
