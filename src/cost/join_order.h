#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "cost/cost_model.h"
#include "optimizer/join_graph.h"
#include "stats/cardinality.h"

namespace cardinal {

struct JoinOrder {
    std::vector<std::size_t> order;  // the tables, first joined first
    PlanPtr plan;                    // build_join_tree(graph, order), the same steps
    Cost cost;                       // of `plan`, as plan_by_cost prices it
    std::size_t joins_priced = 0;    // how many ways of adding one table to a group were tried
};

// The most tables the search takes on. Its work doubles with every table.
constexpr std::size_t kMaxSearchedRelations = 10;

// The cheapest left-deep order for the tables in `graph`, found the Selinger way (see
// docs/optimizer.md). Groups of tables are bitmasks. Each group keeps its cheapest plan, and
// a bigger group is built by adding one table to a smaller group's best plan. A cross join
// is only tried when nothing else joins to the group (cross_join_can_wait).
//
// Gives nothing for more than kMaxSearchedRelations tables.
std::optional<JoinOrder> choose_join_order(const JoinGraph& graph, CardinalityEstimator& estimator, const CostModel& model);

}  // namespace cardinal
