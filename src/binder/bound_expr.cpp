#include "binder/bound_expr.h"

#include "sql/ast_printer.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

bool expr_equal(const BoundExpr& a, const BoundExpr& b) {
    if (&a == &b) return true;
    if (a.node.index() != b.node.index()) return false;
    return std::visit(
        Overloaded{
            [&](const BoundLiteral& x) { return x.value == std::get<BoundLiteral>(b.node).value; },
            [&](const BoundColumn& x) { return x.id == std::get<BoundColumn>(b.node).id; },
            [&](const BoundUnary& x) {
                const auto& y = std::get<BoundUnary>(b.node);
                return x.op == y.op && expr_equal(*x.operand, *y.operand);
            },
            [&](const BoundBinary& x) {
                const auto& y = std::get<BoundBinary>(b.node);
                return x.op == y.op && expr_equal(*x.left, *y.left) && expr_equal(*x.right, *y.right);
            },
            [&](const BoundIsNull& x) {
                const auto& y = std::get<BoundIsNull>(b.node);
                return x.negated == y.negated && expr_equal(*x.operand, *y.operand);
            },
        },
        a.node);
}

std::string print(const BoundExpr& expr) {
    return std::visit(
        Overloaded{
            [](const BoundLiteral& n) { return format_literal(n.value); },
            [](const BoundColumn& n) { return "#" + std::to_string(n.id.value); },
            [](const BoundUnary& n) {
                return std::string("(") + unary_op_name(n.op) + " " + print(*n.operand) + ")";
            },
            [](const BoundBinary& n) {
                return std::string("(") + binary_op_name(n.op) + " " + print(*n.left) + " " +
                       print(*n.right) + ")";
            },
            [](const BoundIsNull& n) {
                return std::string(n.negated ? "(is-not-null " : "(is-null ") +
                       print(*n.operand) + ")";
            },
        },
        expr.node);
}

}  // namespace cardinal
