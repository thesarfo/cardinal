#include "sql/ast_printer.h"

#include <algorithm>
#include <cctype>

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

const char* unary_op_name(UnaryOp op) { return op == UnaryOp::Not ? "not" : "neg"; }

const char* binary_op_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq: return "=";
        case BinaryOp::Ne: return "<>";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "and";
        case BinaryOp::Or: return "or";
    }
    return "?";
}

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

std::string format_literal(const Value& v) {
    if (v.is_null()) return "null";
    if (v.type() != Type::Text) return v.to_string();
    std::string out = "'";
    for (char c : v.as_text()) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

namespace {

std::string table_text(const TableRef& ref) {
    return ref.table + (ref.alias ? " " + *ref.alias : "");
}

std::string join_exprs(const std::vector<ExprPtr>& exprs) {
    std::string out;
    for (const ExprPtr& e : exprs) out += " " + print(*e);
    return out;
}

}  // namespace

std::string print(const Expr& expr) {
    return std::visit(
        Overloaded{
            [](const Literal& n) { return format_literal(n.value); },
            [](const ColumnRef& n) {
                return "(col " + (n.table ? *n.table + "." : "") + n.name + ")";
            },
            [](const Unary& n) {
                return std::string("(") + unary_op_name(n.op) + " " + print(*n.operand) + ")";
            },
            [](const Binary& n) {
                return std::string("(") + binary_op_name(n.op) + " " + print(*n.left) + " " +
                       print(*n.right) + ")";
            },
            [](const IsNull& n) {
                return std::string(n.negated ? "(is-not-null " : "(is-null ") +
                       print(*n.operand) + ")";
            },
            [](const Between& n) {
                return std::string(n.negated ? "(not-between " : "(between ") +
                       print(*n.operand) + " " + print(*n.low) + " " + print(*n.high) + ")";
            },
            [](const InList& n) {
                return std::string(n.negated ? "(not-in " : "(in ") + print(*n.operand) +
                       join_exprs(n.items) + ")";
            },
        },
        expr.node);
}

std::string print(const Statement& statement) {
    return std::visit(
        Overloaded{
            [](const CreateTable& n) {
                std::string out = "(create-table " + n.name;
                for (const ColumnDef& c : n.columns)
                    out += " (" + c.name + " " + lower(type_name(c.type)) + ")";
                return out + ")";
            },
            [](const Insert& n) {
                std::string out = "(insert " + n.table;
                for (const auto& row : n.rows) out += " (row" + join_exprs(row) + ")";
                return out + ")";
            },
            [](const Select& n) {
                std::string out = "(select";
                for (const SelectItem& item : n.items) {
                    out += " ";
                    out += std::holds_alternative<Star>(item.item)
                               ? "(star)"
                               : print(*std::get<ExprPtr>(item.item));
                }
                out += " (from " + table_text(n.from) + ")";
                for (const JoinClause& join : n.joins) {
                    out += join.on ? " (inner-join " + table_text(join.table) + " (on " + print(*join.on) + "))"
                                   : " (cross-join " + table_text(join.table) + ")";
                }
                if (n.where) out += " (where " + print(*n.where) + ")";
                if (!n.order_by.empty()) {
                    out += " (order-by";
                    for (const OrderKey& k : n.order_by)
                        out += std::string(" (") + (k.descending ? "desc " : "asc ") +
                               print(*k.expr) + ")";
                    out += ")";
                }
                if (n.limit) out += " (limit " + std::to_string(*n.limit) + ")";
                return out + ")";
            },
            [](const Explain& n) {
                return std::string("(explain") + (n.analyze ? "-analyze" : "") + (n.verbose ? "-verbose" : "") + " " +
                       print(*n.inner) + ")";
            },
            [](const Analyze& n) { return "(analyze " + n.table + ")"; },
        },
        statement.node);
}

}  // namespace cardinal
