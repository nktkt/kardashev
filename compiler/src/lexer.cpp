#include "kardashev/lexer.hpp"

#include <cctype>

namespace kardashev {

namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentCont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

TokenKind keywordOrIdent(std::string_view s) {
    if (s == "fn") return TokenKind::KwFn;
    if (s == "let") return TokenKind::KwLet;
    if (s == "if") return TokenKind::KwIf;
    if (s == "else") return TokenKind::KwElse;
    if (s == "return") return TokenKind::KwReturn;
    if (s == "struct") return TokenKind::KwStruct;
    if (s == "enum") return TokenKind::KwEnum;
    if (s == "match") return TokenKind::KwMatch;
    if (s == "trait") return TokenKind::KwTrait;
    if (s == "impl") return TokenKind::KwImpl;
    if (s == "for") return TokenKind::KwFor;
    if (s == "while") return TokenKind::KwWhile;
    if (s == "loop") return TokenKind::KwLoop;
    if (s == "break") return TokenKind::KwBreak;
    if (s == "continue") return TokenKind::KwContinue;
    if (s == "mod") return TokenKind::KwMod;
    if (s == "pub") return TokenKind::KwPub;
    if (s == "true") return TokenKind::KwTrue;
    if (s == "false") return TokenKind::KwFalse;
    if (s == "extern") return TokenKind::KwExtern;
    if (s == "const") return TokenKind::KwConst; // Phase 25
    if (s == "as") return TokenKind::KwAs;        // Phase 65 numeric cast
    // A bare `_` is the wildcard pattern; `_foo` stays an Identifier.
    if (s == "_") return TokenKind::Underscore;
    return TokenKind::Identifier;
}

} // namespace

