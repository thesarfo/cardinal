#pragma once

#include "physical/plan.h"

namespace cardinal {

// Turns each logical step into the one physical step that matches it (Scan becomes
// SeqScan) and swaps every ColumnId for a row position. A scan's rows come out in
// table order, and Filter, Sort and Limit pass that layout up unchanged.
//
// Throws std::logic_error if an expression uses a column its input doesn't produce,
// which would be a bug in the binder or a rewrite.
//
// `options` says how to run joins. This is the simple version; the planner that chooses by
// cost comes later (task 6.3).
enum class JoinMethod { NestedLoop, Hash };

struct PlannerOptions {
    JoinMethod join_method = JoinMethod::NestedLoop;
    // For hash joins: which input to put in the table. A join with no equality between its
    // two sides cannot be hashed and stays a nested loop whatever this says.
    bool build_left = false;
};

PhysicalPtr plan_physical(const LogicalPlan& plan, const PlannerOptions& options = {});

}  // namespace cardinal
