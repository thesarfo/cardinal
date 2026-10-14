#pragma once

#include <vector>

#include "physical/plan.h"

namespace cardinal {

// The order a step's rows are known to come out in: ascending or descending by these
// expressions, each over the step's own output row, most significant first. Empty means no
// order is promised. Only what is certain is reported, so a step with no known order can
// still happen to be in order.
//
//   IndexScan      by the indexed column, ascending
//   Sort           by its keys
//   SortAggregate  by its group keys, which are the first columns it produces
//   Filter, Limit, NestedLoopJoin, MergeJoin, and a HashJoin that probes with its left input
//                  keep the order of the input whose rows come out in order
//   Project        keeps the leading keys that are still one of its output columns
std::vector<SortKey> sorted_by(const PhysicalPlan& plan);

// Whether rows ordered by `have` are also ordered by `want`: `want` must be a leading part of
// `have`, key for key, in the same directions.
bool satisfies(const std::vector<SortKey>& have, const std::vector<SortKey>& want);

// "#0, #2 DESC", or an empty string for no order.
std::string describe_order(const std::vector<SortKey>& order);

}  // namespace cardinal
