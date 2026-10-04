#include "optimizer/conjuncts.h"

#include <algorithm>

namespace cardinal {

namespace {

void collect(const BoundExprPtr& e, std::vector<BoundExprPtr>& out) {
    if (const auto* b = std::get_if<BoundBinary>(&e->node); b && b->op == BinaryOp::And) {
        collect(b->left, out);
        collect(b->right, out);
    } else {
        out.push_back(e);
    }
}

}  // namespace

std::vector<BoundExprPtr> split_ands(const BoundExprPtr& expr) {
    std::vector<BoundExprPtr> parts;
    collect(expr, parts);
    return parts;
}

BoundExprPtr and_all(const std::vector<BoundExprPtr>& parts) {
    BoundExprPtr result = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i)
        result = std::make_shared<const BoundExpr>(
            BoundExpr{BoundBinary{BinaryOp::And, result, parts[i]}, Type::Bool});
    return result;
}

bool columns_within(const std::set<ColumnId>& used, const std::vector<ColumnId>& available) {
    return std::all_of(used.begin(), used.end(), [&](ColumnId id) {
        return std::find(available.begin(), available.end(), id) != available.end();
    });
}

}  // namespace cardinal