std::vector<Token> lex(std::string_view source) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    std::size_t line = 1;
    std::size_t col = 1;

    auto push1 = [&](TokenKind k, std::size_t startCol) {
        tokens.push_back({k, std::string(1, source[i]), line, startCol});
        ++i;
        ++col;
    };
    auto push2 = [&](TokenKind k, std::size_t startCol) {
        tokens.push_back({k, std::string(source.substr(i, 2)), line, startCol});
        i += 2;
        col += 2;
    };
    auto push3 = [&](TokenKind k, std::size_t startCol) {
        tokens.push_back({k, std::string(source.substr(i, 3)), line, startCol});
        i += 3;
        col += 3;
    };

    while (i < source.size()) {
        char c = source[i];

        // Whitespace.
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            ++col;
            continue;
        }
        if (c == '\n') {
            ++i;
            ++line;
            col = 1;
            continue;
        }

        // `// line comment` — drop through end of line (the \n itself is
        // re-handled by the whitespace branch on the next iteration). Phase 134:
        // EXACTLY `///` (not `//` or `////+`) is a DOC comment — emit a
        // DocComment token carrying the text (one leading space trimmed).
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            const bool isDoc = i + 2 < source.size() && source[i + 2] == '/' &&
                               (i + 3 >= source.size() || source[i + 3] != '/');
            std::size_t docCol = col;
            std::size_t textStart = i + 3; // first char past the three slashes
            while (i < source.size() && source[i] != '\n') {
                ++i;
                ++col;
            }
            if (isDoc) {
                std::string text(source.substr(textStart, i - textStart));
                if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                tokens.push_back(
                    {TokenKind::DocComment, std::move(text), line, docCol});
            }
            continue;
        }

        std::size_t startCol = col;

        // Phase 5.y: string literal. Supports \n, \\, \" escapes; any
        // other backslash sequence is taken literally so a stray `\` in
        // source survives lexing.
        if (c == '"') {
            ++i;
            ++col;
            std::string val;
            while (i < source.size() && source[i] != '"') {
                char ch = source[i];
                if (ch == '\\' && i + 1 < source.size()) {
                    char e = source[i + 1];
                    if (e == 'n') { val.push_back('\n'); i += 2; col += 2; continue; }
                    if (e == '\\') { val.push_back('\\'); i += 2; col += 2; continue; }
                    if (e == '"') { val.push_back('"'); i += 2; col += 2; continue; }
                    if (e == 't') { val.push_back('\t'); i += 2; col += 2; continue; }
                }
                if (ch == '\n') { ++line; col = 1; ++i; }
                else            { ++col; ++i; }
                val.push_back(ch);
            }
            if (i < source.size() && source[i] == '"') {
                ++i; ++col;
            }
            tokens.push_back({TokenKind::StringLit, std::move(val), line, startCol});
            continue;
        }

        // v27 Phase 147: char literal `'c'` — exactly one Unicode scalar.
        // Supports the escapes `\n \t \r \\ \' \0` and `\u{XXXX}` (1–6 hex).
        // A bare char may be multi-byte UTF-8 in the source; we decode it to a
        // codepoint. The token's lexeme carries the codepoint as a decimal
        // string (the parser turns it into a CharLitExpr). (`'` is unused
        // elsewhere — kardashev has no lifetime syntax — so this is
        // unambiguous.)
        if (c == '\'') {
            ++i;
            ++col;
            unsigned long cp = 0;
            bool haveCp = false;
            if (i < source.size() && source[i] == '\\' && i + 1 < source.size()) {
                char e = source[i + 1];
                i += 2;
                col += 2;
                haveCp = true;
                switch (e) {
                    case 'n': cp = '\n'; break;
                    case 't': cp = '\t'; break;
                    case 'r': cp = '\r'; break;
                    case '\\': cp = '\\'; break;
                    case '\'': cp = '\''; break;
                    case '0': cp = 0; break;
                    case 'u': {
                        // `\u{XXXX}` — skip `{`, read hex, skip `}`.
                        if (i < source.size() && source[i] == '{') {
                            ++i; ++col;
                            cp = 0;
                            while (i < source.size() && source[i] != '}') {
                                char h = source[i];
                                int d = (h >= '0' && h <= '9') ? h - '0'
                                        : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                        : (h >= 'A' && h <= 'F') ? h - 'A' + 10
                                                                 : -1;
                                if (d < 0) break;
                                cp = cp * 16 + static_cast<unsigned long>(d);
                                ++i; ++col;
                            }
                            if (i < source.size() && source[i] == '}') { ++i; ++col; }
                        }
                        break;
                    }
                    default: cp = static_cast<unsigned char>(e); break;
                }
            } else if (i < source.size() && source[i] != '\'') {
                // A raw character — decode a UTF-8 sequence to its codepoint.
                unsigned char b0 = static_cast<unsigned char>(source[i]);
                int extra = (b0 < 0x80)   ? 0
                            : (b0 >> 5) == 0x6 ? 1
                            : (b0 >> 4) == 0xE ? 2
                            : (b0 >> 3) == 0x1E ? 3
                                                : 0;
                if (extra == 0) {
                    cp = b0;
                } else {
                    cp = b0 & (0x7Fu >> (extra + 1));
                    for (int k = 0; k < extra && i + 1 < source.size(); ++k) {
                        ++i;
                        cp = (cp << 6) |
                             (static_cast<unsigned char>(source[i]) & 0x3Fu);
                    }
                }
                ++i;
                col += 1 + extra;
                haveCp = true;
            }
            if (i < source.size() && source[i] == '\'') { ++i; ++col; }
            if (!haveCp) {
                // empty `''` — recover as NUL so the parser still produces a node
                cp = 0;
            }
            tokens.push_back({TokenKind::CharLit, std::to_string(cp), line,
                              startCol});
            continue;
        }

        // Integer or float literal (Phase 39). Scan the integer part, then
        // promote to a float iff a `.` is FOLLOWED BY A DIGIT (so `1.5` is a
        // float, but `1..5` stays the int `1` + the `..` range op) or an
        // exponent `e`/`E` follows. A float is `DIGIT+ ('.' DIGIT+)? ([eE]
        // [+-]? DIGIT+)?` with at least one of the fractional/exponent parts.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = i;
            // A number that immediately follows a `.` is a TUPLE FIELD INDEX
            // (`t.0`, and crucially `n.0.0` = `(n.0).0`), never a float — so we
            // must NOT let `0.0` lex as a single float and swallow the second
            // access. `1.0` elsewhere (after `=`, `(`, `,`, an operator, ...)
            // still lexes as a float.
            bool afterDot =
                !tokens.empty() && tokens.back().kind == TokenKind::Dot;
            // Phase 64 (v11): consume an optional integer SUFFIX `i8/i16/i32/
            // i64/u8/u16/u32/u64` into the token (the parser splits it back
            // out). Shared by the decimal / hex / binary paths below.
            auto consumeIntSuffix = [&]() {
                if (i < source.size() &&
                    (source[i] == 'i' || source[i] == 'u')) {
                    std::size_t j = i + 1, digits = 0;
                    while (j < source.size() &&
                           std::isdigit(static_cast<unsigned char>(source[j]))) {
                        ++j;
                        ++digits;
                    }
                    if (digits > 0) { // e.g. i32, u8
                        col += static_cast<int>(j - i);
                        i = j;
                    }
                }
            };
            // Phase 64: hex `0x..` / binary `0b..` integer literals (never
            // floats; not in tuple-index position).
            if (!afterDot && source[i] == '0' && i + 1 < source.size() &&
                (source[i + 1] == 'x' || source[i + 1] == 'X' ||
                 source[i + 1] == 'b' || source[i + 1] == 'B')) {
                bool hex = (source[i + 1] == 'x' || source[i + 1] == 'X');
                i += 2;
                col += 2;
                auto isDig = [&](char ch) {
                    return hex ? std::isxdigit(static_cast<unsigned char>(ch))
                               : (ch == '0' || ch == '1');
                };
                while (i < source.size() && isDig(source[i])) {
                    ++i;
                    ++col;
                }
                consumeIntSuffix();
                tokens.push_back({TokenKind::Integer,
                                  std::string(source.substr(start, i - start)),
                                  line, startCol});
                continue;
            }
            while (i < source.size() &&
                   std::isdigit(static_cast<unsigned char>(source[i]))) {
                ++i;
                ++col;
            }
            bool isFloat = false;
            // Fractional part: `.` then a digit (NOT `..`, the range op).
            if (!afterDot && i + 1 < source.size() && source[i] == '.' &&
                std::isdigit(static_cast<unsigned char>(source[i + 1]))) {
                isFloat = true;
                ++i;
                ++col; // consume '.'
                while (i < source.size() &&
                       std::isdigit(static_cast<unsigned char>(source[i]))) {
                    ++i;
                    ++col;
                }
            }
            // Exponent: `e`/`E` [+-]? DIGIT+. Also suppressed after a `.` (a
            // tuple index is a plain integer).
            if (!afterDot && i < source.size() &&
                (source[i] == 'e' || source[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < source.size() && (source[j] == '+' || source[j] == '-'))
                    ++j;
                if (j < source.size() &&
                    std::isdigit(static_cast<unsigned char>(source[j]))) {
                    isFloat = true;
                    col += static_cast<int>(j - i);
                    i = j;
                    while (i < source.size() &&
                           std::isdigit(static_cast<unsigned char>(source[i]))) {
                        ++i;
                        ++col;
                    }
                }
            }
            // Phase 67: a trailing `f32`/`f64` suffix. It makes ANY numeric
            // literal a float — even an otherwise-integer one (`5f32` == 5.0f32),
            // mirroring Rust — so it is tried before the integer suffix.
            auto consumeFloatSuffix = [&]() -> bool {
                if (i + 2 < source.size() && source[i] == 'f' &&
                    ((source[i + 1] == '3' && source[i + 2] == '2') ||
                     (source[i + 1] == '6' && source[i + 2] == '4'))) {
                    i += 3;
                    col += 3;
                    return true;
                }
                return false;
            };
            // Phase 64/67: a trailing width suffix. `i`/`u` keeps an integer an
            // integer; `f32`/`f64` makes it a float. NOT after a `.`: a tuple
            // index (`t.0`, `t.0.1`) is a plain integer and must never absorb a
            // following `i`/`u`/`f` (which begins the next field access or an
            // invalid suffix) — same `afterDot` guard the radix/float scanning
            // above uses.
            if (!afterDot && consumeFloatSuffix())
                isFloat = true;
            else if (!afterDot && !isFloat)
                consumeIntSuffix();
            tokens.push_back({isFloat ? TokenKind::Float : TokenKind::Integer,
                              std::string(source.substr(start, i - start)),
                              line, startCol});
            continue;
        }

        // Identifier or keyword.
        if (isIdentStart(c)) {
            std::size_t start = i;
            while (i < source.size() && isIdentCont(source[i])) {
                ++i;
                ++col;
            }
            std::string_view lex = source.substr(start, i - start);
            tokens.push_back(
                {keywordOrIdent(lex), std::string(lex), line, startCol});
            continue;
        }

        // Two-char operators.
        char n = (i + 1 < source.size()) ? source[i + 1] : '\0';
        char n2 = (i + 2 < source.size()) ? source[i + 2] : '\0';
        // Phase 9: range operators. `..=` (inclusive) must be tried before
        // `..` (exclusive) so the longer match wins.
        if (c == '.' && n == '.' && n2 == '=') {
            push3(TokenKind::DotDotEq, startCol);
            continue;
        }
        if (c == '.' && n == '.') {
            push2(TokenKind::DotDot, startCol);
            continue;
        }
        if (c == '-' && n == '>') {
            push2(TokenKind::Arrow, startCol);
            continue;
        }
        if (c == '=' && n == '=') {
            push2(TokenKind::EqEq, startCol);
            continue;
        }
        if (c == '=' && n == '>') {
            push2(TokenKind::FatArrow, startCol);
            continue;
        }
        if (c == '!' && n == '=') {
            push2(TokenKind::NotEq, startCol);
            continue;
        }
        if (c == '<' && n == '=') {
            push2(TokenKind::Le, startCol);
            continue;
        }
        if (c == '>' && n == '=') {
            push2(TokenKind::Ge, startCol);
            continue;
        }
        if (c == ':' && n == ':') {
            push2(TokenKind::DoubleColon, startCol);
            continue;
        }
        // Phase 10b: `||` (zero-param closure) before `|` so the longer
        // match wins. There is no bitwise / logical-or operator in the
        // language, so both forms are unambiguously closure syntax.
        if (c == '|' && n == '|') {
            push2(TokenKind::PipePipe, startCol);
            continue;
        }
        // Phase 33: `&&` short-circuit logical-and, matched before single `&`
        // so the longer match wins. (There is no `||` logical-or — `||` is
        // the zero-param-closure token above.)
        if (c == '&' && n == '&') {
            push2(TokenKind::AmpAmp, startCol);
            continue;
        }

        // Single-char tokens.
        switch (c) {
        case '%': push1(TokenKind::Percent, startCol); continue; // Phase 33
        case '+': push1(TokenKind::Plus, startCol); continue;
        case '-': push1(TokenKind::Minus, startCol); continue;
        case '*': push1(TokenKind::Star, startCol); continue;
        case '/': push1(TokenKind::Slash, startCol); continue;
        case '<': push1(TokenKind::Lt, startCol); continue;
        case '>': push1(TokenKind::Gt, startCol); continue;
        case '=': push1(TokenKind::Eq, startCol); continue;
        case '(': push1(TokenKind::LParen, startCol); continue;
        case ')': push1(TokenKind::RParen, startCol); continue;
        case '{': push1(TokenKind::LBrace, startCol); continue;
        case '}': push1(TokenKind::RBrace, startCol); continue;
        case '[': push1(TokenKind::LBracket, startCol); continue;
        case ']': push1(TokenKind::RBracket, startCol); continue;
        case '#': push1(TokenKind::Pound, startCol); continue; // Phase 42 attrs
        case ',': push1(TokenKind::Comma, startCol); continue;
        case ';': push1(TokenKind::Semi, startCol); continue;
        case ':': push1(TokenKind::Colon, startCol); continue;
        case '.': push1(TokenKind::Dot, startCol); continue;
        case '?': push1(TokenKind::Question, startCol); continue;
        case '&': push1(TokenKind::Ampersand, startCol); continue;
        case '!': push1(TokenKind::Bang, startCol); continue;
        case '|': push1(TokenKind::Pipe, startCol); continue;
        case '^': push1(TokenKind::Caret, startCol); continue; // Phase 66 bit-xor
        case '~': push1(TokenKind::Tilde, startCol); continue; // Phase 66 bit-not
        default: break;
        }

        // Unrecognized.
        tokens.push_back({TokenKind::Invalid, std::string(1, c), line, startCol});
        ++i;
        ++col;
    }

    tokens.push_back({TokenKind::EndOfInput, "", line, col});
    return tokens;
}

