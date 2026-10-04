#include "optimizer/filter_pushdown.h"

#include "logical/plan_util.h"
#include "optimizer/conjuncts.h"

namespace cardinal {

namespace {

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

PlanPtr filtered(const PlanPtr& input, const std::vector<BoundExprPtr>& parts) {
    return parts.empty() ? input : make(LogicalFilter{input, and_all(parts)});
}

}  // namespace

std::optional<PlanPtr> FilterPushdown::apply(const PlanPtr& node) const {
    const auto* filter = std::get_if<LogicalFilter>(&node->node);
    if (!filter) return std::nullopt;
    const auto* join = std::get_if<LogicalJoin>(&filter->input->node);
    if (!join) return std::nullopt;
    if (join->type != JoinType::Inner && join->type != JoinType::Cross) return std::nullopt;

    std::vector<BoundExprPtr> parts = split_ands(filter->predicate);
    std::vector<ColumnId> left_columns = output_columns(*join->left);
    std::vector<ColumnId> right_columns = output_columns(*join->right);

    std::vector<BoundExprPtr> to_left, to_right, stay;
    for (const BoundExprPtr& part : parts) {
        std::set<ColumnId> used = columns_used(*part);
        if (used.empty()) {
            stay.push_back(part);
        } else if (columns_within(used, left_columns)) {
            to_left.push_back(part);
        } else if (columns_within(used, right_columns)) {
            to_right.push_back(part);
        } else {
            stay.push_back(part);
        }
    }
    if (to_left.empty() && to_right.empty()) return std::nullopt;

    PlanPtr new_join = make(LogicalJoin{filtered(join->left, to_left), filtered(join->right, to_right),
                                        join->type, join->condition});
    return filtered(new_join, stay);
}

}  // namespace cardinal
