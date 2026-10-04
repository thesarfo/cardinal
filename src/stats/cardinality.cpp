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
            [&](const LogicalJoin& n) {
                double pairs = rows(n.left) * rows(n.right);
                return n.condition ? pairs * estimate_selectivity(*n.condition, stats_) : pairs;
            },
        },
        plan.node);
}

}  // namespace cardinal
