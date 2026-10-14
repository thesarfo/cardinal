#include "cost/cost_planner.h"

#include <algorithm>
#include <map>
#include <optional>
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
        // Sorting both inputs. The pricer replaces this with a cheaper variant when an input can
        // arrive in key order from an index (see price_join).
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
        Cost cost = price_step(node);
        best_[node.get()] = cost;
        return cost;
    }

    // Prices the whole plan, then settles which way each step with a choice is run: a step the
    // parent needs in key order is run the way that gives the order, whatever it costs alone.
    Cost price_all(const PlanPtr& root) {
        Cost cost = price(root);
        settle(root, std::nullopt);
        return cost;
    }

private:
    Cost price_step(const PlanPtr& node) {
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
                    Cost step = model_.project(estimator_.rows(n.input));
                    Cost cost = price(n.input) + step;
                    record(node, {{"Prune", cost}});
                    // Keeping columns keeps their order.
                    for (const auto& [column, ordered] : ordered_[n.input.get()])
                        if (std::find(n.columns.begin(), n.columns.end(), ColumnId{column}) != n.columns.end())
                            ordered_[node.get()][column] = ordered + step;
                    return cost;
                },
                [&](const LogicalSort& n) { return price_sort(node, n); },
                [&](const LogicalLimit& n) {
                    Cost cost = price(n.input);  // see docs/cost-model.md: no discount without a startup cost
                    record(node, {{"Limit", cost}});
                    ordered_[node.get()] = ordered_[n.input.get()];
                    return cost;
                },
                [&](const LogicalAggregate& n) { return price_aggregate(node, n); },
                [&](const LogicalJoin& n) { return price_join(node, n); },
            },
            node->node);
    }

public:
    std::unordered_map<const LogicalJoin*, JoinChoice> choices;
    std::unordered_map<const LogicalAggregate*, AggregateMethod> aggregate_choices;
    std::unordered_map<const LogicalFilter*, bool> index_choices;
    PlanTrace trace;

