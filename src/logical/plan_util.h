#pragma once

#include "logical/plan.h"

namespace cardinal {

// The step a node reads from, or null for a Scan.
PlanPtr input_of(const LogicalPlan& node);

// A copy of `node` that reads from `input` instead. Everything else is shared.
// `node` must not be a Scan.
PlanPtr with_input(const LogicalPlan& node, PlanPtr input);

}  // namespace cardinal
