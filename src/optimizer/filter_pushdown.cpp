#include "optimizer/filter_pushdown.h"

#include <algorithm>

#include "logical/plan_util.h"

namespace cardinal {

namespace {

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

void split_ands(const BoundExprPtr& e, std::vector<BoundExprPtr>& out) {
    if (const auto* b = std::get_if<BoundBinary>(&e->node); b && b->op == BinaryOp::And) {
        split_ands(b->left, out);
        split_ands(b->right, out);
    } else {
        out.push_back(e);
    }
}

BoundExprPtr and_all(const std::vector<BoundExprPtr>& parts) {
    BoundExprPtr result = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i)
        result = std::make_shared<const BoundExpr>(
            BoundExpr{BoundBinary{BinaryOp::And, result, parts[i]}, Type::Bool});
    return result;
}

PlanPtr filtered(const PlanPtr& input, const std::vector<BoundExprPtr>& parts) {
    return parts.empty() ? input : make(LogicalFilter{input, and_all(parts)});
}

bool covers(const std::vector<ColumnId>& available, const std::set<ColumnId>& needed) {
    return std::all_of(needed.begin(), needed.end(), [&](ColumnId id) {
        return std::find(available.begin(), available.end(), id) != available.end();
    });
}

}  // namespace

std::optional<PlanPtr> FilterPushdown::apply(const PlanPtr& node) const {
    const auto* filter = std::get_if<LogicalFilter>(&node->node);
    if (!filter) return std::nullopt;
    const auto* join = std::get_if<LogicalJoin>(&filter->input->node);
    if (!join) return std::nullopt;
    if (join->type != JoinType::Inner && join->type != JoinType::Cross) return std::nullopt;

    std::vector<BoundExprPtr> parts;
    split_ands(filter->predicate, parts);
    std::vector<ColumnId> left_columns = output_columns(*join->left);
    std::vector<ColumnId> right_columns = output_columns(*join->right);

    std::vector<BoundExprPtr> to_left, to_right, stay;
    for (const BoundExprPtr& part : parts) {
        std::set<ColumnId> used = columns_used(*part);
        if (used.empty()) {
            stay.push_back(part);
        } else if (covers(left_columns, used)) {
            to_left.push_back(part);
        } else if (covers(right_columns, used)) {
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
