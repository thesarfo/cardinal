#pragma once

#include "physical/plan.h"

namespace cardinal {

// Turns each logical step into the one physical step that matches it (Scan becomes
// SeqScan) and swaps every ColumnId for a row position. A scan's rows come out in
// table order, and Filter, Sort and Limit pass that layout up unchanged.
//
// Throws std::logic_error if an expression uses a column its input doesn't produce,
// which would be a bug in the binder or a rewrite.
PhysicalPtr plan_physical(const LogicalPlan& plan);

}  // namespace cardinal
