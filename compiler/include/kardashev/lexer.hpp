// Lexer for the kardashev V1 token grammar.
//
// Recognized tokens:
//   - Integer literals: [0-9]+
//   - Identifiers:      [A-Za-z_][A-Za-z0-9_]*
//   - Keywords:         fn let if else return struct
//   - Operators:        +  -  *  /  <  <=  >  >=  ==  !=  =  ->
//   - Punctuation:      (  )  {  }  ,  ;  :  .
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

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semi,
    Colon,
    Dot,

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
