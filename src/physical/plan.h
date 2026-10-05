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
// An equality join through a lookup table (see exec/hash_join.h). `left_keys` read the left
// input's rows and `right_keys` the right input's, each by position in its own row. `residual`
// is the rest of the condition, over the combined row. `build_left` says which input goes in
// the table; the output is left values then right values either way.
struct PhysicalHashJoin {
    PhysicalPtr left, right;
    std::vector<BoundExprPtr> left_keys, right_keys;
    BoundExprPtr residual;  // null if the keys are the whole condition
    bool build_left;
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
    std::variant<PhysicalSeqScan, PhysicalEmpty, PhysicalFilter, PhysicalProject, PhysicalSort, PhysicalLimit, PhysicalNestedLoopJoin, PhysicalHashJoin> node;
};

}  // namespace cardinal
