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

PlanPtr input_of(const LogicalPlan& node) {
    return std::visit(
        Overloaded{
            [](const LogicalScan&) -> PlanPtr { return nullptr; },
            [](const LogicalEmpty&) -> PlanPtr { return nullptr; },
            [](const auto& n) -> PlanPtr { return n.input; },
        },
        node.node);
}

std::vector<ColumnId> output_columns(const LogicalPlan& node) {
    return std::visit(
        Overloaded{
            [](const LogicalScan& n) { return n.columns; },
            [](const LogicalEmpty& n) { return n.columns; },
            [](const LogicalProject&) { return std::vector<ColumnId>{}; },
            [](const auto& n) { return output_columns(*n.input); },
        },
        node.node);
}

PlanPtr with_input(const LogicalPlan& node, PlanPtr input) {
    return std::visit(
        Overloaded{
            [](const LogicalScan&) -> PlanPtr { throw std::logic_error("a Scan has no input"); },
            [](const LogicalEmpty&) -> PlanPtr { throw std::logic_error("an Empty has no input"); },
            [&](LogicalFilter n) { n.input = std::move(input); return make(std::move(n)); },
            [&](LogicalProject n) { n.input = std::move(input); return make(std::move(n)); },
            [&](LogicalSort n) { n.input = std::move(input); return make(std::move(n)); },
            [&](LogicalLimit n) { n.input = std::move(input); return make(std::move(n)); },
        },
        node.node);
}

}  // namespace cardinal
