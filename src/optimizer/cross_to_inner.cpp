#include "optimizer/cross_to_inner.h"

#include "logical/plan_util.h"
#include "optimizer/conjuncts.h"

namespace cardinal {

namespace {

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

bool reads_both(const std::set<ColumnId>& used, const std::vector<ColumnId>& left, const std::vector<ColumnId>& right) {
    return !columns_within(used, left) && !columns_within(used, right);
}

// left_expr = right_expr, with one side from each input, in either order.
bool is_join_equality(const BoundExpr& e, const std::vector<ColumnId>& left, const std::vector<ColumnId>& right) {
    const auto* b = std::get_if<BoundBinary>(&e.node);
    if (!b || b->op != BinaryOp::Eq) return false;
    std::set<ColumnId> l = columns_used(*b->left);
    std::set<ColumnId> r = columns_used(*b->right);
    if (l.empty() || r.empty()) return false;
    return (columns_within(l, left) && columns_within(r, right)) || (columns_within(l, right) && columns_within(r, left));
}

}  // namespace

std::optional<PlanPtr> CrossToInnerJoin::apply(const PlanPtr& node) const {
    const auto* filter = std::get_if<LogicalFilter>(&node->node);
    if (!filter) return std::nullopt;
    const auto* join = std::get_if<LogicalJoin>(&filter->input->node);
    if (!join || join->type != JoinType::Cross) return std::nullopt;

    std::vector<ColumnId> left = output_columns(*join->left);
    std::vector<ColumnId> right = output_columns(*join->right);

    std::vector<BoundExprPtr> parts = split_ands(filter->predicate);
    bool has_equality = false;
    for (const BoundExprPtr& part : parts)
        if (is_join_equality(*part, left, right)) has_equality = true;
    if (!has_equality) return std::nullopt;

    std::vector<BoundExprPtr> condition, stay;
    for (const BoundExprPtr& part : parts)
        (reads_both(columns_used(*part), left, right) ? condition : stay).push_back(part);

    PlanPtr inner = make(LogicalJoin{join->left, join->right, JoinType::Inner, and_all(condition)});
    return stay.empty() ? inner : make(LogicalFilter{inner, and_all(stay)});
}

}  // namespace cardinal
