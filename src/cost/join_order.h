#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "cost/cost_model.h"
#include "optimizer/join_graph.h"
#include "stats/cardinality.h"

namespace cardinal {

struct JoinOrder {
    std::vector<std::size_t> order;
    PlanPtr plan;
    Cost cost;
    std::size_t joins_priced = 0;
};

// The search's work doubles with every table.
constexpr std::size_t kMaxSearchedRelations = 10;

// The cheapest left-deep order, by the Selinger method (see docs/optimizer.md).
std::optional<JoinOrder> choose_join_order(const JoinGraph& graph, CardinalityEstimator& estimator, const CostModel& model);

// A group with too many tables to search is left as written.
PlanPtr order_joins(const PlanPtr& plan, CardinalityEstimator& estimator, const CostModel& model);

}  // namespace cardinal
