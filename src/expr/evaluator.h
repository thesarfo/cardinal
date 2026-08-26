#pragma once

#include "binder/bound_expr.h"
#include "common/value.h"

namespace cardinal {

// Works out an expression's value for one row. A column's `id.value` is read as its
// position in `row`. The binder hands out ids 0, 1, 2... per table, so for a single
// table those are already positions; the physical planner remaps them for plans
// with more than one table.
//
// SQL logic: comparisons and arithmetic with a NULL give NULL ("unknown"). AND is
// false if either side is false, even when the other is NULL; OR is true if either
// side is true. Both sides are always evaluated, so an error never depends on order.
//
// Throws DbError on division by zero or integer overflow. Numbers of different
// types compare as numbers; INT op INT stays INT (and `/` truncates).
Value evaluate(const BoundExpr& expr, const Row& row);

// True only for the value `true`. NULL and false both drop a row from a WHERE.
inline bool is_true(const Value& v) { return !v.is_null() && v.as_bool(); }

}  // namespace cardinal
