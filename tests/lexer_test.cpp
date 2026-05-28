// Unit tests for kardashev::lex. Uses plain <cassert>; passing == exit 0.

#include "kardashev/lexer.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

using kardashev::lex;
using kardashev::Token;
using kardashev::TokenKind;
using kardashev::tokenKindName;

namespace {

// Tiny helper to surface which assertion fired without dragging in a real
// test framework. ASSERT_EQ_K compares a token's kind against `expected`
// and dumps a one-line diagnostic on failure before assert() terminates.
void assertKind(const Token& tok, TokenKind expected, const char* label) {
    if (tok.kind != expected) {
        std::cerr << "assertion failed: " << label
                  << ": got " << tokenKindName(tok.kind)
                  << " (lexeme '" << tok.lexeme << "'), expected "
                  << tokenKindName(expected) << '\n';
        std::abort();
    }
}

#define ASSERT_KIND(tok, expected) assertKind((tok), (expected), #expected)

void test_empty() {
    auto t = lex("");
    assert(t.size() == 1);
    ASSERT_KIND(t[0], TokenKind::EndOfInput);
}

void test_arithmetic() {
    auto t = lex("1 + 2 * 3");
    assert(t.size() == 6);
    ASSERT_KIND(t[0], TokenKind::Integer);
    assert(t[0].lexeme == "1");
    ASSERT_KIND(t[1], TokenKind::Plus);
    ASSERT_KIND(t[2], TokenKind::Integer);
    assert(t[2].lexeme == "2");
    ASSERT_KIND(t[3], TokenKind::Star);
    ASSERT_KIND(t[4], TokenKind::Integer);
    assert(t[4].lexeme == "3");
    ASSERT_KIND(t[5], TokenKind::EndOfInput);
}

void test_keywords_vs_identifiers() {
    auto t = lex("fn fib let n if else return foo");
    ASSERT_KIND(t[0], TokenKind::KwFn);
    ASSERT_KIND(t[1], TokenKind::Identifier);
    assert(t[1].lexeme == "fib");
    ASSERT_KIND(t[2], TokenKind::KwLet);
    ASSERT_KIND(t[3], TokenKind::Identifier);
    assert(t[3].lexeme == "n");
    ASSERT_KIND(t[4], TokenKind::KwIf);
    ASSERT_KIND(t[5], TokenKind::KwElse);
    ASSERT_KIND(t[6], TokenKind::KwReturn);
    ASSERT_KIND(t[7], TokenKind::Identifier);
    assert(t[7].lexeme == "foo");
}

void test_multichar_ops() {
    auto t = lex("a <= b != c >= d == e -> f");
    ASSERT_KIND(t[1], TokenKind::Le);
    ASSERT_KIND(t[3], TokenKind::NotEq);
    ASSERT_KIND(t[5], TokenKind::Ge);
    ASSERT_KIND(t[7], TokenKind::EqEq);
    ASSERT_KIND(t[9], TokenKind::Arrow);
}

void test_punctuation() {
    auto t = lex("fn f() { return 42; }");
    ASSERT_KIND(t[0], TokenKind::KwFn);
    ASSERT_KIND(t[1], TokenKind::Identifier);
    ASSERT_KIND(t[2], TokenKind::LParen);
    ASSERT_KIND(t[3], TokenKind::RParen);
    ASSERT_KIND(t[4], TokenKind::LBrace);
    ASSERT_KIND(t[5], TokenKind::KwReturn);
    ASSERT_KIND(t[6], TokenKind::Integer);
    assert(t[6].lexeme == "42");
    ASSERT_KIND(t[7], TokenKind::Semi);
    ASSERT_KIND(t[8], TokenKind::RBrace);
    ASSERT_KIND(t[9], TokenKind::EndOfInput);
}

void test_comments_and_newlines() {
    auto t = lex("1 // first comment\n+ 2 // trailing\n");
    assert(t.size() == 4); // 1 + 2 EOF
    ASSERT_KIND(t[0], TokenKind::Integer);
    assert(t[0].lexeme == "1");
    assert(t[0].line == 1);
    ASSERT_KIND(t[1], TokenKind::Plus);
    assert(t[1].line == 2);
    ASSERT_KIND(t[2], TokenKind::Integer);
    assert(t[2].lexeme == "2");
    ASSERT_KIND(t[3], TokenKind::EndOfInput);
}

void test_line_column_tracking() {
    auto t = lex("foo\n  bar");
    assert(t[0].line == 1 && t[0].column == 1);
    assert(t[1].line == 2 && t[1].column == 3); // after two spaces
}

void test_fib_signature() {
    // The MVP target: this exact line must lex cleanly.
    auto t = lex(
        "fn fib(n: i64) -> i64 { if n < 2 { n } else { fib(n-1) + fib(n-2) } }");
    // Spot-check key tokens; full sequence is implicitly validated by
    // not producing any TokenKind::Invalid.
    ASSERT_KIND(t[0], TokenKind::KwFn);
    assert(t[1].lexeme == "fib");
    ASSERT_KIND(t[2], TokenKind::LParen);
    ASSERT_KIND(t[4], TokenKind::Colon);
    ASSERT_KIND(t[6], TokenKind::RParen);
    ASSERT_KIND(t[7], TokenKind::Arrow);
    for (const auto& tok : t) {
        if (tok.kind == TokenKind::Invalid) {
            std::cerr << "test_fib_signature: unexpected Invalid token: '"
                      << tok.lexeme << "' at " << tok.line << ":" << tok.column
                      << '\n';
            std::abort();
        }
    }
}

void test_invalid_char() {
    auto t = lex("a @ b");
    ASSERT_KIND(t[0], TokenKind::Identifier);
    ASSERT_KIND(t[1], TokenKind::Invalid);
    assert(t[1].lexeme == "@");
    ASSERT_KIND(t[2], TokenKind::Identifier);
}

void test_struct_keyword() {
    auto t = lex("struct");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::KwStruct);
    assert(t[0].lexeme == "struct");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_dot_token() {
    auto t = lex(".");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::Dot);
    assert(t[0].lexeme == ".");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_struct_decl_and_member_access() {
    auto t = lex("struct Point { x : i64 } p . x");
    ASSERT_KIND(t[0], TokenKind::KwStruct);
    ASSERT_KIND(t[1], TokenKind::Identifier);
    assert(t[1].lexeme == "Point");
    ASSERT_KIND(t[2], TokenKind::LBrace);
    ASSERT_KIND(t[3], TokenKind::Identifier);
    assert(t[3].lexeme == "x");
    ASSERT_KIND(t[4], TokenKind::Colon);
    ASSERT_KIND(t[5], TokenKind::Identifier);
    assert(t[5].lexeme == "i64");
    ASSERT_KIND(t[6], TokenKind::RBrace);
    ASSERT_KIND(t[7], TokenKind::Identifier);
    assert(t[7].lexeme == "p");
    ASSERT_KIND(t[8], TokenKind::Dot);
    ASSERT_KIND(t[9], TokenKind::Identifier);
    assert(t[9].lexeme == "x");
    ASSERT_KIND(t[10], TokenKind::EndOfInput);
}

void test_enum_keyword() {
    auto t = lex("enum");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::KwEnum);
    assert(t[0].lexeme == "enum");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_match_keyword() {
    auto t = lex("match");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::KwMatch);
    assert(t[0].lexeme == "match");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_fat_arrow_token() {
    auto t = lex("=>");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::FatArrow);
    assert(t[0].lexeme == "=>");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_underscore_token() {
    auto t = lex("_");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::Underscore);
    assert(t[0].lexeme == "_");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_underscore_prefix_is_ident() {
    // Regression: `_foo` must remain an Identifier, not Underscore + foo.
    auto t = lex("_foo");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::Identifier);
    assert(t[0].lexeme == "_foo");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_eqeq_not_split_by_fat_arrow() {
    // Regression: adding `=>` must not break `==` lexing.
    auto t = lex("==");
    assert(t.size() == 2);
    ASSERT_KIND(t[0], TokenKind::EqEq);
    assert(t[0].lexeme == "==");
    ASSERT_KIND(t[1], TokenKind::EndOfInput);
}

void test_match_arm_combined() {
    auto t = lex("match m { Some(x) => x, _ => 0 }");
    ASSERT_KIND(t[0], TokenKind::KwMatch);
    ASSERT_KIND(t[1], TokenKind::Identifier);
    assert(t[1].lexeme == "m");
    ASSERT_KIND(t[2], TokenKind::LBrace);
    ASSERT_KIND(t[3], TokenKind::Identifier);
    assert(t[3].lexeme == "Some");
    ASSERT_KIND(t[4], TokenKind::LParen);
    ASSERT_KIND(t[5], TokenKind::Identifier);
    assert(t[5].lexeme == "x");
    ASSERT_KIND(t[6], TokenKind::RParen);
    ASSERT_KIND(t[7], TokenKind::FatArrow);
    ASSERT_KIND(t[8], TokenKind::Identifier);
    assert(t[8].lexeme == "x");
    ASSERT_KIND(t[9], TokenKind::Comma);
    ASSERT_KIND(t[10], TokenKind::Underscore);
    ASSERT_KIND(t[11], TokenKind::FatArrow);
    ASSERT_KIND(t[12], TokenKind::Integer);
    assert(t[12].lexeme == "0");
    ASSERT_KIND(t[13], TokenKind::RBrace);
    ASSERT_KIND(t[14], TokenKind::EndOfInput);
}

// Phase 9: loop keywords + range operators.
void test_loop_keywords() {
    auto t = lex("while loop break continue for");
    ASSERT_KIND(t[0], TokenKind::KwWhile);
    ASSERT_KIND(t[1], TokenKind::KwLoop);
    ASSERT_KIND(t[2], TokenKind::KwBreak);
    ASSERT_KIND(t[3], TokenKind::KwContinue);
    ASSERT_KIND(t[4], TokenKind::KwFor);
    ASSERT_KIND(t[5], TokenKind::EndOfInput);
}

void test_range_operators() {
    // `..=` must out-prioritize `..` (longest match), and `..` must not be
    // split into two `.` tokens.
    auto t = lex("0..10 1..=5");
    ASSERT_KIND(t[0], TokenKind::Integer);
    ASSERT_KIND(t[1], TokenKind::DotDot);
    ASSERT_KIND(t[2], TokenKind::Integer);
    ASSERT_KIND(t[3], TokenKind::Integer);
    ASSERT_KIND(t[4], TokenKind::DotDotEq);
    ASSERT_KIND(t[5], TokenKind::Integer);
    ASSERT_KIND(t[6], TokenKind::EndOfInput);
}

// Phase 15: `true` / `false` are keyword tokens; `trueish` stays an Ident.
void test_bool_keywords() {
    auto t = lex("true false trueish");
    ASSERT_KIND(t[0], TokenKind::KwTrue);
    ASSERT_KIND(t[1], TokenKind::KwFalse);
    ASSERT_KIND(t[2], TokenKind::Identifier);
    assert(t[2].lexeme == "trueish");
}

// Phase 25: `const` is a keyword token; `constant` / `const_x` stay Idents.
void test_const_keyword() {
    auto t = lex("const constant const_x");
    ASSERT_KIND(t[0], TokenKind::KwConst);
    ASSERT_KIND(t[1], TokenKind::Identifier);
    assert(t[1].lexeme == "constant");
    ASSERT_KIND(t[2], TokenKind::Identifier);
    assert(t[2].lexeme == "const_x");
}

} // namespace

int main() {
    test_empty();
    test_arithmetic();
    test_keywords_vs_identifiers();
    test_multichar_ops();
    test_punctuation();
    test_comments_and_newlines();
    test_line_column_tracking();
    test_fib_signature();
    test_invalid_char();
    test_struct_keyword();
    test_dot_token();
    test_struct_decl_and_member_access();
    test_enum_keyword();
    test_match_keyword();
    test_fat_arrow_token();
    test_underscore_token();
    test_underscore_prefix_is_ident();
    test_eqeq_not_split_by_fat_arrow();
    test_match_arm_combined();
    test_loop_keywords();
    test_range_operators();
    test_bool_keywords();
    test_const_keyword();
    std::cout << "All lexer tests passed (23 cases)\n";
    return 0;
}
