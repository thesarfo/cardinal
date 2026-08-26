#include "expr/evaluator.h"

#include <cmath>

#include "common/error.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

double as_number(const Value& v) {
    return v.type() == Type::Int ? static_cast<double>(v.as_int()) : v.as_double();
}

// -1, 0 or 1. Both values are non-NULL and the binder has checked they are comparable.
int compare(const Value& a, const Value& b) {
    if (a.type() == Type::Int && b.type() == Type::Int)
        return a.as_int() < b.as_int() ? -1 : a.as_int() > b.as_int() ? 1 : 0;
    if (a.type() == Type::Text) return a.as_text().compare(b.as_text()) < 0   ? -1
                                       : a.as_text() == b.as_text()           ? 0
                                                                              : 1;
    if (a.type() == Type::Bool) return static_cast<int>(a.as_bool()) - static_cast<int>(b.as_bool());
    double x = as_number(a), y = as_number(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

Value compare_op(BinaryOp op, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return Value();
    int c = compare(a, b);
    switch (op) {
        case BinaryOp::Eq: return Value(c == 0);
        case BinaryOp::Ne: return Value(c != 0);
        case BinaryOp::Lt: return Value(c < 0);
        case BinaryOp::Le: return Value(c <= 0);
        case BinaryOp::Gt: return Value(c > 0);
        default: return Value(c >= 0);
    }
}

Value int_arithmetic(BinaryOp op, std::int64_t a, std::int64_t b) {
    std::int64_t out = 0;
    bool overflow = false;
    switch (op) {
        case BinaryOp::Add: overflow = __builtin_add_overflow(a, b, &out); break;
        case BinaryOp::Sub: overflow = __builtin_sub_overflow(a, b, &out); break;
        case BinaryOp::Mul: overflow = __builtin_mul_overflow(a, b, &out); break;
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (b == 0) throw DbError("division by zero");
            if (a == INT64_MIN && b == -1) {
                overflow = true;
            } else {
                out = op == BinaryOp::Div ? a / b : a % b;
            }
            break;
        default: break;
    }
    if (overflow) throw DbError("integer overflow");
    return Value(out);
}

Value arithmetic(BinaryOp op, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return Value();
    if (a.type() == Type::Int && b.type() == Type::Int) return int_arithmetic(op, a.as_int(), b.as_int());

    double x = as_number(a), y = as_number(b);
    switch (op) {
        case BinaryOp::Add: return Value(x + y);
        case BinaryOp::Sub: return Value(x - y);
        case BinaryOp::Mul: return Value(x * y);
        default:
            if (y == 0) throw DbError("division by zero");
            return Value(x / y);
    }
}

Value logic(BinaryOp op, const Value& a, const Value& b) {
    // The value that decides the answer on its own: false for AND, true for OR.
    bool decider = op == BinaryOp::Or;
    if ((!a.is_null() && a.as_bool() == decider) || (!b.is_null() && b.as_bool() == decider))
        return Value(decider);
    if (a.is_null() || b.is_null()) return Value();
    return Value(!decider);
}

}  // namespace

Value evaluate(const BoundExpr& expr, const Row& row) {
    return std::visit(
        Overloaded{
            [&](const BoundLiteral& n) { return n.value; },
            [&](const BoundColumn& n) { return row[n.id.value]; },
            [&](const BoundUnary& n) {
                Value v = evaluate(*n.operand, row);
                if (v.is_null()) return v;
                if (n.op == UnaryOp::Not) return Value(!v.as_bool());
                if (v.type() == Type::Double) return Value(-v.as_double());
                if (v.as_int() == INT64_MIN) throw DbError("integer overflow");
                return Value(-v.as_int());
            },
            [&](const BoundBinary& n) {
                Value a = evaluate(*n.left, row);
                Value b = evaluate(*n.right, row);
                switch (n.op) {
                    case BinaryOp::And:
                    case BinaryOp::Or: return logic(n.op, a, b);
                    case BinaryOp::Add:
                    case BinaryOp::Sub:
                    case BinaryOp::Mul:
                    case BinaryOp::Div:
                    case BinaryOp::Mod: return arithmetic(n.op, a, b);
                    default: return compare_op(n.op, a, b);
                }
            },
            [&](const BoundIsNull& n) {
                bool null = evaluate(*n.operand, row).is_null();
                return Value(null != n.negated);
            },
        },
        expr.node);
}

}  // namespace cardinal
