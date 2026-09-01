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
struct PhysicalFilter {
    PhysicalPtr input;
    BoundExprPtr predicate;
};
struct PhysicalProject {
    PhysicalPtr input;
    std::vector<ProjectItem> items;
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
    std::variant<PhysicalSeqScan, PhysicalFilter, PhysicalProject, PhysicalSort, PhysicalLimit> node;
};

}  // namespace cardinal
