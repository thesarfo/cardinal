#include "cost/cost_planner.h"

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
        double left = estimator_.rows(join.left), right = estimator_.rows(join.right), out = estimator_.rows(node);

        std::vector<PlanOption> options;
        std::vector<JoinChoice> choice_of;
        options.push_back({"NestedLoopJoin", inputs + model_.nested_loop_join(left, right, out)});
        choice_of.push_back({JoinMethod::NestedLoop, false});

        JoinKeys keys = split_join_condition(join.condition, output_columns(*join.left), output_columns(*join.right));
        if (!keys.keys.empty()) {
            options.push_back({"HashJoin (build right)", inputs + model_.hash_join(right, left, out)});
            choice_of.push_back({JoinMethod::Hash, false});
            options.push_back({"HashJoin (build left)", inputs + model_.hash_join(left, right, out)});
            choice_of.push_back({JoinMethod::Hash, true});
        } else {
            options[0].note = "no equality between the two inputs, so no hash join";
        }

        std::size_t best = 0;
        for (std::size_t i = 1; i < options.size(); ++i)
            if (options[i].cost < options[best].cost) best = i;
        options[best].chosen = true;
        choices[&join] = choice_of[best];
        Cost cost = options[best].cost;
        trace.push_back({node.get(), std::move(options)});
        return cost;
    }

    CardinalityEstimator& estimator_;
    const CostModel& model_;
};

}  // namespace

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
