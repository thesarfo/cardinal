#include "cost/cost_planner.h"

#include <algorithm>
#include <unordered_map>

#include "logical/plan_util.h"
#include "optimizer/conjuncts.h"
#include "physical/join_keys.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

struct JoinOptions {
    std::vector<PlanOption> options;  // costed without their inputs
    std::vector<JoinChoice> choices;
};

JoinOptions join_options(const PlanPtr& node, const LogicalJoin& join, CardinalityEstimator& estimator, const CostModel& model) {
    double left = estimator.rows(join.left), right = estimator.rows(join.right), out = estimator.rows(node);
    JoinOptions joins;
    joins.options.push_back({"NestedLoopJoin", model.nested_loop_join(left, right, out)});
    joins.choices.push_back({JoinMethod::NestedLoop, false});

    JoinKeys keys = split_join_condition(join.condition, output_columns(*join.left), output_columns(*join.right));
    if (!keys.keys.empty()) {
        joins.options.push_back({"HashJoin (build right)", model.hash_join(right, left, out)});
        joins.choices.push_back({JoinMethod::Hash, false});
        joins.options.push_back({"HashJoin (build left)", model.hash_join(left, right, out)});
        joins.choices.push_back({JoinMethod::Hash, true});
    } else {
        joins.options[0].note = "no equality between the two inputs, so no hash join";
    }
    return joins;
}

class Pricer {
public:
    Pricer(CardinalityEstimator& estimator, const CostModel& model) : estimator_(estimator), model_(model) {}

    Cost price(const PlanPtr& node) {
        return std::visit(
            Overloaded{
                [&](const LogicalScan& n) {
                    Cost cost = model_.scan(estimator_.rows(node));
                    record(node, {{"SeqScan[" + n.table + "]", cost}});
                    return cost;
                },
                [&](const LogicalEmpty&) {
                    record(node, {{"Empty", Cost{}}});
                    return Cost{};
                },
                [&](const LogicalFilter& n) {
                    Cost cost = price(n.input) + model_.filter(estimator_.rows(n.input), static_cast<int>(split_ands(n.predicate).size()));
                    record(node, {{"Filter", cost}});
                    return cost;
                },
                [&](const LogicalProject& n) {
                    Cost cost = price(n.input) + model_.project(estimator_.rows(n.input));
                    record(node, {{"Project", cost}});
                    return cost;
                },
                [&](const LogicalPrune& n) {
                    Cost cost = price(n.input) + model_.project(estimator_.rows(n.input));
                    record(node, {{"Prune", cost}});
                    return cost;
                },
                [&](const LogicalSort& n) {
                    Cost cost = price(n.input) + model_.sort(estimator_.rows(n.input));
                    record(node, {{"Sort", cost}});
                    return cost;
                },
                [&](const LogicalLimit& n) {
                    Cost cost = price(n.input);  // see docs/cost-model.md: no discount without a startup cost
                    record(node, {{"Limit", cost}});
                    return cost;
                },
                [&](const LogicalJoin& n) { return price_join(node, n); },
            },
            node->node);
    }

    std::unordered_map<const LogicalJoin*, JoinChoice> choices;
    PlanTrace trace;

private:
    void record(const PlanPtr& node, std::vector<PlanOption> options) {
        if (options.size() == 1) options[0].chosen = true;
        trace.push_back({node.get(), std::move(options)});
    }

    Cost price_join(const PlanPtr& node, const LogicalJoin& join) {
        Cost inputs = price(join.left) + price(join.right);
        JoinOptions joins = join_options(node, join, estimator_, model_);
        std::vector<PlanOption>& options = joins.options;
        for (PlanOption& option : options) option.cost = inputs + option.cost;

        std::size_t best = 0;
        for (std::size_t i = 1; i < options.size(); ++i)
            if (options[i].cost < options[best].cost) best = i;
        options[best].chosen = true;
        choices[&join] = joins.choices[best];
        Cost cost = options[best].cost;
        trace.push_back({node.get(), std::move(options)});
        return cost;
    }

    CardinalityEstimator& estimator_;
    const CostModel& model_;
};

}  // namespace

Cost join_step_cost(const PlanPtr& join, CardinalityEstimator& estimator, const CostModel& model) {
    JoinOptions joins = join_options(join, std::get<LogicalJoin>(join->node), estimator, model);
    Cost best = joins.options[0].cost;
    for (const PlanOption& option : joins.options) best = std::min(best, option.cost);
    return best;
}

CostBasedPlan plan_by_cost(const PlanPtr& logical, CardinalityEstimator& estimator, const CostModel& model) {
    Pricer pricer(estimator, model);
    Cost total = pricer.price(logical);

    PlannerOptions options;
    options.choose_join = [&](const LogicalJoin& join) {
        auto it = pricer.choices.find(&join);
        return it == pricer.choices.end() ? JoinChoice{} : it->second;
    };
    return {plan_physical(*logical, options), total, std::move(pricer.trace)};
}

}  // namespace cardinal
