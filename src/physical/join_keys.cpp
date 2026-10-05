#include "physical/join_keys.h"

#include "optimizer/conjuncts.h"

namespace cardinal {

JoinKeys split_join_condition(const BoundExprPtr& condition, const std::vector<ColumnId>& left_columns,
                              const std::vector<ColumnId>& right_columns) {
    JoinKeys out;
    if (!condition) return out;
    for (const BoundExprPtr& part : split_ands(condition)) {
        const auto* b = std::get_if<BoundBinary>(&part->node);
        if (b && b->op == BinaryOp::Eq) {
            std::set<ColumnId> l = columns_used(*b->left);
            std::set<ColumnId> r = columns_used(*b->right);
            if (!l.empty() && !r.empty()) {
                if (columns_within(l, left_columns) && columns_within(r, right_columns)) {
                    out.keys.push_back({b->left, b->right});
                    continue;
                }
                if (columns_within(l, right_columns) && columns_within(r, left_columns)) {
                    out.keys.push_back({b->right, b->left});
                    continue;
                }
            }
        }
        out.residual.push_back(part);
    }
    return out;
}

}  // namespace cardinal
