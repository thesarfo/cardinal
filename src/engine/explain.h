#pragma once

#include <string>

#include "binder/scope.h"
#include "catalog/catalog.h"
#include "logical/plan.h"
#include "optimizer/rule_optimizer.h"

namespace cardinal {

// What EXPLAIN prints: the plan as first written, the rules that fired in order, and
// the plan after them. No trailing newline.
//   Original plan
//     Project[name]  est_rows=250
//       Filter[age > 20 + 5 AND TRUE]  est_rows=250
//         Scan[users]  est_rows=1000
//
//   Rules fired
//     1. constant-folding (pass 1)
//     2. boolean-cleanup (pass 1)
//
//   Final plan
//     Project[name]  est_rows=250
//       Filter[age > 25]  est_rows=250
//         Scan[users]  est_rows=1000
//
//   Statistics
//     users: analyzed when it had 1000 rows
//
// Every line carries the optimizer's guess at how many rows the step produces
// (est_rows). The Statistics lines say which tables the guesses rest on, and warn about
// tables that have changed since ANALYZE. No trailing newline.
std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                           const Catalog& catalog);

}  // namespace cardinal
