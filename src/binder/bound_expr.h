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

// The totals SQL can compute over a group of rows. Count skips NULLs, CountStar counts rows.
// Sum, Avg, Min and Max give NULL for a group with no non-NULL value.
enum class AggFunc { CountStar, Count, Sum, Avg, Min, Max };

// "count", "sum", ...; CountStar is also "count".
const char* agg_name(AggFunc func);

// One group key of an Aggregate: the expression that is computed per input row, and the
// column id its value goes by in the rows above. A key that is a plain column keeps its
// own id, so what reads it above the Aggregate needs no rewriting.
struct GroupKey {
    BoundExprPtr expr;
    ColumnId output;
};

// One total: `arg` is null for CountStar. `output` is a fresh column id for the result.
struct AggregateItem {
    AggFunc func;
    BoundExprPtr arg;
    ColumnId output;
};

// Every column the expression reads.
std::set<ColumnId> columns_used(const BoundExpr& expr);

// True if the two trees have the same shape, operators, columns and literal values.
// Types are not compared. `Int 1` and `Double 1.0` are different literals.
bool expr_equal(const BoundExpr& a, const BoundExpr& b);

// Bracket form with columns as #id, e.g. (and (>= #2 1) (<= #2 5)).
std::string print(const BoundExpr& expr);

}  // namespace cardinal
