#include "physical/index_match.h"

#include <algorithm>

#include "optimizer/conjuncts.h"

namespace cardinal {

namespace {

// `column op constant`, or `constant op column` turned around, where op is a comparison.
struct Comparison {
    ColumnId column;
    BinaryOp op;
    Value value;
};

std::optional<Comparison> as_comparison(const BoundExprPtr& expr) {
    const auto* b = std::get_if<BoundBinary>(&expr->node);
    if (!b) return std::nullopt;
    BinaryOp flipped;
    switch (b->op) {
        case BinaryOp::Eq: flipped = BinaryOp::Eq; break;
        case BinaryOp::Lt: flipped = BinaryOp::Gt; break;
        case BinaryOp::Le: flipped = BinaryOp::Ge; break;
        case BinaryOp::Gt: flipped = BinaryOp::Lt; break;
        case BinaryOp::Ge: flipped = BinaryOp::Le; break;
        default: return std::nullopt;
    }
    const auto* lc = std::get_if<BoundColumn>(&b->left->node);
    const auto* rl = std::get_if<BoundLiteral>(&b->right->node);
    if (lc && rl && !rl->value.is_null()) return Comparison{lc->id, b->op, rl->value};
    const auto* ll = std::get_if<BoundLiteral>(&b->left->node);
    const auto* rc = std::get_if<BoundColumn>(&b->right->node);
    if (ll && rc && !ll->value.is_null()) return Comparison{rc->id, flipped, ll->value};
    return std::nullopt;
}

}  // namespace

std::optional<IndexMatch> match_index(const LogicalFilter& filter, const Catalog& catalog) {
    const auto* prune = std::get_if<LogicalPrune>(&filter.input->node);
    const auto* scan = std::get_if<LogicalScan>(prune ? &prune->input->node : &filter.input->node);
    if (!scan) return std::nullopt;
    const Table* table = catalog.get_table(scan->table);
    if (!table || table->indexes().empty()) return std::nullopt;

    std::vector<BoundExprPtr> conjuncts = split_ands(filter.predicate);
    for (const BoundExprPtr& first : conjuncts) {
        std::optional<Comparison> head = as_comparison(first);
        if (!head) continue;
        auto at = std::find(scan->columns.begin(), scan->columns.end(), head->column);
        if (at == scan->columns.end()) continue;
        const std::string& column = table->info().columns[static_cast<std::size_t>(at - scan->columns.begin())].name;
        const Table::IndexEntry* entry = table->index_on(column);
        if (!entry) continue;

        IndexMatch match;
        match.scan = scan;
        match.scan_node = prune ? prune->input.get() : filter.input.get();
        match.prune = prune;
        for (const auto& [name, e] : table->indexes())
            if (&e == entry) match.index = name;
        match.column = column;
        bool exact = false;
        for (const BoundExprPtr& conjunct : conjuncts) {
            std::optional<Comparison> c = as_comparison(conjunct);
            bool used = false;
            if (c && c->column == head->column && !exact) {
                if (c->op == BinaryOp::Eq && !match.lo && !match.hi) {
                    match.lo = Bound{c->value, true};
                    match.hi = Bound{c->value, true};
                    exact = used = true;
                } else if ((c->op == BinaryOp::Gt || c->op == BinaryOp::Ge) && !match.lo) {
                    match.lo = Bound{c->value, c->op == BinaryOp::Ge};
                    used = true;
                } else if ((c->op == BinaryOp::Lt || c->op == BinaryOp::Le) && !match.hi) {
                    match.hi = Bound{c->value, c->op == BinaryOp::Le};
                    used = true;
                }
            }
            (used ? match.answered : match.rest).push_back(conjunct);
        }
        // An equality found after a bound was taken is left in `rest`, which is still correct.
        if (match.answered.empty()) continue;
        return match;
    }
    return std::nullopt;
}

}  // namespace cardinal
