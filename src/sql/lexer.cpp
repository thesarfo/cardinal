#include "sql/lexer.h"

#include <cctype>
#include <string>
#include <unordered_map>

namespace cardinal {

namespace {

const std::unordered_map<std::string, TokenKind>& keywords() {
    static const std::unordered_map<std::string, TokenKind> table = {
        {"CREATE", TokenKind::Create},   {"TABLE", TokenKind::Table},
        {"INSERT", TokenKind::Insert},   {"INTO", TokenKind::Into},
        {"VALUES", TokenKind::Values},   {"SELECT", TokenKind::Select},
        {"FROM", TokenKind::From},       {"WHERE", TokenKind::Where},
        {"ORDER", TokenKind::Order},     {"BY", TokenKind::By},
        {"ASC", TokenKind::Asc},         {"DESC", TokenKind::Desc},
        {"LIMIT", TokenKind::Limit},     {"EXPLAIN", TokenKind::Explain},
        {"AND", TokenKind::And},         {"OR", TokenKind::Or},
        {"NOT", TokenKind::Not},         {"IS", TokenKind::Is},
        {"NULL", TokenKind::Null},       {"BETWEEN", TokenKind::Between},
        {"IN", TokenKind::In},           {"TRUE", TokenKind::True},
        {"FALSE", TokenKind::False},     {"INT", TokenKind::IntType},
        {"DOUBLE", TokenKind::DoubleType}, {"TEXT", TokenKind::TextType},
        {"BOOL", TokenKind::BoolType},   {"JOIN", TokenKind::Join},
        {"INNER", TokenKind::Inner},     {"ON", TokenKind::On},
        {"AS", TokenKind::As},
    };
    return table;
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

class Lexer {
public:
    explicit Lexer(std::string_view sql) : sql_(sql) {}

    std::vector<Token> run() {
        std::vector<Token> tokens;
        for (;;) {
            skip_space();
            if (pos_ >= sql_.size()) break;
            Token t = next();
            bool failed = t.kind == TokenKind::Error;
            tokens.push_back(std::move(t));
            if (failed) break;
        }
        tokens.push_back({TokenKind::End, "", line_, column_});
        return tokens;
    }

private:
    char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < sql_.size() ? sql_[pos_ + ahead] : '\0';
    }

    char advance() {
        char c = sql_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    void skip_space() {
        while (pos_ < sql_.size() && std::isspace(static_cast<unsigned char>(peek()))) advance();
    }

    Token next() {
        const int line = line_, column = column_;
        const char c = peek();

        if (is_ident_start(c)) return word(line, column);
        if (is_digit(c)) return number(line, column);
        if (c == '\'') return string(line, column);

        auto op = [&](TokenKind kind, std::size_t length) {
            std::string text(sql_.substr(pos_, length));
            for (std::size_t i = 0; i < length; ++i) advance();
            return Token{kind, std::move(text), line, column};
        };
        switch (c) {
            case '+': return op(TokenKind::Plus, 1);
            case '-': return op(TokenKind::Minus, 1);
            case '*': return op(TokenKind::Star, 1);
            case '/': return op(TokenKind::Slash, 1);
            case '%': return op(TokenKind::Percent, 1);
            case '=': return op(TokenKind::Eq, 1);
            case '(': return op(TokenKind::LParen, 1);
            case ')': return op(TokenKind::RParen, 1);
            case ',': return op(TokenKind::Comma, 1);
            case '.': return op(TokenKind::Dot, 1);
            case ';': return op(TokenKind::Semicolon, 1);
            case '<':
                if (peek(1) == '=') return op(TokenKind::Le, 2);
                if (peek(1) == '>') return op(TokenKind::Ne, 2);
                return op(TokenKind::Lt, 1);
            case '>':
                if (peek(1) == '=') return op(TokenKind::Ge, 2);
                return op(TokenKind::Gt, 1);
            case '!':
                if (peek(1) == '=') return op(TokenKind::Ne, 2);
                break;
        }
        return {TokenKind::Error, std::string("unexpected character '") + c + "'", line, column};
    }

    Token word(int line, int column) {
        std::string text;
        while (is_ident_char(peek())) text += advance();
        std::string upper = text;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = keywords().find(upper);
        if (it != keywords().end()) return {it->second, std::move(text), line, column};
        return {TokenKind::Identifier, std::move(text), line, column};
    }

    Token number(int line, int column) {
        std::string text;
        while (is_digit(peek())) text += advance();
        // A '.' only belongs to the number when digits follow it.
        if (peek() == '.' && is_digit(peek(1))) {
            text += advance();
            while (is_digit(peek())) text += advance();
            return {TokenKind::Decimal, std::move(text), line, column};
        }
        return {TokenKind::Integer, std::move(text), line, column};
    }

    Token string(int line, int column) {
        advance();  // opening quote
        std::string text;
        for (;;) {
            if (pos_ >= sql_.size())
                return {TokenKind::Error, "unterminated string", line, column};
            char c = advance();
            if (c != '\'') {
                text += c;
            } else if (peek() == '\'') {
                text += advance();
            } else {
                return {TokenKind::String, std::move(text), line, column};
            }
        }
    }

    std::string_view sql_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

}  // namespace

std::vector<Token> lex(std::string_view sql) { return Lexer(sql).run(); }

const char* token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::Create: return "CREATE";
        case TokenKind::Table: return "TABLE";
        case TokenKind::Insert: return "INSERT";
        case TokenKind::Into: return "INTO";
        case TokenKind::Values: return "VALUES";
        case TokenKind::Select: return "SELECT";
        case TokenKind::From: return "FROM";
        case TokenKind::Where: return "WHERE";
        case TokenKind::Order: return "ORDER";
        case TokenKind::By: return "BY";
        case TokenKind::Asc: return "ASC";
        case TokenKind::Desc: return "DESC";
        case TokenKind::Limit: return "LIMIT";
        case TokenKind::Explain: return "EXPLAIN";
        case TokenKind::And: return "AND";
        case TokenKind::Or: return "OR";
        case TokenKind::Not: return "NOT";
        case TokenKind::Is: return "IS";
        case TokenKind::Null: return "NULL";
        case TokenKind::Between: return "BETWEEN";
        case TokenKind::In: return "IN";
        case TokenKind::True: return "TRUE";
        case TokenKind::False: return "FALSE";
        case TokenKind::Join: return "JOIN";
        case TokenKind::Inner: return "INNER";
        case TokenKind::On: return "ON";
        case TokenKind::As: return "AS";
        case TokenKind::IntType: return "INT";
        case TokenKind::DoubleType: return "DOUBLE";
        case TokenKind::TextType: return "TEXT";
        case TokenKind::BoolType: return "BOOL";
        case TokenKind::Identifier: return "identifier";
        case TokenKind::Integer: return "integer";
        case TokenKind::Decimal: return "decimal";
        case TokenKind::String: return "string";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Eq: return "=";
        case TokenKind::Ne: return "<>";
        case TokenKind::Lt: return "<";
        case TokenKind::Le: return "<=";
        case TokenKind::Gt: return ">";
        case TokenKind::Ge: return ">=";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::Comma: return ",";
        case TokenKind::Dot: return ".";
        case TokenKind::Semicolon: return ";";
        case TokenKind::End: return "end of input";
        case TokenKind::Error: return "error";
    }
    return "?";
}

}  // namespace cardinal
