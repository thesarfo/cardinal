#include "binder/binder.h"

#include "common/error.h"
#include "sql/ast_printer.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

using MaybeType = std::optional<Type>;

std::string label(MaybeType type) { return type ? type_name(*type) : "NULL"; }

bool is_number(MaybeType type) { return !type || *type == Type::Int || *type == Type::Double; }
bool is_bool(MaybeType type) { return !type || *type == Type::Bool; }

BoundExprPtr make(auto node, MaybeType type) {
    return std::make_shared<const BoundExpr>(BoundExpr{std::move(node), type});
}

BoundExprPtr make_binary(BinaryOp op, BoundExprPtr left, BoundExprPtr right, MaybeType type) {
    return make(BoundBinary{op, std::move(left), std::move(right)}, type);
}

bool is_comparison(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
        case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge: return true;
        default: return false;
    }
}

BoundExprPtr make_compare(BinaryOp op, BoundExprPtr left, BoundExprPtr right) {
    MaybeType l = left->type, r = right->type;
    bool ok = !l || !r || *l == *r || (is_number(l) && is_number(r));
    if (!ok) throw DbError("cannot compare " + label(l) + " with " + label(r));
    return make_binary(op, std::move(left), std::move(right), Type::Bool);
}

BoundExprPtr make_arithmetic(BinaryOp op, BoundExprPtr left, BoundExprPtr right) {
    MaybeType l = left->type, r = right->type;
    const char* name = binary_op_name(op);
    if (op == BinaryOp::Mod) {
        for (MaybeType t : {l, r})
            if (t && *t != Type::Int)
                throw DbError(std::string("operator % needs whole numbers, got ") + label(t));
    }
    for (MaybeType t : {l, r})
        if (!is_number(t))
            throw DbError(std::string("operator ") + name + " needs numbers, got " + label(t));

    MaybeType type;
    if (l == Type::Double || r == Type::Double) {
        type = Type::Double;
    } else if (l == Type::Int || r == Type::Int) {
        type = Type::Int;
    }
    return make_binary(op, std::move(left), std::move(right), type);
}

BoundExprPtr make_logic(BinaryOp op, BoundExprPtr left, BoundExprPtr right) {
    const char* name = op == BinaryOp::And ? "AND" : "OR";
    for (MaybeType t : {left->type, right->type})
        if (!is_bool(t)) throw DbError(std::string(name) + " needs true/false values, got " + label(t));
    return make_binary(op, std::move(left), std::move(right), Type::Bool);
}

// NOT of an already-bound expression. In three-valued logic NOT (a < b) is exactly
// a >= b, and De Morgan holds, so the NOT can be pushed all the way in.
BoundExprPtr negate(const BoundExprPtr& e) {
    if (const auto* b = std::get_if<BoundBinary>(&e->node)) {
        switch (b->op) {
            case BinaryOp::Eq: return make_binary(BinaryOp::Ne, b->left, b->right, Type::Bool);
            case BinaryOp::Ne: return make_binary(BinaryOp::Eq, b->left, b->right, Type::Bool);
            case BinaryOp::Lt: return make_binary(BinaryOp::Ge, b->left, b->right, Type::Bool);
            case BinaryOp::Le: return make_binary(BinaryOp::Gt, b->left, b->right, Type::Bool);
            case BinaryOp::Gt: return make_binary(BinaryOp::Le, b->left, b->right, Type::Bool);
            case BinaryOp::Ge: return make_binary(BinaryOp::Lt, b->left, b->right, Type::Bool);
            case BinaryOp::And:
                return make_binary(BinaryOp::Or, negate(b->left), negate(b->right), Type::Bool);
            case BinaryOp::Or:
                return make_binary(BinaryOp::And, negate(b->left), negate(b->right), Type::Bool);
            default: break;
        }
    }
    if (const auto* n = std::get_if<BoundIsNull>(&e->node))
        return make(BoundIsNull{n->operand, !n->negated}, Type::Bool);
    if (const auto* u = std::get_if<BoundUnary>(&e->node))
        if (u->op == UnaryOp::Not) return u->operand;
    if (const auto* l = std::get_if<BoundLiteral>(&e->node)) {
        if (l->value.is_null()) return e;
        return make(BoundLiteral{Value(!l->value.as_bool())}, Type::Bool);
    }
    return make(BoundUnary{UnaryOp::Not, e}, Type::Bool);
}

