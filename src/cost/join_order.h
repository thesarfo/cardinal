#pragma once

#include <cstddef>
#include <vector>

#include "cost/cost_model.h"
#include "optimizer/join_graph.h"
#include "stats/cardinality.h"

namespace cardinal {

enum class JoinSearch {
    LeftDeep,  // every join adds one table
    Bushy,     // a join may join two joined groups
    Greedy,    // left-deep, one cheapest step at a time, for too many tables to search
};

struct JoinOrder {
    PlanPtr plan;
    Cost cost;
    JoinSearch search;
    std::vector<std::size_t> order;  // empty for a bushy plan
    std::size_t joins_priced = 0;
};

// Above this many tables the greedy search is used. The full search's work doubles with every
// table, and triples for bushy.
constexpr std::size_t kMaxSearchedRelations = 10;

// The cheapest order, by the Selinger method (see docs/optimizer.md).
JoinOrder choose_join_order(const JoinGraph& graph, CardinalityEstimator& estimator, const CostModel& model,
                            bool bushy = false);

// One group of joins that order_joins ordered, priced both ways for EXPLAIN VERBOSE.
struct JoinOrderReport {
    std::size_t tables;
    JoinSearch used;
    std::size_t joins_priced;
    Cost left_deep;  // both are the greedy cost when the greedy search was used
    Cost bushy;
};

// Every group of joins in `plan` in its cheapest order. With `reports`, each group is also
// searched the other way, for its cost.
PlanPtr order_joins(const PlanPtr& plan, CardinalityEstimator& estimator, const CostModel& model, bool bushy = false,
                    std::vector<JoinOrderReport>* reports = nullptr);

}  // namespace cardinal
