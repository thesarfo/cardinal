#include "stats/cardinality.h"

#include <algorithm>

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

double CardinalityEstimator::rows(const PlanPtr& plan) {
    if (auto it = cache_.find(plan.get()); it != cache_.end()) return it->second;
    double result = compute(*plan);
    cache_.emplace(plan.get(), result);
    held_.push_back(plan);
    return result;
}

double CardinalityEstimator::rows_of(const LogicalPlan& plan) {
    if (auto it = cache_.find(&plan); it != cache_.end()) return it->second;
    double result = compute(plan);
    cache_.emplace(&plan, result);
    return result;
}

double CardinalityEstimator::compute(const LogicalPlan& plan) {
    return std::visit(
        Overloaded{
            [&](const LogicalScan& n) {
                const Table* table = catalog_.get_table(n.table);
                return table ? static_cast<double>(table->rows().size()) : 0.0;
            },
            [&](const LogicalEmpty&) { return 0.0; },
            [&](const LogicalFilter& n) { return rows(n.input) * estimate_selectivity(*n.predicate, stats_); },
            [&](const LogicalProject& n) { return rows(n.input); },
            [&](const LogicalSort& n) { return rows(n.input); },
            [&](const LogicalPrune& n) { return rows(n.input); },
            [&](const LogicalLimit& n) { return std::min(rows(n.input), static_cast<double>(n.count)); },
            [&](const LogicalAggregate& n) {
                // One row per group. With no keys that is one row, even from an empty input.
                if (n.keys.empty()) return 1.0;
                double input = rows(n.input);
                // Groups can't outnumber the distinct values multiplied together (each NULL
                // makes one more), nor the rows. A key that isn't a plain column is a guess
                // of "every row is its own group", which the cap on rows then trims.
                double groups = 1;
                for (const GroupKey& key : n.keys) {
                    const auto* column = std::get_if<BoundColumn>(&key.expr->node);
                    const ColumnFacts* facts = column ? stats_.find(column->id) : nullptr;
                    if (!facts) {
                        groups = input;
                        break;
                    }
                    groups *= std::max<double>(1, static_cast<double>(facts->column->distinct) + (facts->column->null_fraction > 0 ? 1 : 0));
                }
                return std::min(input, groups);
            },
            [&](const LogicalJoin& n) {
                double pairs = rows(n.left) * rows(n.right);
                return n.condition ? pairs * estimate_selectivity(*n.condition, stats_) : pairs;
            },
        },
        plan.node);
}

}  // namespace cardinal
