#include "cost/join_order.h"

#include <bit>

#include "cost/cost_planner.h"
#include "logical/plan_util.h"

namespace cardinal {

namespace {

constexpr std::uint32_t bit(std::size_t table) { return std::uint32_t{1} << table; }

struct Group {
    PlanPtr plan;
    Cost cost;
    double rows = 0;  // the same whichever way built the group, so worked out once
    std::vector<std::size_t> order;
};

class Search {
public:
    Search(const JoinGraph& graph, CardinalityEstimator& estimator, const CostModel& model)
        : graph_(graph), estimator_(estimator), model_(model), n_(graph.relations.size()) {
        for (std::size_t i = 0; i < n_; ++i) {
            PlanPtr table = relation_with_filters(graph_, i);
            tables_.push_back({table, plan_by_cost(table, estimator_, model_).cost, estimator_.rows(table), {i}});
        }
    }

    JoinOrder full(bool bushy) {
        best_.assign(std::size_t{1} << n_, {});
        has_neighbour_.assign(std::size_t{1} << n_, -1);
        for (std::size_t i = 0; i < n_; ++i) best_[bit(i)] = tables_[i];

        // The parts of a group are smaller numbers, so counting up finishes them first. A tie
        // keeps the way tried first, and the way closest to the written order is tried first.
        const std::uint32_t all = static_cast<std::uint32_t>((std::size_t{1} << n_) - 1);
        for (std::uint32_t group = 1; group <= all; ++group) {
            if (std::popcount(group) < 2) continue;
            if (bushy) {
                for (std::uint32_t left = (0 - group) & group; left != group; left = (left - group) & group)
                    try_join(left, group ^ left);
            } else {
                for (std::size_t t = n_; t-- > 0;)
                    if (group >> t & 1) try_join(group ^ bit(t), bit(t));
            }
        }
        return finish(best_[all], bushy ? JoinSearch::Bushy : JoinSearch::LeftDeep);
    }

    // From each table in turn, keeps adding whichever table makes the cheapest next join.
    JoinOrder greedy() {
        Group winner;
        for (std::size_t start = 0; start < n_; ++start) {
            Group group = tables_[start];
            std::uint32_t joined = bit(start);
            while (group.order.size() < n_) {
                Group step;
                std::size_t added = 0;
                for (std::size_t t = 0; t < n_; ++t) {
                    if ((joined >> t & 1) || cross_join_can_wait(graph_, joined, bit(t))) continue;
                    const Group& table = tables_[t];
                    PlanPtr plan = join_onto(graph_, group.plan, joined, table.plan, bit(t));
                    double rows = estimator_.rows(plan);
                    Cost cost = group.cost + table.cost +
                                join_step_cost(hash_joinable(graph_, joined, bit(t)), group.rows, table.rows, rows, model_);
                    ++priced_;
                    if (step.plan && !(cost < step.cost)) continue;
                    step = {plan, cost, rows, {}};
                    added = t;
                }
                step.order = std::move(group.order);
                step.order.push_back(added);
                group = std::move(step);
                joined |= bit(added);
            }
            if (!winner.plan || group.cost < winner.cost) winner = std::move(group);
        }
        return finish(winner, JoinSearch::Greedy);
    }

private:
    void try_join(std::uint32_t left, std::uint32_t right) {
        const Group& l = best_[left];
        const Group& r = best_[right];
        if (!l.plan || !r.plan || can_wait(left, right)) return;

        // The plan is only built for the first way into a group, to estimate its rows, and
        // for a way that wins.
        Group& into = best_[left | right];
        PlanPtr joined = into.plan ? nullptr : join_onto(graph_, l.plan, left, r.plan, right);
        const double rows = joined ? estimator_.rows(joined) : into.rows;
        Cost cost = l.cost + r.cost + join_step_cost(hash_joinable(graph_, left, right), l.rows, r.rows, rows, model_);
        ++priced_;
        if (into.plan && !(cost < into.cost)) return;

        std::vector<std::size_t> order;
        if (!l.order.empty() && std::popcount(right) == 1) {
            order = l.order;
            order.push_back(static_cast<std::size_t>(std::countr_zero(right)));
        }
        into = {joined ? joined : join_onto(graph_, l.plan, left, r.plan, right), cost, rows, std::move(order)};
    }

    // cross_join_can_wait, with each group's answer to "is there anything else to join" kept.
    bool can_wait(std::uint32_t a, std::uint32_t b) { return !connects(graph_, a, b) && has_neighbour(a) && has_neighbour(b); }

    bool has_neighbour(std::uint32_t group) {
        signed char& known = has_neighbour_[group];
        if (known < 0) {
            known = 0;
            for (std::size_t t = 0; t < n_ && !known; ++t)
                if (!(group >> t & 1) && connects(graph_, group, bit(t))) known = 1;
        }
        return known;
    }

    JoinOrder finish(const Group& group, JoinSearch search) {
        JoinOrder out;
        out.plan = with_constant_filters(graph_, group.plan);
        out.cost = plan_by_cost(out.plan, estimator_, model_).cost;
        out.search = search;
        out.order = group.order;
        out.joins_priced = priced_;
        return out;
    }

    const JoinGraph& graph_;
    CardinalityEstimator& estimator_;
    const CostModel& model_;
    const std::size_t n_;
    std::vector<Group> tables_;
    std::vector<Group> best_;
    std::vector<signed char> has_neighbour_;  // -1 until worked out
    std::size_t priced_ = 0;
};

}  // namespace

JoinOrder choose_join_order(const JoinGraph& graph, CardinalityEstimator& estimator, const CostModel& model, bool bushy) {
    Search search(graph, estimator, model);
    return graph.relations.size() > kMaxSearchedRelations ? search.greedy() : search.full(bushy);
}

PlanPtr order_joins(const PlanPtr& plan, CardinalityEstimator& estimator, const CostModel& model, bool bushy,
                    std::vector<JoinOrderReport>* reports) {
    if (std::optional<JoinGraph> graph = extract_join_graph(plan)) {
        for (GraphRelation& relation : graph->relations)
            relation.plan = order_joins(relation.plan, estimator, model, bushy, reports);
        JoinOrder chosen = choose_join_order(*graph, estimator, model, bushy);
        if (reports) {
            JoinOrderReport report{graph->relations.size(), chosen.search, chosen.joins_priced, chosen.cost, chosen.cost};
            if (chosen.search != JoinSearch::Greedy)
                (bushy ? report.left_deep : report.bushy) = choose_join_order(*graph, estimator, model, !bushy).cost;
            reports->push_back(report);
        }
        return chosen.plan;
    }
    std::vector<PlanPtr> children = children_of(*plan);
    if (children.empty()) return plan;
    bool changed = false;
    for (PlanPtr& child : children) {
        PlanPtr ordered = order_joins(child, estimator, model, bushy, reports);
        changed = changed || ordered != child;
        child = std::move(ordered);
    }
    return changed ? with_children(*plan, std::move(children)) : plan;
}

}  // namespace cardinal
