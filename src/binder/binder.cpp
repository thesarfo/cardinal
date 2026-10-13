#include "binder/binder.h"

#include <cctype>

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

// Where totals found while binding are collected. `scope` hands out their column ids.
struct Aggregation {
    Scope& scope;
    std::vector<AggregateItem> items;
};

std::string lower_case(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<AggFunc> aggregate_named(const std::string& name, bool star) {
    std::string n = lower_case(name);
    if (n == "count") return star ? AggFunc::CountStar : AggFunc::Count;
    if (n == "sum") return AggFunc::Sum;
    if (n == "avg") return AggFunc::Avg;
    if (n == "min") return AggFunc::Min;
    if (n == "max") return AggFunc::Max;
    return std::nullopt;
}

// True if a total is called anywhere in the expression.
bool has_aggregate(const Expr& expr) {
    return std::visit(
        Overloaded{
            [](const Literal&) { return false; },
            [](const ColumnRef&) { return false; },
            [](const Unary& n) { return has_aggregate(*n.operand); },
            [](const Binary& n) { return has_aggregate(*n.left) || has_aggregate(*n.right); },
            [](const IsNull& n) { return has_aggregate(*n.operand); },
            [](const Between& n) { return has_aggregate(*n.operand) || has_aggregate(*n.low) || has_aggregate(*n.high); },
            [](const InList& n) {
                if (has_aggregate(*n.operand)) return true;
                for (const ExprPtr& item : n.items)
                    if (has_aggregate(*item)) return true;
                return false;
            },
            [](const FunctionCall& n) { return aggregate_named(n.name, n.star).has_value(); },
        },
        expr.node);
}

class ExprBinder {
public:
    // With `aggregation`, a call to COUNT, SUM and so on is allowed and becomes a column of
    // the Aggregate's output. Without, it is an error that names `clause`.
    explicit ExprBinder(const Scope& scope, Aggregation* aggregation = nullptr, const char* clause = "this place")
        : scope_(scope), aggregation_(aggregation), clause_(clause) {}

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
                [&](const FunctionCall& n) { return bind_call(n); },
            },
            expr.node);
    }

private:
    BoundExprPtr bind_call(const FunctionCall& call) {
        std::optional<AggFunc> func = aggregate_named(call.name, call.star);
        if (!func) {
            if (call.star) throw DbError("only COUNT can take *, not " + call.name);
            throw DbError("unknown function " + call.name);
        }
        if (call.star && *func != AggFunc::CountStar) throw DbError("only COUNT can take *, not " + call.name);
        if (!aggregation_) throw DbError(std::string("aggregate functions are not allowed in ") + clause_);

        BoundExprPtr arg;
        if (!call.star) arg = ExprBinder(scope_, nullptr, "an aggregate's argument").bind(*call.arg);
        MaybeType arg_type = arg ? arg->type : MaybeType{};
        if ((*func == AggFunc::Sum || *func == AggFunc::Avg) && !is_number(arg_type))
            throw DbError(std::string(agg_name(*func)) + " needs numbers, got " + label(arg_type));

        MaybeType type;
        switch (*func) {
            case AggFunc::CountStar:
            case AggFunc::Count: type = Type::Int; break;
            case AggFunc::Avg: type = Type::Double; break;
            default: type = arg_type; break;  // Sum, Min, Max
        }

        // The same total written twice is worked out once.
        for (const AggregateItem& item : aggregation_->items) {
            bool same_arg = (!item.arg && !arg) || (item.arg && arg && expr_equal(*item.arg, *arg));
            if (item.func == *func && same_arg) return make(BoundColumn{item.output}, type);
        }
        ColumnId output = aggregation_->scope.add_computed(call.star ? "count(*)" : "?column?", type.value_or(Type::Int));
        aggregation_->items.push_back({*func, arg, output});
        return make(BoundColumn{output}, type);
    }

    const Scope& scope_;
    Aggregation* aggregation_;
    const char* clause_;
};

std::string output_name(const Expr& expr) {
    if (const auto* c = std::get_if<ColumnRef>(&expr.node)) return c->name;
    if (const auto* f = std::get_if<FunctionCall>(&expr.node)) {
        if (f->star) return lower_case(f->name) + "(*)";
        if (const auto* c = std::get_if<ColumnRef>(&f->arg->node)) return lower_case(f->name) + "(" + c->name + ")";
    }
    return "?column?";
}

// Above an Aggregate, only the group keys and the totals exist. A subtree equal to a group
// key's expression becomes that key's column; any base column still left is not grouped.
BoundExprPtr over_groups(const BoundExprPtr& expr, const std::vector<GroupKey>& keys, const Scope& scope) {
    for (const GroupKey& key : keys)
        if (expr_equal(*expr, *key.expr)) return make(BoundColumn{key.output}, expr->type);

    return std::visit(
        Overloaded{
            [&](const BoundLiteral&) { return expr; },
            [&](const BoundColumn& n) -> BoundExprPtr {
                for (const GroupKey& key : keys)
                    if (key.output == n.id) return expr;
                if (scope.meta(n.id).table.empty()) return expr;  // a total, already a column of the Aggregate
                const ColumnMeta& meta = scope.meta(n.id);
                throw DbError("column " + meta.table + "." + meta.name + " must appear in GROUP BY or be used in an aggregate");
            },
            [&](const BoundUnary& n) { return make(BoundUnary{n.op, over_groups(n.operand, keys, scope)}, expr->type); },
            [&](const BoundBinary& n) {
                return make(BoundBinary{n.op, over_groups(n.left, keys, scope), over_groups(n.right, keys, scope)}, expr->type);
            },
            [&](const BoundIsNull& n) { return make(BoundIsNull{over_groups(n.operand, keys, scope), n.negated}, expr->type); },
        },
        expr->node);
}

}  // namespace

