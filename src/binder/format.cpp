#include "binder/format.h"

#include <functional>

#include "sql/ast_printer.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

std::string upper_op(BinaryOp op) {
    switch (op) {
        case BinaryOp::And: return "AND";
        case BinaryOp::Or: return "OR";
        default: return binary_op_name(op);
    }
}

std::string literal(const Value& v) {
    if (v.is_null()) return "NULL";
    if (v.type() == Type::Bool) return v.as_bool() ? "TRUE" : "FALSE";
    return format_literal(v);
}

std::string scope_column_name(ColumnId id, const Scope& scope) {
    const ColumnMeta& meta = scope.meta(id);
    int tables_with_name = 0;
    for (const BoundTable& t : scope.tables())
        if (t.info->find_column(meta.name)) ++tables_with_name;
    return tables_with_name > 1 ? meta.table + "." + meta.name : meta.name;
}

constexpr int kOr = 1, kAnd = 2, kNot = 3, kCompare = 4, kAdd = 5, kMul = 6, kNeg = 7, kAtom = 8;

int level(BinaryOp op) {
    switch (op) {
        case BinaryOp::Or: return kOr;
        case BinaryOp::And: return kAnd;
        case BinaryOp::Add:
        case BinaryOp::Sub: return kAdd;
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod: return kMul;
        default: return kCompare;
    }
}

struct Text {
    std::string text;
    int level;
};

using Namer = std::function<std::string(ColumnId)>;

Text render(const BoundExpr& expr, const Namer& name);

// A child gets brackets when it binds looser than its parent, so the text reads back
// the way the tree is shaped.
std::string child(const BoundExpr& expr, const Namer& name, int min_level) {
    Text t = render(expr, name);
    return t.level < min_level ? "(" + t.text + ")" : t.text;
}

Text render(const BoundExpr& expr, const Namer& name) {
    return std::visit(
        Overloaded{
            [&](const BoundLiteral& n) { return Text{literal(n.value), kAtom}; },
            [&](const BoundColumn& n) { return Text{name(n.id), kAtom}; },
            [&](const BoundUnary& n) {
                if (n.op == UnaryOp::Not)
                    return Text{"NOT " + child(*n.operand, name, kNot), kNot};
                return Text{"-" + child(*n.operand, name, kAtom), kNeg};
            },
            [&](const BoundBinary& n) {
                int p = level(n.op);
                // Same-level children on the right keep their brackets: a AND (b AND c)
                // is a different tree from (a AND b) AND c. Comparisons never chain.
                int left_min = p == kCompare ? p + 1 : p;
                return Text{child(*n.left, name, left_min) + " " + upper_op(n.op) + " " +
                                child(*n.right, name, p + 1),
                            p};
            },
            [&](const BoundIsNull& n) {
                return Text{child(*n.operand, name, kAdd) +
                                (n.negated ? " IS NOT NULL" : " IS NULL"),
                            kCompare};
            },
        },
        expr.node);
}

}  // namespace

std::string format(const BoundExpr& expr, const Scope& scope) {
    return render(expr, [&](ColumnId id) { return scope_column_name(id, scope); }).text;
}

std::string format(const BoundExpr& expr, const std::function<std::string(ColumnId)>& column_name) {
    return render(expr, column_name).text;
}

}  // namespace cardinal
