#pragma once

#include "binder/bound_expr.h"
#include "optimizer/rule.h"

namespace cardinal {

// Tidies AND and OR, using only facts that hold when a value is NULL too:
//   x AND TRUE    ->  x          x OR FALSE   ->  x
//   x AND FALSE   ->  FALSE      x OR TRUE    ->  TRUE
//   x = 5 AND x = 5   ->  x = 5  (and the same for OR)
//   NOT NOT x     ->  x
// A chain like a AND b AND a is looked at as a whole, so the repeat is found wherever it is.
//
// What it must NOT do, because NULL breaks it:
//   x = x          is unknown when x is NULL, so it is not TRUE
//   x AND NOT x    is unknown when x is NULL, so it is not FALSE
//   x OR NOT x     is unknown when x is NULL, so it is not TRUE
//
// One side effect to know about: `x AND FALSE -> FALSE` throws x away, so if x would
// have raised an error (1 / 0 = 1), the error goes with it. Databases are free to do
// this; the answer, when there is one, never changes.
class BooleanCleanup : public Rule {
public:
    std::string name() const override { return "boolean-cleanup"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

BoundExprPtr clean_booleans(const BoundExprPtr& expr);

}  // namespace cardinal
