#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/value.h"

namespace cardinal {

// Names only, no binding yet: the binder (task 2.2) resolves them.

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class UnaryOp { Not, Neg };
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge, And, Or };

struct Literal {
    Value value;
};
struct ColumnRef {
    std::optional<std::string> table;
    std::string name;
};
struct Unary {
    UnaryOp op;
    ExprPtr operand;
};
struct Binary {
    BinaryOp op;
    ExprPtr left, right;
};
struct IsNull {
    ExprPtr operand;
    bool negated;
};
struct Between {
    ExprPtr operand, low, high;
    bool negated;
};
struct InList {
    ExprPtr operand;
    std::vector<ExprPtr> items;
    bool negated;
};

struct Expr {
    std::variant<Literal, ColumnRef, Unary, Binary, IsNull, Between, InList> node;
};

// Builders, so tests and the parser don't spell out the nesting.
inline ExprPtr make_expr(auto node) {
    return std::make_unique<Expr>(Expr{std::move(node)});
}
inline ExprPtr lit(Value v) { return make_expr(Literal{std::move(v)}); }
inline ExprPtr col(std::string name) { return make_expr(ColumnRef{std::nullopt, std::move(name)}); }
inline ExprPtr col(std::string table, std::string name) {
    return make_expr(ColumnRef{std::move(table), std::move(name)});
}
inline ExprPtr unary(UnaryOp op, ExprPtr operand) {
    return make_expr(Unary{op, std::move(operand)});
}
inline ExprPtr binary(BinaryOp op, ExprPtr left, ExprPtr right) {
    return make_expr(Binary{op, std::move(left), std::move(right)});
}

struct ColumnDef {
    std::string name;
    Type type;
};
struct CreateTable {
    std::string name;
    std::vector<ColumnDef> columns;
};
struct Insert {
    std::string table;
    std::vector<std::vector<ExprPtr>> rows;
};

// `SELECT *` is a select list with one Star.
struct Star {};
struct SelectItem {
    std::variant<Star, ExprPtr> item;
};
struct OrderKey {
    ExprPtr expr;
    bool descending = false;
};
// A table in FROM, with the name it goes by in the query (its own name if no alias).
struct TableRef {
    std::string table;
    std::optional<std::string> alias;
};
// `JOIN b ON cond` has a condition; `, b` does not.
struct JoinClause {
    TableRef table;
    ExprPtr on;  // null for a comma join
};
struct Select {
    std::vector<SelectItem> items;
    // FROM a JOIN b ON ..., c  is read left to right: ((a JOIN b) , c).
    TableRef from;
    std::vector<JoinClause> joins;
    ExprPtr where;  // null when there is no WHERE
    std::vector<OrderKey> order_by;
    std::optional<std::int64_t> limit;
};

// ANALYZE users: gather statistics about the table's columns.
struct Analyze {
    std::string table;
};

struct Statement;
struct Explain {
    std::unique_ptr<Statement> inner;
};

struct Statement {
    std::variant<CreateTable, Insert, Select, Explain, Analyze> node;
};

ExprPtr clone(const Expr& expr);
Select clone(const Select& select);

}  // namespace cardinal
