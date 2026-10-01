#include "logical/planner.h"

namespace cardinal {

namespace {

template <class Node>
PlanPtr make(Node node) {
    return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)});
}

}  // namespace

PlanPtr plan_select(const BoundSelect& select) {
    auto scan = [&](const BoundTable& t) { return make(LogicalScan{t.info->name, t.alias, t.columns}); };
    const std::vector<BoundTable>& tables = select.scope.tables();

    // Joins read left to right: ((t0 join t1) join t2) ...
    PlanPtr plan = scan(tables.front());
    for (std::size_t i = 0; i < select.joins.size(); ++i) {
        const BoundExprPtr& condition = select.joins[i].condition;
        plan = make(LogicalJoin{plan, scan(tables[i + 1]), condition ? JoinType::Inner : JoinType::Cross, condition});
    }
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
