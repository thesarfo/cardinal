#include "sql/parser.h"

#include <charconv>
#include <vector>

#include "sql/lexer.h"

namespace cardinal {

namespace {

// Binding powers, loosest to tightest. An operator is taken when its power is at
// least the `min_bp` the caller asked for.
constexpr int kOr = 1;
constexpr int kAnd = 2;
constexpr int kNot = 3;      // prefix NOT: `NOT a = b` means NOT (a = b)
constexpr int kCompare = 4;  // = <> < <= > >= IS BETWEEN IN
constexpr int kAdd = 5;
constexpr int kMul = 6;
constexpr int kNeg = 7;  // prefix minus

struct BinaryInfo {
    BinaryOp op;
    int bp;
};

std::optional<BinaryInfo> binary_info(TokenKind kind) {
    switch (kind) {
        case TokenKind::Or: return BinaryInfo{BinaryOp::Or, kOr};
        case TokenKind::And: return BinaryInfo{BinaryOp::And, kAnd};
        case TokenKind::Eq: return BinaryInfo{BinaryOp::Eq, kCompare};
        case TokenKind::Ne: return BinaryInfo{BinaryOp::Ne, kCompare};
        case TokenKind::Lt: return BinaryInfo{BinaryOp::Lt, kCompare};
        case TokenKind::Le: return BinaryInfo{BinaryOp::Le, kCompare};
        case TokenKind::Gt: return BinaryInfo{BinaryOp::Gt, kCompare};
        case TokenKind::Ge: return BinaryInfo{BinaryOp::Ge, kCompare};
        case TokenKind::Plus: return BinaryInfo{BinaryOp::Add, kAdd};
        case TokenKind::Minus: return BinaryInfo{BinaryOp::Sub, kAdd};
        case TokenKind::Star: return BinaryInfo{BinaryOp::Mul, kMul};
        case TokenKind::Slash: return BinaryInfo{BinaryOp::Div, kMul};
        case TokenKind::Percent: return BinaryInfo{BinaryOp::Mod, kMul};
        default: return std::nullopt;
    }
}

class Parser {
public:
    explicit Parser(std::string_view sql) : tokens_(lex(sql)) { fail_on_lex_error(); }

    ExprPtr expression_to_end() {
        ExprPtr e = expression(kOr);
        if (peek().kind != TokenKind::End) error_at(peek(), "expected end of input");
        return e;
    }

    Statement statement_to_end() {
        Statement s = statement(/*allow_explain=*/true);
        match(TokenKind::Semicolon);
        if (peek().kind != TokenKind::End) error_at(peek(), "expected end of input");
        return s;
    }

private:
    const Token& peek(std::size_t ahead = 0) const {
        std::size_t i = pos_ + ahead;
        return i < tokens_.size() ? tokens_[i] : tokens_.back();  // back() is End
    }

    const Token& advance() {
        const Token& t = peek();
        if (pos_ < tokens_.size() - 1) ++pos_;
        return t;
    }

    bool match(TokenKind kind) {
        if (peek().kind != kind) return false;
        advance();
        return true;
    }

    void expect(TokenKind kind) {
        if (!match(kind))
            error_at(peek(), std::string("expected '") + token_kind_name(kind) + "'");
    }

    [[noreturn]] void error_at(const Token& t, const std::string& message) const {
        throw ParseError(message, t.line, t.column);
    }

    void fail_on_lex_error() const {
        for (const Token& t : tokens_)
            if (t.kind == TokenKind::Error) error_at(t, t.text);
    }

    Statement statement(bool allow_explain) {
        const Token& t = peek();
        switch (t.kind) {
            case TokenKind::Create: return Statement{create_table()};
            case TokenKind::Insert: return Statement{insert()};
            case TokenKind::Select: return Statement{select()};
            case TokenKind::Explain:
                if (!allow_explain) break;
                advance();
                return Statement{
                    Explain{std::make_unique<Statement>(statement(/*allow_explain=*/false))}};
            default: break;
        }
        error_at(t, "expected a statement");
    }

    std::string name(const char* what) {
        const Token& t = peek();
        if (t.kind != TokenKind::Identifier) error_at(t, std::string("expected ") + what);
        advance();
        return t.text;
    }

    TableRef table_ref() {
        TableRef ref;
        ref.table = name("a table name");
        // `users u` and `users AS u`. A keyword can't be an alias, so WHERE, JOIN... end the table.
        if (match(TokenKind::As)) {
            ref.alias = name("an alias after AS");
        } else if (peek().kind == TokenKind::Identifier) {
            ref.alias = advance().text;
        }
        return ref;
    }

    Type column_type() {
        const Token& t = advance();
        switch (t.kind) {
            case TokenKind::IntType: return Type::Int;
            case TokenKind::DoubleType: return Type::Double;
            case TokenKind::TextType: return Type::Text;
            case TokenKind::BoolType: return Type::Bool;
            default: error_at(t, "expected a column type (INT, DOUBLE, TEXT or BOOL)");
        }
    }

    CreateTable create_table() {
        expect(TokenKind::Create);
        expect(TokenKind::Table);
        CreateTable ct;
        ct.name = name("a table name");
        expect(TokenKind::LParen);
        do {
            std::string column = name("a column name");
            ct.columns.push_back({std::move(column), column_type()});
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen);
        return ct;
    }

    Insert insert() {
        expect(TokenKind::Insert);
        expect(TokenKind::Into);
        Insert ins;
        ins.table = name("a table name");
        expect(TokenKind::Values);
        do {
            expect(TokenKind::LParen);
            std::vector<ExprPtr> row;
            do {
                row.push_back(expression(kOr));
            } while (match(TokenKind::Comma));
            expect(TokenKind::RParen);
            ins.rows.push_back(std::move(row));
        } while (match(TokenKind::Comma));
        return ins;
    }

