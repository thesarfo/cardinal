#include "optimizer/having_pushdown.h"

#include <map>

#include "optimizer/conjuncts.h"

namespace cardinal {

namespace {

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

using Replacements = std::map<ColumnId, BoundExprPtr>;

// `expr` with each column in `with` replaced by its expression.
BoundExprPtr substitute(const BoundExprPtr& expr, const Replacements& with) {
    auto rebuilt = [&](auto node) { return std::make_shared<const BoundExpr>(BoundExpr{std::move(node), expr->type}); };
    return std::visit(
        Overloaded{
            [&](const BoundLiteral&) { return expr; },
            [&](const BoundColumn& n) {
                auto it = with.find(n.id);
                return it == with.end() ? expr : it->second;
            },
            [&](const BoundUnary& n) { return rebuilt(BoundUnary{n.op, substitute(n.operand, with)}); },
            [&](const BoundBinary& n) {
                return rebuilt(BoundBinary{n.op, substitute(n.left, with), substitute(n.right, with)});
            },
            [&](const BoundIsNull& n) { return rebuilt(BoundIsNull{substitute(n.operand, with), n.negated}); },
        },
        expr->node);
}

}  // namespace

std::optional<PlanPtr> HavingPushdown::apply(const PlanPtr& node) const {
    const auto* filter = std::get_if<LogicalFilter>(&node->node);
    if (!filter) return std::nullopt;
    const auto* aggregate = std::get_if<LogicalAggregate>(&filter->input->node);
    if (!aggregate || aggregate->keys.empty()) return std::nullopt;

    Replacements keys;
    std::vector<ColumnId> key_columns;
    for (const GroupKey& key : aggregate->keys) {
        keys[key.output] = key.expr;
        key_columns.push_back(key.output);
    }

    std::vector<BoundExprPtr> down, stay;
    for (const BoundExprPtr& part : split_ands(filter->predicate)) {
        std::set<ColumnId> used = columns_used(*part);
        if (!used.empty() && columns_within(used, key_columns)) down.push_back(substitute(part, keys));
        else stay.push_back(part);
    }
    if (down.empty()) return std::nullopt;

    PlanPtr input = make(LogicalFilter{aggregate->input, and_all(down)});
    PlanPtr grouped = make(LogicalAggregate{input, aggregate->keys, aggregate->aggregates});
    return stay.empty() ? grouped : make(LogicalFilter{grouped, and_all(stay)});
}

}  // namespace cardinal
