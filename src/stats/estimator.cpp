#include "stats/estimator.h"

#include <algorithm>

#include "expr/evaluator.h"

namespace cardinal {

namespace {

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

bool is_number(const Value& v) { return !v.is_null() && (v.type() == Type::Int || v.type() == Type::Double); }
double number(const Value& v) { return v.type() == Type::Int ? static_cast<double>(v.as_int()) : v.as_double(); }

// col op literal, with the literal moved to the right (5 < a becomes a > 5).
struct ColumnVsLiteral {
    ColumnId column;
    BinaryOp op;
    Value literal;
};

BinaryOp flipped(BinaryOp op) {
    switch (op) {
        case BinaryOp::Lt: return BinaryOp::Gt;
        case BinaryOp::Le: return BinaryOp::Ge;
        case BinaryOp::Gt: return BinaryOp::Lt;
        case BinaryOp::Ge: return BinaryOp::Le;
        default: return op;
    }
}

bool is_comparison(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
        case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge: return true;
        default: return false;
    }
}

std::optional<ColumnVsLiteral> column_vs_literal(const BoundExpr& e) {
    const auto* b = std::get_if<BoundBinary>(&e.node);
    if (!b || !is_comparison(b->op)) return std::nullopt;
    const auto* lc = std::get_if<BoundColumn>(&b->left->node);
    const auto* rl = std::get_if<BoundLiteral>(&b->right->node);
    if (lc && rl) return ColumnVsLiteral{lc->id, b->op, rl->value};
    const auto* ll = std::get_if<BoundLiteral>(&b->left->node);
    const auto* rc = std::get_if<BoundColumn>(&b->right->node);
    if (ll && rc) return ColumnVsLiteral{rc->id, flipped(b->op), ll->value};
    return std::nullopt;
}

// ---- facts about one column, as fractions of all the table's rows ----

double rows_of(const ColumnFacts& f) { return static_cast<double>(std::max<std::int64_t>(1, f.table_rows)); }
double common_share(const ColumnFacts& f) {
    std::int64_t total = 0;
    for (const CommonValue& c : f.column->common) total += c.count;
    return static_cast<double>(total) / rows_of(f);
}
double non_null(const ColumnFacts& f) { return 1.0 - f.column->null_fraction; }

// Rows equal to `v`. A common value has its exact share. Any other value gets an equal
// slice of what the common values leave.
double equals(const ColumnFacts& f, const Value& v) {
    const ColumnStats& c = *f.column;
    for (const CommonValue& cv : c.common)
        if (compare_values(cv.value, v) == 0) return static_cast<double>(cv.count) / rows_of(f);
    if (!c.min.is_null() && (compare_values(v, c.min) < 0 || compare_values(v, c.max) > 0)) return 0.0;
    std::int64_t remaining_distinct = c.distinct - static_cast<std::int64_t>(c.common.size());
    if (remaining_distinct <= 0) return 0.0;
    return clamp01(non_null(f) - common_share(f)) / static_cast<double>(remaining_distinct);
}

// The share of a histogram's values that are below x, reading across the buckets in a straight line.
double below_in_histogram(const Histogram& h, double x) {
    const std::vector<double>& b = h.bounds;
    if (x <= b.front()) return 0.0;
    if (x > b.back()) return 1.0;
    std::size_t i = 0;
    while (i + 2 < b.size() && b[i + 1] < x) ++i;  // the last bucket whose lower edge is below x
    double width = b[i + 1] - b[i];
    double within = width > 0 ? (x - b[i]) / width : 1.0;
    return (static_cast<double>(i) + within) / static_cast<double>(h.buckets());
}

// Rows with a value below `v`.
double less_than(const ColumnFacts& f, const Value& v) {
    const ColumnStats& c = *f.column;
    double result = 0;
    for (const CommonValue& cv : c.common)
        if (compare_values(cv.value, v) < 0) result += static_cast<double>(cv.count) / rows_of(f);

    double rest = clamp01(non_null(f) - common_share(f));
    if (c.histogram && is_number(v)) {
        result += static_cast<double>(c.histogram->rows) / rows_of(f) * below_in_histogram(*c.histogram, number(v));
    } else if (rest > 0) {
        result += rest * kDefaultRange;  // no way to place the value among the rest
    }
    return result;
}

// Rows for which `column op literal` is true.
double compare_selectivity(const ColumnFacts& f, BinaryOp op, const Value& v) {
    if (v.is_null()) return 0.0;  // comparing with NULL is never true
    double eq = equals(f, v);
    double lt = less_than(f, v);
    double result = 0;
    switch (op) {
        case BinaryOp::Eq: result = eq; break;
        case BinaryOp::Ne: result = non_null(f) - eq; break;
        case BinaryOp::Lt: result = lt; break;
        case BinaryOp::Le: result = lt + eq; break;
        case BinaryOp::Gt: result = non_null(f) - lt - eq; break;
        default: result = non_null(f) - lt; break;  // Ge
    }
    return clamp01(result);
}

double fallback_for(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return kDefaultEquality;
        case BinaryOp::Ne: return 1.0 - kDefaultEquality;
        default: return kDefaultRange;
    }
}

bool is_lower_bound(BinaryOp op) { return op == BinaryOp::Gt || op == BinaryOp::Ge; }
bool is_upper_bound(BinaryOp op) { return op == BinaryOp::Lt || op == BinaryOp::Le; }

double selectivity(const BoundExpr& e, const StatsLookup& stats);

