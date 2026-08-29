#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "binder/bound_expr.h"

namespace cardinal {

// A logical plan says what to do, not how. Nodes never change once built; a
// rewrite builds a new tree and shares the parts it didn't touch.
struct LogicalPlan;
using PlanPtr = std::shared_ptr<const LogicalPlan>;

// Every row of a table. `columns` are the ids this scan produces, in table order.
struct LogicalScan {
    std::string table;
    std::string alias;
    std::vector<ColumnId> columns;
};
// Rows for which the predicate is true. NULL and false are dropped.
struct LogicalFilter {
    PlanPtr input;
    BoundExprPtr predicate;
};
struct ProjectItem {
    BoundExprPtr expr;
    std::string name;  // what the result column is called
};
// Replaces each row with one value per item.
struct LogicalProject {
    PlanPtr input;
    std::vector<ProjectItem> items;
};
struct SortKey {
    BoundExprPtr expr;
    bool descending;
};
struct LogicalSort {
    PlanPtr input;
    std::vector<SortKey> keys;
};
struct LogicalLimit {
    PlanPtr input;
    std::int64_t count;
};

struct LogicalPlan {
    std::variant<LogicalScan, LogicalFilter, LogicalProject, LogicalSort, LogicalLimit> node;
};

}  // namespace cardinal
