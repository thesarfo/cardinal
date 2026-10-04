#pragma once

#include <set>
#include <vector>

#include "binder/bound_expr.h"

namespace cardinal {

// The parts of `a AND b AND c`, in order, whichever way the ANDs are nested.
std::vector<BoundExprPtr> split_ands(const BoundExprPtr& expr);

// The parts joined back with AND, left to right. `parts` must not be empty.
BoundExprPtr and_all(const std::vector<BoundExprPtr>& parts);

// True if every column in `used` is among `available`.
bool columns_within(const std::set<ColumnId>& used, const std::vector<ColumnId>& available);

}  // namespace cardinal
