#include "physical/physical_planner.h"

#include <algorithm>
#include <stdexcept>


namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <class Node>
PhysicalPtr make(Node node) {
    return std::make_shared<const PhysicalPlan>(PhysicalPlan{std::move(node)});
}

BoundExprPtr make_expr(auto node, const BoundExpr& like) {
    return std::make_shared<const BoundExpr>(BoundExpr{std::move(node), like.type});
}

// Which ColumnId sits at each position of the rows a step produces.
using Layout = std::vector<ColumnId>;

BoundExprPtr to_positions(const BoundExprPtr& expr, const Layout& layout) {
    return std::visit(
        Overloaded{
            [&](const BoundLiteral&) { return expr; },
            [&](const BoundColumn& n) {
                auto it = std::find(layout.begin(), layout.end(), n.id);
                if (it == layout.end())
                    throw std::logic_error("column #" + std::to_string(n.id.value) +
                                           " is not produced by the step below");
                auto position = static_cast<std::uint32_t>(it - layout.begin());
                return make_expr(BoundColumn{ColumnId{position}}, *expr);
            },
            [&](const BoundUnary& n) {
                return make_expr(BoundUnary{n.op, to_positions(n.operand, layout)}, *expr);
            },
            [&](const BoundBinary& n) {
                return make_expr(BoundBinary{n.op, to_positions(n.left, layout),
                                             to_positions(n.right, layout)},
                                 *expr);
            },
            [&](const BoundIsNull& n) {
                return make_expr(BoundIsNull{to_positions(n.operand, layout), n.negated}, *expr);
            },
        },
        expr->node);
}

struct Planned {
    PhysicalPtr plan;
    Layout layout;  // empty after a Project: its rows hold values, not columns
};

Planned plan_node(const LogicalPlan& plan) {
    return std::visit(
        Overloaded{
            [&](const LogicalScan& n) {
                return Planned{make(PhysicalSeqScan{n.table}), n.columns};
            },
            [&](const LogicalEmpty& n) { return Planned{make(PhysicalEmpty{}), n.columns}; },
            [&](const LogicalJoin& n) {
                Planned left = plan_node(*n.left);
                Planned right = plan_node(*n.right);
                Layout layout = left.layout;
                layout.insert(layout.end(), right.layout.begin(), right.layout.end());
                BoundExprPtr condition = n.condition ? to_positions(n.condition, layout) : nullptr;
                return Planned{make(PhysicalNestedLoopJoin{left.plan, right.plan, condition}), layout};
            },
            [&](const LogicalFilter& n) {
                Planned in = plan_node(*n.input);
                return Planned{make(PhysicalFilter{in.plan, to_positions(n.predicate, in.layout)}),
                               in.layout};
            },
            [&](const LogicalProject& n) {
                Planned in = plan_node(*n.input);
                std::vector<ProjectItem> items;
                for (const ProjectItem& item : n.items)
                    items.push_back({to_positions(item.expr, in.layout), item.name});
                return Planned{make(PhysicalProject{in.plan, std::move(items)}), {}};
            },
            [&](const LogicalSort& n) {
                Planned in = plan_node(*n.input);
                std::vector<SortKey> keys;
                for (const SortKey& key : n.keys)
                    keys.push_back({to_positions(key.expr, in.layout), key.descending});
                return Planned{make(PhysicalSort{in.plan, std::move(keys)}), in.layout};
            },
            [&](const LogicalLimit& n) {
                Planned in = plan_node(*n.input);
                return Planned{make(PhysicalLimit{in.plan, n.count}), in.layout};
            },
        },
        plan.node);
}

}  // namespace

PhysicalPtr plan_physical(const LogicalPlan& plan) { return plan_node(plan).plan; }

}  // namespace cardinal
