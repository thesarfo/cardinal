#include "binder/bound_expr.h"

#include "sql/ast_printer.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

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
