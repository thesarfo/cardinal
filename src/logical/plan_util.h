#pragma once

#include <vector>

#include "logical/plan.h"

namespace cardinal {

// The step a node reads from, or null for a Scan.
PlanPtr input_of(const LogicalPlan& node);

// The column ids a node's rows hold, in order. A Project's rows hold computed values,
// not columns, so it gives none.
std::vector<ColumnId> output_columns(const LogicalPlan& node);

// A copy of `node` that reads from `input` instead. Everything else is shared.
// `node` must not be a Scan.
PlanPtr with_input(const LogicalPlan& node, PlanPtr input);

}  // namespace cardinal