    Select select() {
        expect(TokenKind::Select);
        Select sel;
        if (match(TokenKind::Star)) {
            sel.items.push_back({Star{}});
        } else {
            do {
                sel.items.push_back({expression(kOr)});
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::From);
        sel.from = table_ref();
        for (;;) {
            if (match(TokenKind::Comma)) {
                sel.joins.push_back({table_ref(), nullptr});
            } else if (peek().kind == TokenKind::Join || peek().kind == TokenKind::Inner) {
                if (match(TokenKind::Inner)) expect(TokenKind::Join);
                else advance();
                TableRef table = table_ref();
                expect(TokenKind::On);
                sel.joins.push_back({std::move(table), expression(kOr)});
            } else {
                break;
            }
        }
        if (match(TokenKind::Where)) sel.where = expression(kOr);
        if (match(TokenKind::Order)) {
            expect(TokenKind::By);
            do {
                ExprPtr key = expression(kOr);
                bool descending = false;
                if (match(TokenKind::Desc)) {
                    descending = true;
                } else {
                    match(TokenKind::Asc);
                }
                sel.order_by.push_back({std::move(key), descending});
            } while (match(TokenKind::Comma));
        }
        if (match(TokenKind::Limit)) {
            const Token& t = peek();
            if (t.kind != TokenKind::Integer) error_at(t, "expected a whole number after LIMIT");
            advance();
            sel.limit = integer(t);
        }
        return sel;
    }

    std::int64_t integer(const Token& t) const {
        std::int64_t v = 0;
        auto [end, ec] = std::from_chars(t.text.data(), t.text.data() + t.text.size(), v);
        if (ec != std::errc()) error_at(t, "integer is too large");
        return v;
    }

    ExprPtr expression(int min_bp) {
        ExprPtr left = prefix();
        for (;;) {
            const Token& t = peek();
            if (auto info = binary_info(t.kind)) {
                if (info->bp < min_bp) break;
                advance();
                ExprPtr right = expression(info->bp + 1);  // +1 makes it left-associative
                left = binary(info->op, std::move(left), std::move(right));
                continue;
            }
            if (kCompare < min_bp) break;
            if (t.kind == TokenKind::Is) {
                advance();
                bool negated = match(TokenKind::Not);
                expect(TokenKind::Null);
                left = make_expr(IsNull{std::move(left), negated});
                continue;
            }
            // `x NOT BETWEEN ...` and `x NOT IN ...`. A lone NOT here is left for the caller.
            bool negated = t.kind == TokenKind::Not &&
                           (peek(1).kind == TokenKind::Between || peek(1).kind == TokenKind::In);
            if (negated) advance();
            if (peek().kind == TokenKind::Between) {
                advance();
                // Bounds stop before AND, which belongs to BETWEEN itself.
                ExprPtr low = expression(kAdd);
                expect(TokenKind::And);
                ExprPtr high = expression(kAdd);
                left = make_expr(Between{std::move(left), std::move(low), std::move(high), negated});
                continue;
            }
            if (peek().kind == TokenKind::In) {
                advance();
                expect(TokenKind::LParen);
                std::vector<ExprPtr> items;
                do {
                    items.push_back(expression(kOr));
                } while (match(TokenKind::Comma));
                expect(TokenKind::RParen);
                left = make_expr(InList{std::move(left), std::move(items), negated});
                continue;
            }
            break;
        }
        return left;
    }

    ExprPtr prefix() {
        const Token& t = peek();
        switch (t.kind) {
            case TokenKind::Not:
                advance();
                return unary(UnaryOp::Not, expression(kNot));
            case TokenKind::Minus:
                advance();
                return unary(UnaryOp::Neg, expression(kNeg));
            default:
                return primary();
        }
    }

    ExprPtr primary() {
        const Token& t = advance();
        switch (t.kind) {
            case TokenKind::Integer: return lit(Value(integer(t)));
            case TokenKind::Decimal: {
                double v = 0;
                std::from_chars(t.text.data(), t.text.data() + t.text.size(), v);
                return lit(Value(v));
            }
            case TokenKind::String: return lit(Value(t.text));
            case TokenKind::True: return lit(Value(true));
            case TokenKind::False: return lit(Value(false));
            case TokenKind::Null: return lit(Value());
            case TokenKind::Identifier: {
                if (match(TokenKind::Dot)) {
                    const Token& name = peek();
                    if (name.kind != TokenKind::Identifier)
                        error_at(name, "expected a column name after '.'");
                    advance();
                    return col(t.text, name.text);
                }
                return col(t.text);
            }
            case TokenKind::LParen: {
                ExprPtr inner = expression(kOr);
                expect(TokenKind::RParen);
                return inner;
            }
            default: error_at(t, "expected an expression");
        }
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

}  // namespace

ExprPtr parse_expression(std::string_view sql) { return Parser(sql).expression_to_end(); }

Statement parse_statement(std::string_view sql) { return Parser(sql).statement_to_end(); }

std::string format_error(std::string_view sql, const ParseError& error) {
    std::size_t start = 0;
    for (int line = 1; line < error.line(); ++line) {
        std::size_t newline = sql.find('\n', start);
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    std::size_t end = sql.find('\n', start);
    std::string text(sql.substr(start, end == std::string_view::npos ? end : end - start));
    // Tabs would push the caret out of line.
    for (char& c : text)
        if (c == '\t') c = ' ';

    std::string out = std::string("error: ") + error.what() + "\n";
    out += "  " + text + "\n";
    out += "  " + std::string(static_cast<std::size_t>(error.column() - 1), ' ') + "^";
    return out;
}

}  // namespace cardinal
