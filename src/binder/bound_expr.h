#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>

#include "common/value.h"
#include "sql/ast.h"

namespace cardinal {

// One column of one table as used in one query. Selecting from the same table
// twice gives two different ids, which is what keeps self-joins and aliases
// straight through every later rewrite.
struct ColumnId {
    std::uint32_t value;
    bool operator==(const ColumnId&) const = default;
    auto operator<=>(const ColumnId&) const = default;
};

// Bound expressions never change once built, so subtrees can be shared freely.
struct BoundExpr;
using BoundExprPtr = std::shared_ptr<const BoundExpr>;

struct BoundLiteral {
    Value value;
};
struct BoundColumn {
    ColumnId id;
};
// After binding, NOT only survives in front of something that isn't a comparison,
// AND, OR, IS NULL or a boolean literal (a boolean column, say).
struct BoundUnary {
    UnaryOp op;
    BoundExprPtr operand;
};
// BETWEEN and IN are gone: they become AND / OR of comparisons.
struct BoundBinary {
    BinaryOp op;
    BoundExprPtr left, right;
};
struct BoundIsNull {
    BoundExprPtr operand;
    bool negated;
};

struct BoundExpr {
    std::variant<BoundLiteral, BoundColumn, BoundUnary, BoundBinary, BoundIsNull> node;
    // Empty only for a bare NULL, which fits any type.
    std::optional<Type> type;
};

// Every column the expression reads.
std::set<ColumnId> columns_used(const BoundExpr& expr);

// True if the two trees have the same shape, operators, columns and literal values.
// Types are not compared. `Int 1` and `Double 1.0` are different literals.
bool expr_equal(const BoundExpr& a, const BoundExpr& b);

// Bracket form with columns as #id, e.g. (and (>= #2 1) (<= #2 5)).
std::string print(const BoundExpr& expr);

}  // namespace cardinal
