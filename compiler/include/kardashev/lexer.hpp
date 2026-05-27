// Lexer for the kardashev V1 token grammar.
//
// Recognized tokens:
//   - Integer literals: [0-9]+
//   - Identifiers:      [A-Za-z_][A-Za-z0-9_]*
//   - Keywords:         fn let if else return struct enum match trait impl for
//   - Operators:        +  -  *  /  <  <=  >  >=  ==  !=  =  ->  =>  ?
//   - Punctuation:      (  )  {  }  ,  ;  :  .  _
//   - Skipped:          whitespace, `// ... \n` line comments
//
// Line / column counts start at 1.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kardashev {

enum class TokenKind {
    // Literals & identifiers
    Integer,
    Identifier,

    // Keywords
    KwFn,
    KwLet,
    KwIf,
    KwElse,
    KwReturn,
    KwStruct,
    KwEnum,
    KwMatch,
    KwTrait,
    KwImpl,
    KwFor,
    // `Self` and `self` stay as Identifiers — the typechecker rewrites
    // `Self` to the implementing type inside trait method sigs / impl
    // bodies, and `self` is just a parameter name.

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Lt,
    Le,
    Gt,
    Ge,
    EqEq,
    NotEq,
    Eq,
    Arrow, // ->
    FatArrow, // =>
    Question, // ? — Phase 3.4 try operator

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semi,
    Colon,
    Dot,
    Underscore,

    // Sentinel
    EndOfInput,
    Invalid,
};

struct Token {
    TokenKind kind;
    std::string lexeme;
    std::size_t line;
    std::size_t column;
};

// Tokenize the entire input. The returned vector always ends with a single
// TokenKind::EndOfInput token. Lex errors (unrecognized characters) yield a
// TokenKind::Invalid token whose lexeme is the offending character — the
// caller is responsible for surfacing the diagnostic.
std::vector<Token> lex(std::string_view source);

// Human-readable name for a token kind, useful in tests / diagnostics.
std::string_view tokenKindName(TokenKind kind);

} // namespace kardashev