std::string_view tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::Integer: return "Integer";
    case TokenKind::Float: return "Float";
    case TokenKind::StringLit: return "StringLit";
    case TokenKind::CharLit: return "CharLit";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::KwFn: return "KwFn";
    case TokenKind::KwLet: return "KwLet";
    case TokenKind::KwIf: return "KwIf";
    case TokenKind::KwElse: return "KwElse";
    case TokenKind::KwReturn: return "KwReturn";
    case TokenKind::KwStruct: return "KwStruct";
    case TokenKind::KwEnum: return "KwEnum";
    case TokenKind::KwMatch: return "KwMatch";
    case TokenKind::KwTrait: return "KwTrait";
    case TokenKind::KwImpl: return "KwImpl";
    case TokenKind::KwFor: return "KwFor";
    case TokenKind::KwWhile: return "KwWhile";
    case TokenKind::KwLoop: return "KwLoop";
    case TokenKind::KwBreak: return "KwBreak";
    case TokenKind::KwContinue: return "KwContinue";
    case TokenKind::KwMod: return "KwMod";
    case TokenKind::KwPub: return "KwPub";
    case TokenKind::KwTrue: return "KwTrue";
    case TokenKind::KwFalse: return "KwFalse";
    case TokenKind::KwExtern: return "KwExtern";
    case TokenKind::KwConst: return "KwConst";
    case TokenKind::KwAs: return "KwAs";
    case TokenKind::DoubleColon: return "DoubleColon";
    case TokenKind::Plus: return "Plus";
    case TokenKind::Minus: return "Minus";
    case TokenKind::Star: return "Star";
    case TokenKind::Slash: return "Slash";
    case TokenKind::Percent: return "Percent";
    case TokenKind::AmpAmp: return "AmpAmp";
    case TokenKind::Lt: return "Lt";
    case TokenKind::Le: return "Le";
    case TokenKind::Gt: return "Gt";
    case TokenKind::Ge: return "Ge";
    case TokenKind::EqEq: return "EqEq";
    case TokenKind::NotEq: return "NotEq";
    case TokenKind::Eq: return "Eq";
    case TokenKind::Arrow: return "Arrow";
    case TokenKind::FatArrow: return "FatArrow";
    case TokenKind::DotDot: return "DotDot";
    case TokenKind::DotDotEq: return "DotDotEq";
    case TokenKind::LParen: return "LParen";
    case TokenKind::RParen: return "RParen";
    case TokenKind::LBrace: return "LBrace";
    case TokenKind::RBrace: return "RBrace";
    case TokenKind::LBracket: return "LBracket";
    case TokenKind::Pound: return "Pound";
    case TokenKind::RBracket: return "RBracket";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Semi: return "Semi";
    case TokenKind::Colon: return "Colon";
    case TokenKind::Dot: return "Dot";
    case TokenKind::Question: return "Question";
    case TokenKind::Ampersand: return "Ampersand";
    case TokenKind::Bang: return "Bang";
    case TokenKind::Pipe: return "Pipe";
    case TokenKind::PipePipe: return "PipePipe";
    case TokenKind::Caret: return "Caret";
    case TokenKind::Tilde: return "Tilde";
    case TokenKind::Underscore: return "Underscore";
    case TokenKind::EndOfInput: return "EndOfInput";
    case TokenKind::Invalid: return "Invalid";
    }
    return "<?>";
}

} // namespace kardashev
