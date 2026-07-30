#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cardinal {

enum class TokenKind {
    // Keywords
    Create, Table, Insert, Into, Values, Select, From, Where, Order, By, Asc, Desc,
    Limit, Explain, And, Or, Not, Is, Null, Between, In, True, False,
    IntType, DoubleType, TextType, BoolType,
    // Names and literals
    Identifier, Integer, Decimal, String,
    // Operators and punctuation
    Plus, Minus, Star, Slash, Percent, Eq, Ne, Lt, Le, Gt, Ge,
    LParen, RParen, Comma, Dot, Semicolon,
    // End of input, or the first thing the lexer could not read (message in `text`).
    End, Error,
};

struct Token {
    TokenKind kind;
    // Identifiers keep their case. Strings hold their contents with '' unescaped.
    // Numbers and operators hold the source text. Errors hold a one-line message.
    std::string text;
    int line;    // 1-based
    int column;  // 1-based
};

// Always ends with End, or with Error then End. Stops at the first error.
std::vector<Token> lex(std::string_view sql);

const char* token_kind_name(TokenKind kind);

}  // namespace cardinal
