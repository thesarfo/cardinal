#include "optimizer/constant_folding.h"

#include "common/error.h"
#include "expr/evaluator.h"
#include "optimizer/expr_rule.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

bool is_literal(const BoundExprPtr& e) { return std::holds_alternative<BoundLiteral>(e->node); }

BoundExprPtr make(auto node, const BoundExpr& like) {
    return std::make_shared<const BoundExpr>(BoundExpr{std::move(node), like.type});
}

// `rebuilt` is `original` with its children already folded. If they are all literals,
// try to work it out. Otherwise (or if it fails) keep `original` when no child
// changed, so "same pointer" still means "nothing changed".
BoundExprPtr evaluate_if_constant(const BoundExprPtr& original, BoundExprPtr rebuilt, bool children_changed,
                                  bool all_literal) {
    BoundExprPtr fallback = children_changed ? rebuilt : original;
    if (!all_literal) return fallback;
    try {
        Value value = evaluate(*rebuilt, {});
        return make(BoundLiteral{std::move(value)}, *rebuilt);
    } catch (const DbError&) {
        return fallback;  // 1 / 0 and friends: leave it for the executor to report
    }
}

}  // namespace

BoundExprPtr fold_constants(const BoundExprPtr& expr) {
    return std::visit(
        Overloaded{
            [&](const BoundLiteral&) { return expr; },
            [&](const BoundColumn&) { return expr; },
            [&](const BoundUnary& n) {
                BoundExprPtr operand = fold_constants(n.operand);
                return evaluate_if_constant(expr, make(BoundUnary{n.op, operand}, *expr), operand != n.operand,
                                            is_literal(operand));
            },
            [&](const BoundBinary& n) {
                BoundExprPtr left = fold_constants(n.left);
                BoundExprPtr right = fold_constants(n.right);
                return evaluate_if_constant(expr, make(BoundBinary{n.op, left, right}, *expr),
                                            left != n.left || right != n.right,
                                            is_literal(left) && is_literal(right));
            },
            [&](const BoundIsNull& n) {
                BoundExprPtr operand = fold_constants(n.operand);
                return evaluate_if_constant(expr, make(BoundIsNull{operand, n.negated}, *expr),
                                            operand != n.operand, is_literal(operand));
            },
        },
        expr->node);
}

std::optional<PlanPtr> ConstantFolding::apply(const PlanPtr& node) const {
    return rewrite_expressions(node, fold_constants);
}

}  // namespace cardinal
