#include "cost/cost_planner.h"

#include <algorithm>
#include <unordered_map>

#include "logical/plan_util.h"
#include "optimizer/conjuncts.h"
#include "physical/index_match.h"
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

JoinOptions join_options(bool hashable, double left, double right, double out, const CostModel& model) {
    JoinOptions joins;
    joins.options.push_back({"NestedLoopJoin", model.nested_loop_join(left, right, out)});
    joins.choices.push_back({JoinMethod::NestedLoop, false});

    if (hashable) {
        joins.options.push_back({"HashJoin (build right)", model.hash_join(right, left, out)});
        joins.choices.push_back({JoinMethod::Hash, false});
        joins.options.push_back({"HashJoin (build left)", model.hash_join(left, right, out)});
        joins.choices.push_back({JoinMethod::Hash, true});
        // Neither input is known to arrive in key order yet (task 10.2), so both are sorted.
        joins.options.push_back({"MergeJoin (sorting both)", model.sort(left) + model.sort(right) + model.merge_join(left, right, out)});
        joins.choices.push_back({JoinMethod::Merge, false});
    } else {
        joins.options[0].note = "no equality between the two inputs, so no hash join";
    }
    return joins;
}

class Pricer {
public:
    Pricer(CardinalityEstimator& estimator, const CostModel& model, const Catalog* indexes)
        : estimator_(estimator), model_(model), indexes_(indexes) {}

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
                [&](const LogicalFilter& n) { return price_filter(node, n); },
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
                [&](const LogicalAggregate& n) { return price_aggregate(node, n); },
                [&](const LogicalJoin& n) { return price_join(node, n); },
            },
            node->node);
    }

    std::unordered_map<const LogicalJoin*, JoinChoice> choices;
    std::unordered_map<const LogicalAggregate*, AggregateMethod> aggregate_choices;
    std::unordered_map<const LogicalFilter*, bool> index_choices;
    PlanTrace trace;

private:
    void record(const PlanPtr& node, std::vector<PlanOption> options) {
        if (options.size() == 1) options[0].chosen = true;
        trace.push_back({node.get(), std::move(options)});
    }

    // A filter, and when it sits directly on a scan with a usable index, also the same step read
    // through the index. Both are costed as the whole step including the scan.
    Cost price_filter(const PlanPtr& node, const LogicalFilter& filter) {
        Cost input = price(filter.input);
        double in = estimator_.rows(filter.input);
        Cost scanned = input + model_.filter(in, static_cast<int>(split_ands(filter.predicate).size()));
        std::optional<IndexMatch> match;
        if (indexes_) match = match_index(filter, *indexes_);
        if (!match) {
            record(node, {{"Filter", scanned}});
            return scanned;
        }

        // How many rows the index finds: what the answered conditions alone let through.
        PlanPtr answered = std::make_shared<const LogicalPlan>(LogicalPlan{LogicalFilter{filter.input, and_all(match->answered)}});
        double found = estimator_.rows(answered);
        Cost through = model_.index_scan(in, found);
        if (!match->rest.empty()) through += model_.filter(found, static_cast<int>(match->rest.size()));

        std::vector<PlanOption> options;
        options.push_back({"Filter", scanned});
        options.push_back({"IndexScan[" + match->index + "]" + (match->rest.empty() ? "" : " + Filter"), through});
        std::size_t best = options[1].cost < options[0].cost ? 1 : 0;
        options[best].chosen = true;
        index_choices[&filter] = best == 1;
        if (best == 1) {
            // The scan below is not run: the index scan reads the table instead.
            for (StepOptions& step : trace)
                if (step.step == filter.input.get())
                    for (PlanOption& o : step.options) {
                        o.chosen = false;
                        o.note = "the index scan above reads the table instead";
                    }
        }
        Cost cost = options[best].cost;
        trace.push_back({node.get(), std::move(options)});
        return cost;
    }

    Cost price_aggregate(const PlanPtr& node, const LogicalAggregate& aggregate) {
        Cost input = price(aggregate.input);
        double in = estimator_.rows(aggregate.input), groups = estimator_.rows(node);
        std::vector<PlanOption> options;
        options.push_back({"HashAggregate", input + model_.hash_aggregate(in, groups)});
        options.push_back({"SortAggregate", input + model_.sort_aggregate(in, groups)});
        std::size_t best = options[1].cost < options[0].cost ? 1 : 0;
        options[best].chosen = true;
        aggregate_choices[&aggregate] = best == 0 ? AggregateMethod::Hash : AggregateMethod::Sort;
        Cost cost = options[best].cost;
        trace.push_back({node.get(), std::move(options)});
        return cost;
    }

    Cost price_join(const PlanPtr& node, const LogicalJoin& join) {
        Cost inputs = price(join.left) + price(join.right);
        bool hashable = !split_join_condition(join.condition, output_columns(*join.left), output_columns(*join.right)).keys.empty();
        JoinOptions joins = join_options(hashable, estimator_.rows(join.left), estimator_.rows(join.right), estimator_.rows(node), model_);
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
    const Catalog* indexes_;
};

}  // namespace

Cost join_step_cost(bool hashable, double left_rows, double right_rows, double out_rows, const CostModel& model) {
    JoinOptions joins = join_options(hashable, left_rows, right_rows, out_rows, model);
    Cost best = joins.options[0].cost;
    for (const PlanOption& option : joins.options) best = std::min(best, option.cost);
    return best;
}

CostBasedPlan plan_by_cost(const PlanPtr& logical, CardinalityEstimator& estimator, const CostModel& model,
                           const Catalog* indexes) {
    Pricer pricer(estimator, model, indexes);
    Cost total = pricer.price(logical);

    PlannerOptions options;
    options.catalog = indexes;
    options.use_indexes = indexes != nullptr;
    options.choose_index = [&](const LogicalFilter& filter) {
        auto it = pricer.index_choices.find(&filter);
        return it != pricer.index_choices.end() && it->second;
    };
    options.choose_join = [&](const LogicalJoin& join) {
        auto it = pricer.choices.find(&join);
        return it == pricer.choices.end() ? JoinChoice{} : it->second;
    };
    options.choose_aggregate = [&](const LogicalAggregate& aggregate) {
        auto it = pricer.aggregate_choices.find(&aggregate);
        return it == pricer.aggregate_choices.end() ? AggregateMethod::Hash : it->second;
    };
    return {plan_physical(*logical, options), total, std::move(pricer.trace)};
}

}  // namespace cardinal
