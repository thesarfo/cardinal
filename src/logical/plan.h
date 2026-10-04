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
enum class JoinType { Inner, Cross };


// No rows at all, without reading anything. `columns` are the ids the step it replaced
// would have produced, so the steps above can still find their columns.
struct LogicalEmpty {
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
// Keeps only these columns, in this order, and drops the rest. Unlike a Project it
// still produces columns (with the same ids), not computed values, so the steps above
// can keep reading them. It makes rows narrower before a join copies them.
struct LogicalPrune {
    PlanPtr input;
    std::vector<ColumnId> columns;
};
struct LogicalLimit {
    PlanPtr input;
    std::int64_t count;
};
// Every pairing of a left row and a right row for which the condition is true. A Cross
// join has no condition and keeps every pairing. Its rows are the left row's values
// followed by the right row's.
struct LogicalJoin {
    PlanPtr left, right;
    JoinType type;
    BoundExprPtr condition;  // null for Cross
};

struct LogicalPlan {
    std::variant<LogicalScan, LogicalEmpty, LogicalFilter, LogicalProject, LogicalSort, LogicalLimit, LogicalJoin, LogicalPrune> node;
};

}  // namespace cardinal