class ExprBinder {
public:
    explicit ExprBinder(const Scope& scope) : scope_(scope) {}

    BoundExprPtr bind(const Expr& expr) {
        return std::visit(
            Overloaded{
                [&](const Literal& n) {
                    MaybeType type;
                    if (!n.value.is_null()) type = n.value.type();
                    return make(BoundLiteral{n.value}, type);
                },
                [&](const ColumnRef& n) {
                    ColumnId id = scope_.resolve(n.table, n.name);
                    return make(BoundColumn{id}, scope_.meta(id).type);
                },
                [&](const Unary& n) {
                    BoundExprPtr operand = bind(*n.operand);
                    if (n.op == UnaryOp::Not) {
                        if (!is_bool(operand->type))
                            throw DbError("NOT needs a true/false value, got " + label(operand->type));
                        return negate(operand);
                    }
                    if (!is_number(operand->type))
                        throw DbError("unary - needs a number, got " + label(operand->type));
                    return make(BoundUnary{UnaryOp::Neg, operand}, operand->type);
                },
                [&](const Binary& n) {
                    BoundExprPtr left = bind(*n.left);
                    BoundExprPtr right = bind(*n.right);
                    if (n.op == BinaryOp::And || n.op == BinaryOp::Or)
                        return make_logic(n.op, left, right);
                    if (is_comparison(n.op)) return make_compare(n.op, left, right);
                    return make_arithmetic(n.op, left, right);
                },
                [&](const IsNull& n) {
                    return make(BoundIsNull{bind(*n.operand), n.negated}, Type::Bool);
                },
                [&](const Between& n) {
                    BoundExprPtr x = bind(*n.operand);
                    BoundExprPtr low = bind(*n.low);
                    BoundExprPtr high = bind(*n.high);
                    BoundExprPtr both = make_logic(BinaryOp::And, make_compare(BinaryOp::Ge, x, low),
                                                   make_compare(BinaryOp::Le, x, high));
                    return n.negated ? negate(both) : both;
                },
                [&](const InList& n) {
                    BoundExprPtr x = bind(*n.operand);
                    BoundExprPtr any;
                    for (const ExprPtr& item : n.items) {
                        BoundExprPtr eq = make_compare(BinaryOp::Eq, x, bind(*item));
                        any = any ? make_logic(BinaryOp::Or, any, eq) : eq;
                    }
                    return n.negated ? negate(any) : any;
                },
            },
            expr.node);
    }

private:
    const Scope& scope_;
};

std::string output_name(const Expr& expr) {
    if (const auto* c = std::get_if<ColumnRef>(&expr.node)) return c->name;
    return "?column?";
}

}  // namespace

BoundExprPtr bind_expression(const Expr& expr, const Scope& scope) {
    return ExprBinder(scope).bind(expr);
}

BoundSelect bind_select(const Select& select, const Catalog& catalog) {
    const Table* table = catalog.get_table(select.from);
    if (!table) throw DbError("unknown table " + select.from);

    BoundSelect bound;
    bound.scope.add(select.from, table->info());
    ExprBinder binder(bound.scope);

    for (const SelectItem& item : select.items) {
        if (std::holds_alternative<Star>(item.item)) {
            for (const BoundTable& t : bound.scope.tables()) {
                for (ColumnId id : t.columns) {
                    const ColumnMeta& meta = bound.scope.meta(id);
                    bound.items.push_back({make(BoundColumn{id}, meta.type), meta.name});
                }
            }
        } else {
            const Expr& expr = *std::get<ExprPtr>(item.item);
            bound.items.push_back({binder.bind(expr), output_name(expr)});
        }
    }

    if (select.where) {
        bound.where = binder.bind(*select.where);
        if (!is_bool(bound.where->type))
            throw DbError("WHERE needs a true/false value, got " + label(bound.where->type));
    }
    for (const OrderKey& key : select.order_by)
        bound.order_by.push_back({binder.bind(*key.expr), key.descending});
    bound.limit = select.limit;
    return bound;
}

}  // namespace cardinal
