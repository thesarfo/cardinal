#pragma once

#include <string>

#include "binder/scope.h"
#include "logical/plan.h"
#include "optimizer/rule_optimizer.h"

namespace cardinal {

// What EXPLAIN prints: the plan as first written, the rules that fired in order, and
// the plan after them. No trailing newline.
//   Original plan
//     Project[name]
//       Filter[age > 20 + 5 AND TRUE]
//         Scan[users]
//
//   Rules fired
//     1. constant-folding (pass 1)
//     2. boolean-cleanup (pass 1)
//
//   Final plan
//     Project[name]
//       Filter[age > 25]
//         Scan[users]
std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope);

}  // namespace cardinal
