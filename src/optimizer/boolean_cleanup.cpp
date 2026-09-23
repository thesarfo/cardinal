#include "optimizer/boolean_cleanup.h"

#include "optimizer/expr_rule.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

BoundExprPtr make(auto node, std::optional<Type> type) {
    return std::make_shared<const BoundExpr>(BoundExpr{std::move(node), type});
}

BoundExprPtr bool_literal(bool b) { return make(BoundLiteral{Value(b)}, Type::Bool); }

std::optional<bool> as_bool_literal(const BoundExpr& e) {
    const auto* lit = std::get_if<BoundLiteral>(&e.node);
    if (!lit || lit->value.is_null() || lit->value.type() != Type::Bool) return std::nullopt;
    return lit->value.as_bool();
}

void flatten(const BoundExprPtr& e, BinaryOp op, std::vector<BoundExprPtr>& out) {
    if (const auto* b = std::get_if<BoundBinary>(&e->node); b && b->op == op) {
        flatten(b->left, op, out);
        flatten(b->right, op, out);
    } else {
        out.push_back(e);
    }
}

// `expr` is an AND or OR whose operands are already clean.
BoundExprPtr clean_chain(const BoundExprPtr& expr, BinaryOp op) {
    const bool is_and = op == BinaryOp::And;
    std::vector<BoundExprPtr> operands;
    flatten(expr, op, operands);

    std::vector<BoundExprPtr> kept;
    for (const BoundExprPtr& e : operands) {
        if (std::optional<bool> b = as_bool_literal(*e)) {
            // TRUE for AND and FALSE for OR change nothing. The other one decides it all.
            if (*b == is_and) continue;
            return bool_literal(*b);
        }
        bool repeated = false;
        for (const BoundExprPtr& k : kept)
            if (expr_equal(*k, *e)) { repeated = true; break; }
        if (!repeated) kept.push_back(e);
    }

    // Nothing dropped or repeated: leave the tree exactly as it was.
    if (kept.size() == operands.size()) return expr;

    if (kept.empty()) return bool_literal(is_and);
    BoundExprPtr result = kept[0];
    for (std::size_t i = 1; i < kept.size(); ++i)
        result = make(BoundBinary{op, result, kept[i]}, Type::Bool);
    // A lone NULL keeps the AND's boolean type, so a column's type doesn't change.
    if (!result->type) {
        if (const auto* lit = std::get_if<BoundLiteral>(&result->node)) return make(*lit, Type::Bool);
    }
    return result;
}

}  // namespace

BoundExprPtr clean_booleans(const BoundExprPtr& expr) {
    return std::visit(
        Overloaded{
            [&](const BoundLiteral&) { return expr; },
            [&](const BoundColumn&) { return expr; },
            [&](const BoundUnary& n) {
                BoundExprPtr operand = clean_booleans(n.operand);
                if (n.op == UnaryOp::Not) {
                    if (const auto* inner = std::get_if<BoundUnary>(&operand->node);
                        inner && inner->op == UnaryOp::Not)
                        return inner->operand;
                }
                if (operand == n.operand) return expr;
                return make(BoundUnary{n.op, operand}, expr->type);
            },
            [&](const BoundBinary& n) {
                BoundExprPtr left = clean_booleans(n.left);
                BoundExprPtr right = clean_booleans(n.right);
                BoundExprPtr rebuilt =
                    (left == n.left && right == n.right) ? expr : make(BoundBinary{n.op, left, right}, expr->type);
                if (n.op == BinaryOp::And || n.op == BinaryOp::Or) return clean_chain(rebuilt, n.op);
                return rebuilt;
            },
            [&](const BoundIsNull& n) {
                BoundExprPtr operand = clean_booleans(n.operand);
                if (operand == n.operand) return expr;
                return make(BoundIsNull{operand, n.negated}, expr->type);
            },
        },
        expr->node);
}

std::optional<PlanPtr> BooleanCleanup::apply(const PlanPtr& node) const {
    return rewrite_expressions(node, clean_booleans);
}

}  // namespace cardinal
