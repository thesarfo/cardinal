#pragma once

#include <string>
#include <vector>

#include "cost/cost_model.h"
#include "physical/physical_planner.h"
#include "stats/cardinality.h"

namespace cardinal {

// One way to run a step, and what the whole plan below it would cost if that way were used.
struct PlanOption {
    PlanOption(std::string option_name, Cost option_cost) : name(std::move(option_name)), cost(option_cost) {}

    std::string name;   // "SeqScan", "NestedLoopJoin", "HashJoin (build left)", ...
    Cost cost;          // this step's own cost plus its inputs' (the cheapest ones)
    bool chosen = false;
    std::string note;   // why there is no other option, say
};

// Every option considered for one step of the logical plan.
struct StepOptions {
    const LogicalPlan* step;
    std::vector<PlanOption> options;
};

// What the planner looked at, inputs before the steps that read them. The winner of each step
// is marked. Losers are kept so EXPLAIN VERBOSE can show them.
using PlanTrace = std::vector<StepOptions>;

struct CostBasedPlan {
    PhysicalPtr physical;
    Cost cost;  // of the whole plan
    PlanTrace trace;
};

// Chooses how to run each step by pricing the choices with the cost model and the estimator's
// row counts. Only joins have a choice so far: a nested loop join, or a hash join building
// from either input, which is possible only when the condition has an equality between the two
// inputs. A tie goes to the earlier of nested loop, hash (build right), hash (build left).
//
// The inputs of a step are priced on their own, which is enough because a step's cost depends
// on the cost of its inputs only through their total, and the row counts do not depend on the
// choice made.
CostBasedPlan plan_by_cost(const PlanPtr& logical, CardinalityEstimator& estimator, const CostModel& model);

}  // namespace cardinal
