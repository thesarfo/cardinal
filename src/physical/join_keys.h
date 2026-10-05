#pragma once

#include <vector>

#include "binder/bound_expr.h"

namespace cardinal {

// A join condition taken apart for a hash join.
struct JoinKeys {
    // Each pair is one equality `left_expr = right_expr`, with the left expression reading
    // only the left input's columns and the right one only the right input's.
    std::vector<std::pair<BoundExprPtr, BoundExprPtr>> keys;
    // Everything else in the condition. Checked on the rows the keys bring together.
    std::vector<BoundExprPtr> residual;
};

// Splits `condition` at its ANDs. An equality whose two sides each belong to one input becomes a
// key (turned around if it was written right = left). Anything else is left over. If `keys`
// comes back empty, a hash join cannot be used.
JoinKeys split_join_condition(const BoundExprPtr& condition, const std::vector<ColumnId>& left_columns,
                              const std::vector<ColumnId>& right_columns);

}  // namespace cardinal
