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
        // re-handled by the whitespace branch on the next iteration).
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') {
                ++i;
                ++col;
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

        // Integer literal.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = i;
            while (i < source.size() &&
                   std::isdigit(static_cast<unsigned char>(source[i]))) {
                ++i;
                ++col;
            }
            tokens.push_back({TokenKind::Integer,
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

        // Single-char tokens.
        switch (c) {
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
        case ',': push1(TokenKind::Comma, startCol); continue;
        case ';': push1(TokenKind::Semi, startCol); continue;
        case ':': push1(TokenKind::Colon, startCol); continue;
        case '.': push1(TokenKind::Dot, startCol); continue;
        case '?': push1(TokenKind::Question, startCol); continue;
        case '&': push1(TokenKind::Ampersand, startCol); continue;
        case '!': push1(TokenKind::Bang, startCol); continue;
        case '|': push1(TokenKind::Pipe, startCol); continue;
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
    case TokenKind::StringLit: return "StringLit";
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
    case TokenKind::DoubleColon: return "DoubleColon";
    case TokenKind::Plus: return "Plus";
    case TokenKind::Minus: return "Minus";
    case TokenKind::Star: return "Star";
    case TokenKind::Slash: return "Slash";
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
    case TokenKind::Underscore: return "Underscore";
    case TokenKind::EndOfInput: return "EndOfInput";
    case TokenKind::Invalid: return "Invalid";
    }
    return "<?>";
}

} // namespace kardashev
