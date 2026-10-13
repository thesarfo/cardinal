#include "logical/plan_util.h"

#include <stdexcept>

#include "binder/bound_expr.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <class Node>
PlanPtr make(Node node) {
    return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)});
}

}  // namespace

std::vector<PlanPtr> children_of(const LogicalPlan& node) {
    return std::visit(
        Overloaded{
            [](const LogicalScan&) { return std::vector<PlanPtr>{}; },
            [](const LogicalEmpty&) { return std::vector<PlanPtr>{}; },
            [](const LogicalJoin& n) { return std::vector<PlanPtr>{n.left, n.right}; },
            [](const auto& n) { return std::vector<PlanPtr>{n.input}; },
        },
        node.node);
}

PlanPtr input_of(const LogicalPlan& node) {
    std::vector<PlanPtr> children = children_of(node);
    return children.empty() ? nullptr : children.front();
}

std::vector<ColumnId> output_columns(const LogicalPlan& node) {
    return std::visit(
        Overloaded{
            [](const LogicalScan& n) { return n.columns; },
            [](const LogicalEmpty& n) { return n.columns; },
            [](const LogicalProject&) { return std::vector<ColumnId>{}; },
            [](const LogicalPrune& n) { return n.columns; },
            [](const LogicalAggregate& n) {
                std::vector<ColumnId> columns;
                for (const GroupKey& key : n.keys) columns.push_back(key.output);
                for (const AggregateItem& item : n.aggregates) columns.push_back(item.output);
                return columns;
            },
            [](const LogicalJoin& n) {
                std::vector<ColumnId> columns = output_columns(*n.left);
                std::vector<ColumnId> right = output_columns(*n.right);
                columns.insert(columns.end(), right.begin(), right.end());
                return columns;
            },
            [](const auto& n) { return output_columns(*n.input); },
        },
        node.node);
}

PlanPtr with_children(const LogicalPlan& node, std::vector<PlanPtr> children) {
    return std::visit(
        Overloaded{
            [](const LogicalScan&) -> PlanPtr { throw std::logic_error("a Scan has no input"); },
            [](const LogicalEmpty&) -> PlanPtr { throw std::logic_error("an Empty has no input"); },
            [&](LogicalJoin n) {
                n.left = std::move(children.at(0));
                n.right = std::move(children.at(1));
                return make(std::move(n));
            },
            [&](auto n) {
                n.input = std::move(children.at(0));
                return make(std::move(n));
            },
        },
        node.node);
}

namespace {

bool expr_same(const BoundExprPtr& a, const BoundExprPtr& b) {
    if (a == b) return true;
    return a && b && expr_equal(*a, *b);
}

bool keys_same(const std::vector<SortKey>& a, const std::vector<SortKey>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].descending != b[i].descending || !expr_same(a[i].expr, b[i].expr)) return false;
    return true;
}

bool items_same(const std::vector<ProjectItem>& a, const std::vector<ProjectItem>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || !expr_same(a[i].expr, b[i].expr)) return false;
    return true;
}

bool aggregates_same(const LogicalAggregate& a, const LogicalAggregate& b) {
    if (a.keys.size() != b.keys.size() || a.aggregates.size() != b.aggregates.size()) return false;
    for (std::size_t i = 0; i < a.keys.size(); ++i)
        if (a.keys[i].output != b.keys[i].output || !expr_same(a.keys[i].expr, b.keys[i].expr)) return false;
    for (std::size_t i = 0; i < a.aggregates.size(); ++i) {
        const AggregateItem &x = a.aggregates[i], &y = b.aggregates[i];
        if (x.func != y.func || x.output != y.output || !expr_same(x.arg, y.arg)) return false;
    }
    return true;
}

// Compares the node itself, not its inputs.
bool node_same(const LogicalPlan& a, const LogicalPlan& b) {
    if (a.node.index() != b.node.index()) return false;
    return std::visit(
        Overloaded{
            [&](const LogicalScan& x) {
                const auto& y = std::get<LogicalScan>(b.node);
                return x.table == y.table && x.alias == y.alias && x.columns == y.columns;
            },
            [&](const LogicalEmpty& x) { return x.columns == std::get<LogicalEmpty>(b.node).columns; },
            [&](const LogicalFilter& x) { return expr_same(x.predicate, std::get<LogicalFilter>(b.node).predicate); },
            [&](const LogicalProject& x) { return items_same(x.items, std::get<LogicalProject>(b.node).items); },
            [&](const LogicalSort& x) { return keys_same(x.keys, std::get<LogicalSort>(b.node).keys); },
            [&](const LogicalLimit& x) { return x.count == std::get<LogicalLimit>(b.node).count; },
            [&](const LogicalPrune& x) { return x.columns == std::get<LogicalPrune>(b.node).columns; },
            [&](const LogicalAggregate& x) { return aggregates_same(x, std::get<LogicalAggregate>(b.node)); },
            [&](const LogicalJoin& x) {
                const auto& y = std::get<LogicalJoin>(b.node);
                return x.type == y.type && expr_same(x.condition, y.condition);
            },
        },
        a.node);
}

}  // namespace

bool plan_equal(const PlanPtr& a, const PlanPtr& b) {
    if (a == b) return true;
    if (!a || !b || !node_same(*a, *b)) return false;
    std::vector<PlanPtr> x = children_of(*a), y = children_of(*b);
    for (std::size_t i = 0; i < x.size(); ++i)
        if (!plan_equal(x[i], y[i])) return false;
    return true;
}

}  // namespace cardinal
