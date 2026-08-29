#include "logical/planner.h"

#include <cassert>

namespace cardinal {

namespace {

template <class Node>
PlanPtr make(Node node) {
    return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)});
}

}  // namespace

PlanPtr plan_select(const BoundSelect& select) {
    // The parser has no joins yet, so there is exactly one table.
    assert(select.scope.tables().size() == 1);
    const BoundTable& table = select.scope.tables().front();

    PlanPtr plan = make(LogicalScan{table.info->name, table.alias, table.columns});
    if (select.where) plan = make(LogicalFilter{plan, select.where});

    if (!select.order_by.empty()) {
        std::vector<SortKey> keys;
        for (const BoundOrderKey& key : select.order_by) keys.push_back({key.expr, key.descending});
        plan = make(LogicalSort{plan, std::move(keys)});
    }

    std::vector<ProjectItem> items;
    for (const BoundSelectItem& item : select.items) items.push_back({item.expr, item.name});
    plan = make(LogicalProject{plan, std::move(items)});

    if (select.limit) plan = make(LogicalLimit{plan, *select.limit});
    return plan;
}

}  // namespace cardinal
