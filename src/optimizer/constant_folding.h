#pragma once

#include "binder/bound_expr.h"
#include "optimizer/rule.h"

namespace cardinal {

// Works out an expression whose inputs are all literals ahead of time:
//   age > 20 + 5     ->   age > 25
//   NULL + 1         ->   NULL
//   NULL AND FALSE   ->   FALSE       (false whatever the NULL turns out to be)
// It uses the same evaluator as the executor, so folding can't disagree with running.
//
// Anything the evaluator rejects is left exactly as written, so the error shows up
// when the query runs, and only if that row of the plan is ever reached:
//   1 / 0, 9223372036854775807 + 1
// Parts of such an expression that are fine still fold: 1 / 0 + (2 + 3) -> 1 / 0 + 5.
class ConstantFolding : public Rule {
public:
    std::string name() const override { return "constant-folding"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

// The folding itself, for other rules to reuse. Returns `expr` itself if nothing folded.
BoundExprPtr fold_constants(const BoundExprPtr& expr);

}  // namespace cardinal
