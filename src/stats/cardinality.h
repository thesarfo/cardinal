#pragma once

#include <unordered_map>

#include "binder/scope.h"
#include "catalog/catalog.h"
#include "logical/plan.h"
#include "stats/estimator.h"

namespace cardinal {

// Guesses how many rows each step of a plan produces, and remembers the answers.
//
//   Scan      the table's row count right now
//   Empty     0
//   Filter    rows in x the condition's selectivity
//   Join      rows in the left x rows in the right x the condition's selectivity.
//             For an equality that is left x right / the larger distinct count.
//             A cross join has no condition, so it is left x right.
//   Project, Sort, Prune   as many as come in
//   Limit n   the smaller of n and what comes in
//
// The scan uses the actual row count, not the count when ANALYZE ran, so a table that has
// grown still gets the right size even if its fractions are a little old.
class CardinalityEstimator {
public:
    CardinalityEstimator(const Scope& scope, const Catalog& catalog)
        : catalog_(catalog), stats_(StatsLookup::for_scope(scope, catalog)) {}

    // Safe to call on any step of the plan, in any order. Each step is worked out once.
    double rows(const PlanPtr& plan);

    // The same, for a step that is not held by a shared pointer (the printer hands those out).
    double rows_of(const LogicalPlan& plan);

    // True if rows() has already worked this step out.
    bool has(const PlanPtr& plan) const { return cache_.count(plan.get()) > 0; }

private:
    double compute(const LogicalPlan& plan);

    const Catalog& catalog_;
    StatsLookup stats_;
    std::unordered_map<const LogicalPlan*, double> cache_;
};

}  // namespace cardinal
