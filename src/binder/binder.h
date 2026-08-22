#pragma once

#include <optional>
#include <string>
#include <vector>

#include "binder/bound_expr.h"
#include "binder/scope.h"
#include "catalog/catalog.h"
#include "sql/ast.h"

namespace cardinal {

struct BoundSelectItem {
    BoundExprPtr expr;
    std::string name;  // what the result column is called
};
struct BoundOrderKey {
    BoundExprPtr expr;
    bool descending;
};

// A SELECT with every name resolved. Expressions hold ColumnIds, never names.
// `scope` keeps the id -> table and column table for printing and planning.
struct BoundSelect {
    Scope scope;
    std::vector<BoundSelectItem> items;  // `*` already expanded
    BoundExprPtr where;                  // null when there is no WHERE
    std::vector<BoundOrderKey> order_by;
    std::optional<std::int64_t> limit;
};

// Looks up the table and columns, checks types, and simplifies:
//   x BETWEEN a AND b  ->  x >= a AND x <= b
//   x IN (a, b)        ->  x = a OR x = b
//   NOT (...)          ->  pushed inward until it disappears where it can
// Types: INT and DOUBLE compare and mix in arithmetic as numbers (INT / INT stays
// INT, anything with a DOUBLE is DOUBLE). TEXT and BOOL only compare with their own
// kind. NULL fits anywhere. WHERE must be boolean. Throws DbError.
BoundSelect bind_select(const Select& select, const Catalog& catalog);

// Binds one expression against the columns in `scope`. An empty scope is enough
// for constants, as in INSERT values.
BoundExprPtr bind_expression(const Expr& expr, const Scope& scope);

}  // namespace cardinal
