// Lexer for the kardashev V1 token grammar.
//
// Recognized tokens:
//   - Integer literals: [0-9]+
//   - Identifiers:      [A-Za-z_][A-Za-z0-9_]*
//   - Keywords:         fn let if else return struct enum match trait impl for mod pub async await true false
//   - Operators:        +  -  *  /  <  <=  >  >=  ==  !=  =  ->  =>  ?  !
//   - Punctuation:      (  )  {  }  ,  ;  :  .  _  &
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
    Float, // Phase 39: f64 literal — DIGIT+ ('.' DIGIT+)? ([eE][+-]?DIGIT+)?
    StringLit, // Phase 5.y: "..." string literal
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
    KwWhile,    // Phase 9: `while cond { ... }`
    KwLoop,     // Phase 9: `loop { ... }`
    KwBreak,    // Phase 9: `break` / `break <value>`
    KwContinue, // Phase 9: `continue`
    KwMod, // Phase 7: `mod foo;` file-import
    KwPub, // Phase 7.2: visibility marker on top-level decls
    KwTrue,  // Phase 15: `true` boolean literal
    KwFalse, // Phase 15: `false` boolean literal
    KwExtern, // Phase 24: `extern "C" fn ...;` FFI declaration
    KwConst,  // Phase 25: `const NAME: T = ...;` item + `const fn` qualifier
    // Note: `async` / `await` stay as Identifiers — they appear in
    // effect rows (`! { async }`) and need lexeme-level lookup in the
    // parser's top-level / postfix logic anyway. Making them keywords
    // would force every callsite to add KwAsync/KwAwait branches.
    DoubleColon, // :: — Phase 7.2 path syntax
    // `Self` and `self` stay as Identifiers — the typechecker rewrites
    // `Self` to the implementing type inside trait method sigs / impl
    // bodies, and `self` is just a parameter name.

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,  // % — Phase 33 integer modulo
    AmpAmp,   // && — Phase 33 short-circuit logical-and
    Lt,
    Le,
    Gt,
    Ge,
    EqEq,
    NotEq,
    Eq,
    Arrow, // ->
    FatArrow, // =>
    DotDot,   // .. — Phase 9 exclusive range
    DotDotEq, // ..= — Phase 9 inclusive range
    Question, // ? — Phase 3.4 try operator
    Ampersand, // & — Phase 2.4b shared borrow / reference type
    Bang, // ! — Phase 4 effect-row introducer (the `!=` two-char form has
          // its own token so this is unambiguous)
    Pipe,     // | — Phase 10b closure-param delimiter (no bitwise-or yet, so
              //     this token is unambiguous outside closure syntax)
    PipePipe, // || — Phase 10b zero-param closure `|| expr`

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket, // Phase 13b: `[` (slice types `&[T]` + slice exprs `&v[a..b]`)
    Pound,    // Phase 42: `#` — attribute lead-in for `#[derive(...)]`
    RBracket, // Phase 13b: `]`
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