std::vector<const BoundExpr*> parts_of(const BoundExpr& e, BinaryOp op) {
    std::vector<const BoundExpr*> out;
    if (const auto* b = std::get_if<BoundBinary>(&e.node); b && b->op == op) {
        for (const BoundExpr* side : {b->left.get(), b->right.get()})
            for (const BoundExpr* p : parts_of(*side, op)) out.push_back(p);
    } else {
        out.push_back(&e);
    }
    return out;
}

// a AND b AND ...: multiply, except that a lower and an upper bound on the same column
// are one range, worked out together (see the notes in docs/cardinality-estimation.md).
double and_selectivity(const BoundExpr& e, const StatsLookup& stats) {
    std::vector<const BoundExpr*> parts = parts_of(e, BinaryOp::And);
    std::vector<bool> used(parts.size(), false);
    double result = 1.0;

    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (used[i]) continue;
        auto first = column_vs_literal(*parts[i]);
        if (first && (is_lower_bound(first->op) || is_upper_bound(first->op))) {
            const ColumnFacts* facts = stats.find(first->column);
            for (std::size_t j = i + 1; facts && j < parts.size(); ++j) {
                auto second = column_vs_literal(*parts[j]);
                if (used[j] || !second || !(second->column == first->column)) continue;
                bool opposite = (is_lower_bound(first->op) && is_upper_bound(second->op)) ||
                                (is_upper_bound(first->op) && is_lower_bound(second->op));
                if (!opposite) continue;
                // P(low <= x <= high) = P(x >= low) + P(x <= high) - P(x is not NULL)
                double both = compare_selectivity(*facts, first->op, first->literal) +
                              compare_selectivity(*facts, second->op, second->literal) - non_null(*facts);
                result *= clamp01(both);
                used[i] = used[j] = true;
                break;
            }
        }
        if (!used[i]) {
            result *= selectivity(*parts[i], stats);
            used[i] = true;
        }
    }
    return result;
}

// a OR b OR ...: a + b - a*b, except that equalities on the same column cannot both be
// true, so their shares just add up.
double or_selectivity(const BoundExpr& e, const StatsLookup& stats) {
    std::vector<const BoundExpr*> parts = parts_of(e, BinaryOp::Or);
    std::vector<bool> used(parts.size(), false);
    double result = 0.0;
    auto add = [&](double s) { result = result + s - result * s; };

    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (used[i]) continue;
        auto first = column_vs_literal(*parts[i]);
        const ColumnFacts* facts = first ? stats.find(first->column) : nullptr;
        if (first && first->op == BinaryOp::Eq && facts) {
            double sum = 0;
            for (std::size_t j = i; j < parts.size(); ++j) {
                auto other = column_vs_literal(*parts[j]);
                if (used[j] || !other || other->op != BinaryOp::Eq || !(other->column == first->column)) continue;
                sum += compare_selectivity(*facts, BinaryOp::Eq, other->literal);
                used[j] = true;
            }
            add(std::min(sum, non_null(*facts)));
        } else {
            add(selectivity(*parts[i], stats));
            used[i] = true;
        }
    }
    return result;
}

double selectivity(const BoundExpr& e, const StatsLookup& stats) {
    double result = std::visit(
        [&](const auto& n) -> double {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, BoundLiteral>) {
                if (n.value.is_null()) return 0.0;
                return n.value.type() == Type::Bool ? (n.value.as_bool() ? 1.0 : 0.0) : kDefaultOther;
            } else if constexpr (std::is_same_v<T, BoundColumn>) {
                // A true/false column used as a condition: the share of rows where it is true.
                const ColumnFacts* f = stats.find(n.id);
                return f ? compare_selectivity(*f, BinaryOp::Eq, Value(true)) : kDefaultOther;
            } else if constexpr (std::is_same_v<T, BoundUnary>) {
                return n.op == UnaryOp::Not ? 1.0 - selectivity(*n.operand, stats) : kDefaultOther;
            } else if constexpr (std::is_same_v<T, BoundIsNull>) {
                const auto* column = std::get_if<BoundColumn>(&n.operand->node);
                const ColumnFacts* f = column ? stats.find(column->id) : nullptr;
                double nulls = f ? f->column->null_fraction : kDefaultIsNull;
                return n.negated ? 1.0 - nulls : nulls;
            } else {
                if (n.op == BinaryOp::And) return and_selectivity(e, stats);
                if (n.op == BinaryOp::Or) return or_selectivity(e, stats);
                if (auto cmp = column_vs_literal(e)) {
                    const ColumnFacts* f = stats.find(cmp->column);
                    return f ? compare_selectivity(*f, cmp->op, cmp->literal) : fallback_for(cmp->op);
                }
                return is_comparison(n.op) ? fallback_for(n.op) : kDefaultOther;
            }
        },
        e.node);
    return clamp01(result);
}

}  // namespace

StatsLookup StatsLookup::for_scope(const Scope& scope, const Catalog& catalog) {
    StatsLookup lookup;
    lookup.facts_.resize(scope.columns().size());
    for (const BoundTable& t : scope.tables()) {
        const Table* table = catalog.get_table(t.info->name);
        if (!table || !table->stats()) continue;
        const TableStats& stats = *table->stats();
        for (std::size_t i = 0; i < t.columns.size() && i < stats.columns.size(); ++i)
            lookup.facts_[t.columns[i].value] = ColumnFacts{&stats.columns[i], stats.row_count};
    }
    return lookup;
}

double estimate_selectivity(const BoundExpr& predicate, const StatsLookup& stats) { return selectivity(predicate, stats); }

}  // namespace cardinal