private:
    // What the pricer remembers about a step besides its cheapest cost: for each column the
    // step's rows can arrive sorted by (ascending), the cheapest cost of a plan that does.
    // Only orders a parent could use are kept: those from an index scan that a filter reads
    // through, from a merge join, and the ones passed up by steps that keep their input's order.
    using Ordered = std::map<std::uint32_t, Cost>;

    struct MergeVariant {
        bool left_ordered = false, right_ordered = false;
        ColumnId left_key{0}, right_key{0};
    };

    // What settle needs to know about a filter on a scan: whether an index fits, and by which column.
    struct IndexFit {
        bool fits = false;
        ColumnId column{0};
    };

    std::unordered_map<const LogicalPlan*, Cost> best_;
    std::unordered_map<const LogicalPlan*, Ordered> ordered_;
    std::unordered_map<const LogicalJoin*, MergeVariant> merge_;
    std::unordered_map<const LogicalFilter*, IndexFit> index_fit_;
    std::unordered_map<const LogicalSort*, bool> sort_skipped_;

    static std::optional<ColumnId> plain_column(const BoundExprPtr& expr) {
        if (const auto* c = std::get_if<BoundColumn>(&expr->node)) return c->id;
        return std::nullopt;
    }

    // Marks the one option of a step whose name starts with `prefix` as chosen, and the rest not.
    void mark_chosen(const LogicalPlan* step, const std::string& prefix) {
        for (StepOptions& s : trace) {
            if (s.step != step) continue;
            for (PlanOption& o : s.options) o.chosen = o.name.rfind(prefix, 0) == 0;
        }
    }

    void settle(const PlanPtr& node, std::optional<ColumnId> want) {
        std::visit(
            Overloaded{
                [&](const LogicalFilter& n) {
                    auto fit = index_fit_.find(&n);
                    if (fit != index_fit_.end() && fit->second.fits) {
                        bool use = want ? true : index_choices[&n];
                        index_choices[&n] = use;
                        if (use) {
                            mark_chosen(node.get(), "IndexScan");
                            for (StepOptions& step : trace)
                                if (step.step == n.input.get())
                                    for (PlanOption& o : step.options) {
                                        o.chosen = false;
                                        o.note = "the index scan above reads the table instead";
                                    }
                        }
                        return;
                    }
                    settle(n.input, want);
                },
                [&](const LogicalPrune& n) { settle(n.input, want); },
                [&](const LogicalLimit& n) { settle(n.input, want); },
                [&](const LogicalSort& n) {
                    std::optional<ColumnId> key = n.keys.size() == 1 ? plain_column(n.keys[0].expr) : std::nullopt;
                    settle(n.input, sort_skipped_[&n] ? key : std::nullopt);
                },
                [&](const LogicalProject& n) { settle(n.input, std::nullopt); },
                [&](const LogicalAggregate& n) { settle(n.input, std::nullopt); },
                [&](const LogicalJoin& n) {
                    auto variant = merge_.find(&n);
                    bool merge = want.has_value() || choices[&n].method == JoinMethod::Merge;
                    if (merge && variant != merge_.end()) {
                        choices[&n] = {JoinMethod::Merge, false};
                        mark_chosen(node.get(), "MergeJoin");
                        settle(n.left, variant->second.left_ordered ? std::optional<ColumnId>(variant->second.left_key) : std::nullopt);
                        settle(n.right, variant->second.right_ordered ? std::optional<ColumnId>(variant->second.right_key) : std::nullopt);
                        return;
                    }
                    settle(n.left, std::nullopt);
                    settle(n.right, std::nullopt);
                },
                [&](const auto&) {},
            },
            node->node);
    }

    // A sort. Free when the input can arrive sorted by the one column it sorts on, and that is
    // cheaper than sorting the cheapest plan for the input.
    Cost price_sort(const PlanPtr& node, const LogicalSort& sort) {
        Cost input = price(sort.input);
        Cost sorted = input + model_.sort(estimator_.rows(sort.input));
        std::optional<ColumnId> key = sort.keys.size() == 1 && !sort.keys[0].descending ? plain_column(sort.keys[0].expr) : std::nullopt;
        std::vector<PlanOption> options{{"Sort", sorted}};
        Cost cost = sorted;
        bool skipped = false;
        if (key) {
            auto have = ordered_[sort.input.get()].find(key->value);
            if (have != ordered_[sort.input.get()].end()) {
                options.push_back({"no sort, the input arrives in order", have->second});
                if (have->second < sorted) {
                    cost = have->second;
                    skipped = true;
                }
            }
            ordered_[node.get()][key->value] = cost;
        }
        sort_skipped_[&sort] = skipped;
        options[skipped ? 1 : 0].chosen = true;
        trace.push_back({node.get(), std::move(options)});
        return cost;
    }

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
            // A filter keeps the order of its rows.
            Cost step = model_.filter(in, static_cast<int>(split_ands(filter.predicate).size()));
            for (const auto& [column, ordered] : ordered_[filter.input.get()]) ordered_[node.get()][column] = ordered + step;
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
        // Read through the index, the rows arrive in the order of the indexed column. That is
        // worth remembering even when a full scan is cheaper alone: a merge join above can
        // make up the difference by not sorting.
        const auto& scan = std::get<LogicalScan>(filter.input->node);
        std::optional<std::size_t> column_at = std::nullopt;
        if (const Table* table = indexes_->get_table(scan.table)) column_at = table->info().find_column(match->column);
        if (column_at) {
            ColumnId column = scan.columns[*column_at];
            ordered_[node.get()][column.value] = through;
            index_fit_[&filter] = {true, column};
        }
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
        Cost left_best = price(join.left), right_best = price(join.right);
        Cost inputs = left_best + right_best;
        JoinKeys split = split_join_condition(join.condition, output_columns(*join.left), output_columns(*join.right));
        bool hashable = !split.keys.empty();
        double left_rows = estimator_.rows(join.left), right_rows = estimator_.rows(join.right), out = estimator_.rows(node);
        JoinOptions joins = join_options(hashable, left_rows, right_rows, out, model_);
        std::vector<PlanOption>& options = joins.options;
        for (PlanOption& option : options) option.cost = inputs + option.cost;

        // A merge join with exactly one plain-column key can take an input that arrives in key
        // order from an index instead of sorting it, and its output is in key order.
        if (hashable) {
            std::size_t merge = options.size() - 1;  // "MergeJoin (sorting both)"
            std::optional<ColumnId> lk, rk;
            if (split.keys.size() == 1) {
                lk = plain_column(split.keys[0].first);
                rk = plain_column(split.keys[0].second);
            }
            MergeVariant variant;
            if (lk && rk) {
                variant.left_key = *lk;
                variant.right_key = *rk;
                Cost left_cost = left_best + model_.sort(left_rows), right_cost = right_best + model_.sort(right_rows);
                auto lo = ordered_[join.left.get()].find(lk->value);
                auto ro = ordered_[join.right.get()].find(rk->value);
                if (lo != ordered_[join.left.get()].end() && lo->second < left_cost) {
                    left_cost = lo->second;
                    variant.left_ordered = true;
                }
                if (ro != ordered_[join.right.get()].end() && ro->second < right_cost) {
                    right_cost = ro->second;
                    variant.right_ordered = true;
                }
                options[merge].cost = left_cost + right_cost + model_.merge_join(left_rows, right_rows, out);
                options[merge].name = std::string("MergeJoin (") +
                                      (variant.left_ordered && variant.right_ordered ? "both inputs in order"
                                       : variant.left_ordered                         ? "left in order, sorting right"
                                       : variant.right_ordered                        ? "right in order, sorting left"
                                                                                      : "sorting both") +
                                      ")";
                ordered_[node.get()][lk->value] = options[merge].cost;
                ordered_[node.get()][rk->value] = options[merge].cost;
                merge_[&join] = variant;
            }
        }

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
    Cost total = pricer.price_all(logical);

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