BoundExprPtr bind_expression(const Expr& expr, const Scope& scope) {
    return ExprBinder(scope).bind(expr);
}

BoundSelect bind_select(const Select& select, const Catalog& catalog) {
    BoundSelect bound;
    auto add_table = [&](const TableRef& ref) {
        const Table* table = catalog.get_table(ref.table);
        if (!table) throw DbError("unknown table " + ref.table);
        bound.scope.add(ref.alias.value_or(ref.table), table->info());
    };
    add_table(select.from);
    for (const JoinClause& join : select.joins) add_table(join.table);

    for (std::size_t i = 0; i < select.joins.size(); ++i) {
        const JoinClause& join = select.joins[i];
        BoundJoin bound_join;
        if (join.on) {
            Scope visible = bound.scope.prefix(i + 2);
            bound_join.condition = ExprBinder(visible, nullptr, "ON").bind(*join.on);
            if (!is_bool(bound_join.condition->type))
                throw DbError("ON needs a true/false value, got " + label(bound_join.condition->type));
        }
        bound.joins.push_back(std::move(bound_join));
    }
    ExprBinder where_binder(bound.scope, nullptr, "WHERE");

    // Totals anywhere, GROUP BY, or HAVING make this an aggregate query.
    bool has_totals = !select.group_by.empty() || select.having != nullptr;
    for (const SelectItem& item : select.items)
        if (const auto* e = std::get_if<ExprPtr>(&item.item)) has_totals = has_totals || has_aggregate(**e);
    for (const OrderKey& key : select.order_by) has_totals = has_totals || has_aggregate(*key.expr);
    bound.aggregated = has_totals;

    if (select.where) {
        bound.where = where_binder.bind(*select.where);
        if (!is_bool(bound.where->type))
            throw DbError("WHERE needs a true/false value, got " + label(bound.where->type));
    }

    Aggregation aggregation{bound.scope, {}};
    ExprBinder binder(bound.scope, bound.aggregated ? &aggregation : nullptr, "this place");
    if (bound.aggregated) {
        ExprBinder key_binder(bound.scope, nullptr, "GROUP BY");
        for (const ExprPtr& key : select.group_by) {
            BoundExprPtr expr = key_binder.bind(*key);
            bool duplicate = false;
            for (const GroupKey& existing : bound.group_by) duplicate = duplicate || expr_equal(*existing.expr, *expr);
            if (duplicate) continue;
            ColumnId output = std::holds_alternative<BoundColumn>(expr->node)
                                  ? std::get<BoundColumn>(expr->node).id
                                  : bound.scope.add_computed("?column?", expr->type.value_or(Type::Int));
            bound.group_by.push_back({expr, output});
        }
    }
    // Over the Aggregate's output: totals are columns already, and group keys are replaced.
    auto above = [&](const BoundExprPtr& expr) {
        return bound.aggregated ? over_groups(expr, bound.group_by, bound.scope) : expr;
    };

    std::vector<std::pair<std::string, std::size_t>> aliased;  // alias -> index into bound.items
    for (const SelectItem& item : select.items) {
        if (std::holds_alternative<Star>(item.item)) {
            for (const BoundTable& t : bound.scope.tables()) {
                for (ColumnId id : t.columns) {
                    const ColumnMeta& meta = bound.scope.meta(id);
                    bound.items.push_back({above(make(BoundColumn{id}, meta.type)), meta.name});
                }
            }
        } else {
            const Expr& expr = *std::get<ExprPtr>(item.item);
            bound.items.push_back({above(binder.bind(expr)), item.alias ? *item.alias : output_name(expr)});
            if (item.alias) aliased.push_back({*item.alias, bound.items.size() - 1});
        }
    }

    if (select.having) {
        bound.having = above(binder.bind(*select.having));
        if (!is_bool(bound.having->type))
            throw DbError("HAVING needs a true/false value, got " + label(bound.having->type));
    }
    for (const OrderKey& key : select.order_by) {
        // A bare name that is a select item's alias means that item.
        const auto* named = std::get_if<ColumnRef>(&key.expr->node);
        BoundExprPtr by_alias;
        if (named && !named->table)
            for (const auto& [alias, index] : aliased)
                if (alias == named->name) by_alias = bound.items[index].expr;
        bound.order_by.push_back({by_alias ? by_alias : above(binder.bind(*key.expr)), key.descending});
    }
    bound.aggregates = std::move(aggregation.items);
    bound.limit = select.limit;
    return bound;
}

}  // namespace cardinal
