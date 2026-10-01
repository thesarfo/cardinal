#pragma once

#include <vector>

#include "logical/plan.h"

namespace cardinal {

// The steps a node reads from: none for a Scan or Empty, two for a Join, else one.
std::vector<PlanPtr> children_of(const LogicalPlan& node);

// The first of those, or null. Handy where a step is known to have a single input.
PlanPtr input_of(const LogicalPlan& node);

// A copy of `node` reading from `children` instead, same number as children_of gave.
// Everything else is shared.
PlanPtr with_children(const LogicalPlan& node, std::vector<PlanPtr> children);

// The column ids a node's rows hold, in order. A Project's rows hold computed values,
// not columns, so it gives none. A Join gives the left's columns then the right's.
std::vector<ColumnId> output_columns(const LogicalPlan& node);

}  // namespace cardinal
