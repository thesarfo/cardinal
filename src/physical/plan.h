#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "logical/plan.h"

namespace cardinal {

// A physical plan says how to run each step. This is the only place column
// positions appear: in every expression below, `BoundColumn::id.value` is the
// position of the value in the row the step's input produces, not a ColumnId.
struct PhysicalPlan;
using PhysicalPtr = std::shared_ptr<const PhysicalPlan>;

// Reads every row of the table in the order it was inserted.
struct PhysicalSeqScan {
    std::string table;
};
struct PhysicalEmpty {};
struct PhysicalFilter {
    PhysicalPtr input;
    BoundExprPtr predicate;
};
struct PhysicalProject {
    PhysicalPtr input;
    std::vector<ProjectItem> items;
};
// For each left row, tests every right row (the right input is read once and kept), and
// passes on left-plus-right rows for which the condition is true. Column ids in the
// condition are positions in that combined row. No condition keeps every pairing.
struct PhysicalNestedLoopJoin {
    PhysicalPtr left, right;
    BoundExprPtr condition;  // null for a cross join
};
struct PhysicalSort {
    PhysicalPtr input;
    std::vector<SortKey> keys;
};
struct PhysicalLimit {
    PhysicalPtr input;
    std::int64_t count;
};

struct PhysicalPlan {
    std::variant<PhysicalSeqScan, PhysicalEmpty, PhysicalFilter, PhysicalProject, PhysicalSort, PhysicalLimit, PhysicalNestedLoopJoin> node;
};

}  // namespace cardinal
