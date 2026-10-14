#include "physical/sorted_by.h"

#include <string>

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

BoundExprPtr position(std::size_t p) {
    return std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{ColumnId{static_cast<std::uint32_t>(p)}}, std::nullopt});
}

}  // namespace

std::vector<SortKey> sorted_by(const PhysicalPlan& plan) {
    return std::visit(
        Overloaded{
            [&](const PhysicalIndexScan& n) { return std::vector<SortKey>{{position(n.column_index), false}}; },
            [&](const PhysicalSort& n) { return n.keys; },
            [&](const PhysicalFilter& n) { return sorted_by(*n.input); },
            [&](const PhysicalLimit& n) { return sorted_by(*n.input); },
            [&](const PhysicalNestedLoopJoin& n) { return sorted_by(*n.left); },
            [&](const PhysicalMergeJoin& n) { return sorted_by(*n.left); },
            [&](const PhysicalHashJoin& n) { return n.build_left ? std::vector<SortKey>{} : sorted_by(*n.left); },
            [&](const PhysicalSortAggregate& n) {
                std::vector<SortKey> out;
                for (std::size_t i = 0; i < n.keys.size(); ++i) out.push_back({position(i), false});
                return out;
            },
            [&](const PhysicalProject& n) {
                // A key survives if the project passes it through as one of its output columns.
                std::vector<SortKey> out;
                for (const SortKey& key : sorted_by(*n.input)) {
                    const auto* column = std::get_if<BoundColumn>(&key.expr->node);
                    if (!column) break;
                    bool found = false;
                    for (std::size_t i = 0; i < n.items.size() && !found; ++i) {
                        const auto* item = std::get_if<BoundColumn>(&n.items[i].expr->node);
                        if (item && item->id == column->id) {
                            out.push_back({position(i), key.descending});
                            found = true;
                        }
                    }
                    if (!found) break;
                }
                return out;
            },
            [&](const auto&) { return std::vector<SortKey>{}; },
        },
        plan.node);
}

bool satisfies(const std::vector<SortKey>& have, const std::vector<SortKey>& want) {
    if (want.size() > have.size()) return false;
    for (std::size_t i = 0; i < want.size(); ++i)
        if (have[i].descending != want[i].descending || !expr_equal(*have[i].expr, *want[i].expr)) return false;
    return true;
}

std::string describe_order(const std::vector<SortKey>& order) {
    std::string out;
    for (const SortKey& key : order) {
        std::string text = "?";
        if (const auto* column = std::get_if<BoundColumn>(&key.expr->node)) text = "#" + std::to_string(column->id.value);
        out += (out.empty() ? "" : ", ") + text + (key.descending ? " DESC" : "");
    }
    return out;
}

}  // namespace cardinal
