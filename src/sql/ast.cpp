#include "sql/ast.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

TableRef clone(const TableRef& ref) { return ref; }

std::vector<ExprPtr> clone_all(const std::vector<ExprPtr>& exprs) {
    std::vector<ExprPtr> out;
    for (const ExprPtr& e : exprs) out.push_back(clone(*e));
    return out;
}

}  // namespace

ExprPtr clone(const Expr& expr) {
    return std::visit(
        Overloaded{
            [](const Literal& n) { return make_expr(Literal{n.value}); },
            [](const ColumnRef& n) { return make_expr(ColumnRef{n.table, n.name}); },
            [](const Unary& n) { return make_expr(Unary{n.op, clone(*n.operand)}); },
            [](const Binary& n) { return make_expr(Binary{n.op, clone(*n.left), clone(*n.right)}); },
            [](const IsNull& n) { return make_expr(IsNull{clone(*n.operand), n.negated}); },
            [](const Between& n) {
                return make_expr(Between{clone(*n.operand), clone(*n.low), clone(*n.high), n.negated});
            },
            [](const InList& n) { return make_expr(InList{clone(*n.operand), clone_all(n.items), n.negated}); },
        },
        expr.node);
}

Select clone(const Select& select) {
    Select out;
    for (const SelectItem& item : select.items) {
        if (std::holds_alternative<Star>(item.item)) {
            out.items.push_back({Star{}});
        } else {
            out.items.push_back({clone(*std::get<ExprPtr>(item.item))});
        }
    }
    out.from = clone(select.from);
    for (const JoinClause& join : select.joins)
        out.joins.push_back({clone(join.table), join.on ? clone(*join.on) : nullptr});
    if (select.where) out.where = clone(*select.where);
    for (const OrderKey& key : select.order_by) out.order_by.push_back({clone(*key.expr), key.descending});
    out.limit = select.limit;
    return out;
}

}  // namespace cardinal
