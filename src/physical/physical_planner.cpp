#include "physical/physical_planner.h"

#include <algorithm>
#include <stdexcept>

#include "optimizer/conjuncts.h"
#include "physical/join_keys.h"


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

Planned plan_node(const LogicalPlan& plan, const PlannerOptions& options) {
    return std::visit(
        Overloaded{
            [&](const LogicalScan& n) {
                return Planned{make(PhysicalSeqScan{n.table}), n.columns};
            },
            [&](const LogicalEmpty& n) { return Planned{make(PhysicalEmpty{}), n.columns}; },
            [&](const LogicalPrune& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<ProjectItem> items;
                for (ColumnId id : n.columns) {
                    BoundExprPtr column = std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{id}, std::nullopt});
                    items.push_back({to_positions(column, in.layout), ""});
                }
                return Planned{make(PhysicalProject{in.plan, std::move(items)}), n.columns};
            },
            [&](const LogicalJoin& n) {
                Planned left = plan_node(*n.left, options);
                Planned right = plan_node(*n.right, options);
                Layout layout = left.layout;
                layout.insert(layout.end(), right.layout.begin(), right.layout.end());
                JoinChoice choice = options.choose_join ? options.choose_join(n) : JoinChoice{options.join_method, options.build_left};
                if (choice.method == JoinMethod::Hash) {
                    JoinKeys split = split_join_condition(n.condition, left.layout, right.layout);
                    if (!split.keys.empty()) {
                        std::vector<BoundExprPtr> left_keys, right_keys;
                        for (const auto& [left_expr, right_expr] : split.keys) {
                            left_keys.push_back(to_positions(left_expr, left.layout));
                            right_keys.push_back(to_positions(right_expr, right.layout));
                        }
                        BoundExprPtr residual = split.residual.empty() ? nullptr : to_positions(and_all(split.residual), layout);
                        return Planned{make(PhysicalHashJoin{left.plan, right.plan, std::move(left_keys), std::move(right_keys),
                                                             residual, choice.build_left}),
                                       layout};
                    }
                }
                BoundExprPtr condition = n.condition ? to_positions(n.condition, layout) : nullptr;
                return Planned{make(PhysicalNestedLoopJoin{left.plan, right.plan, condition}), layout};
            },
            [&](const LogicalFilter& n) {
                Planned in = plan_node(*n.input, options);
                return Planned{make(PhysicalFilter{in.plan, to_positions(n.predicate, in.layout)}),
                               in.layout};
            },
            [&](const LogicalProject& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<ProjectItem> items;
                for (const ProjectItem& item : n.items)
                    items.push_back({to_positions(item.expr, in.layout), item.name});
                return Planned{make(PhysicalProject{in.plan, std::move(items)}), {}};
            },
            [&](const LogicalSort& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<SortKey> keys;
                for (const SortKey& key : n.keys)
                    keys.push_back({to_positions(key.expr, in.layout), key.descending});
                return Planned{make(PhysicalSort{in.plan, std::move(keys)}), in.layout};
            },
            [&](const LogicalLimit& n) {
                Planned in = plan_node(*n.input, options);
                return Planned{make(PhysicalLimit{in.plan, n.count}), in.layout};
            },
        },
        plan.node);
}

}  // namespace

PhysicalPtr plan_physical(const LogicalPlan& plan, const PlannerOptions& options) { return plan_node(plan, options).plan; }

}  // namespace cardinal
