#include "cost/join_order.h"

#include "cost/cost_planner.h"
#include "logical/plan_util.h"

namespace cardinal {

namespace {

struct Group {
    PlanPtr plan;
    Cost cost;
    std::vector<std::size_t> order;
    double rows = 0;  // the same whichever order built the group, so worked out once
};

}  // namespace

std::optional<JoinOrder> choose_join_order(const JoinGraph& graph, CardinalityEstimator& estimator, const CostModel& model) {
    const std::size_t n = graph.relations.size();
    if (n > kMaxSearchedRelations) return std::nullopt;

    std::vector<PlanPtr> tables(n);
    std::vector<Cost> table_cost(n);
    std::vector<Group> best(std::size_t{1} << n);
    for (std::size_t i = 0; i < n; ++i) {
        tables[i] = relation_with_filters(graph, i);
        table_cost[i] = plan_by_cost(tables[i], estimator, model).cost;
        best[std::size_t{1} << i] = {tables[i], table_cost[i], {i}, estimator.rows(tables[i])};
    }

    // A group minus one table is a smaller number, so counting up finishes a group before
    // anything is built from it.
    JoinOrder chosen;
    const std::uint32_t all = static_cast<std::uint32_t>((std::size_t{1} << n) - 1);
    for (std::uint32_t group = 1; group < all; ++group) {
        const Group& from = best[group];
        if (!from.plan) continue;
        // Tables something joins to the group. If there are none, any table may come next.
        std::uint32_t next = 0;
        for (std::size_t t = 0; t < n; ++t)
            if (!(group >> t & 1) && connects(graph, group, t)) next |= std::uint32_t{1} << t;
        if (next == 0) next = all & ~group;

        for (std::size_t t = 0; t < n; ++t) {
            if (!(next >> t & 1)) continue;
            // The plan is only built for the first way into a group, to estimate its rows, and
            // for a way that wins.
            Group& into = best[group | std::uint32_t{1} << t];
            PlanPtr joined = into.plan ? nullptr : join_onto(graph, from.plan, group, t, tables[t]);
            const double rows = joined ? estimator.rows(joined) : into.rows;
            const double right = best[std::size_t{1} << t].rows;
            Cost cost = from.cost + table_cost[t] + join_step_cost(hash_joinable(graph, group, t), from.rows, right, rows, model);
            ++chosen.joins_priced;

            if (into.plan && !(cost < into.cost)) continue;
            into.plan = joined ? joined : join_onto(graph, from.plan, group, t, tables[t]);
            into.cost = cost;
            into.rows = rows;
            into.order = from.order;
            into.order.push_back(t);
        }
    }

    chosen.order = best[all].order;
    chosen.plan = with_constant_filters(graph, best[all].plan);
    chosen.cost = plan_by_cost(chosen.plan, estimator, model).cost;
    return chosen;
}

PlanPtr order_joins(const PlanPtr& plan, CardinalityEstimator& estimator, const CostModel& model) {
    if (std::optional<JoinGraph> graph = extract_join_graph(plan)) {
        for (GraphRelation& relation : graph->relations) relation.plan = order_joins(relation.plan, estimator, model);
        if (std::optional<JoinOrder> chosen = choose_join_order(*graph, estimator, model)) return chosen->plan;
        return build_join_tree(*graph);
    }
    std::vector<PlanPtr> children = children_of(*plan);
    if (children.empty()) return plan;
    bool changed = false;
    for (PlanPtr& child : children) {
        PlanPtr ordered = order_joins(child, estimator, model);
        changed = changed || ordered != child;
        child = std::move(ordered);
    }
    return changed ? with_children(*plan, std::move(children)) : plan;
}

}  // namespace cardinal
