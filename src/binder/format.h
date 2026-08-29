#pragma once

#include <string>

#include "binder/bound_expr.h"
#include "binder/scope.h"

namespace cardinal {

// An expression as SQL-ish text for plans: `score > 2.5 AND (id = 1 OR active)`.
// Columns show their name, or alias.name when two tables have a column of that
// name. Nested operators are bracketed, so there is no precedence to remember.
std::string format(const BoundExpr& expr, const Scope& scope);

}  // namespace cardinal
