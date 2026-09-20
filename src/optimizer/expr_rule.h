#pragma once

#include <functional>
#include <optional>

#include "binder/bound_expr.h"
#include "logical/plan.h"

namespace cardinal {

using ExprRewrite = std::function<BoundExprPtr(const BoundExprPtr&)>;

// For rules that rewrite expressions. Applies `rewrite` to every expression held by
// this one node (a Filter's predicate, a Project's items, a Sort's keys) but not to
// its inputs, which the optimizer visits on its own. Returns the new node, or nothing
// if `rewrite` handed back the same expression every time.
std::optional<PlanPtr> rewrite_expressions(const PlanPtr& node, const ExprRewrite& rewrite);

}  // namespace cardinal
