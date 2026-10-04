#include "optimizer/expr_rule.h"

#include "logical/plan_util.h"

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

std::optional<PlanPtr> rewrite_expressions(const PlanPtr& node, const ExprRewrite& rewrite) {
    bool changed = false;
    auto apply = [&](const BoundExprPtr& expr) {
        BoundExprPtr out = rewrite(expr);
        if (out != expr) changed = true;
        return out;
    };

    PlanPtr result = std::visit(
        Overloaded{
            [&](const LogicalScan&) { return node; },
            [&](const LogicalEmpty&) { return node; },
            [&](const LogicalPrune&) { return node; },
            [&](const LogicalFilter& n) {
                BoundExprPtr predicate = apply(n.predicate);
                return changed ? make(LogicalFilter{n.input, predicate}) : node;
            },
            [&](const LogicalProject& n) {
                std::vector<ProjectItem> items;
                for (const ProjectItem& item : n.items) items.push_back({apply(item.expr), item.name});
                return changed ? make(LogicalProject{n.input, std::move(items)}) : node;
            },
            [&](const LogicalSort& n) {
                std::vector<SortKey> keys;
                for (const SortKey& key : n.keys) keys.push_back({apply(key.expr), key.descending});
                return changed ? make(LogicalSort{n.input, std::move(keys)}) : node;
            },
            [&](const LogicalLimit&) { return node; },
            [&](const LogicalJoin& n) {
                if (!n.condition) return node;
                BoundExprPtr condition = apply(n.condition);
                return changed ? make(LogicalJoin{n.left, n.right, n.type, condition}) : node;
            },
        },
        node->node);

    if (!changed) return std::nullopt;
    return result;
}

}  // namespace cardinal
