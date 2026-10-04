#include "logical/plan_util.h"

#include <stdexcept>

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

}  // namespace cardinal
