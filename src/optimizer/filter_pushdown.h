#pragma once

#include "optimizer/rule.h"

namespace cardinal {

// Moves the parts of a filter below a join, so rows are dropped before the join sees them.
//
// The filter is split at its ANDs. Each part goes to the lowest step that has every column
// it reads: the left input, the right input, or, if it reads both, it stays above the
// join. A part that reads no column (a constant) stays too. A part pushed onto a join
// that is itself inside a bigger join moves down again on the next pass.
//
// Only inner and cross joins are touched. For an outer join, a filter on the side that
// gets NULL-filled is not the same before and after the join, so any other join type is
// refused until it has its own rule.
//
// A pushed filter now runs on rows the join would have dropped. If it can raise an error
// (1 / x > 1), the error can appear for a row that never made it past the join. The rows
// returned never change.
class FilterPushdown : public Rule {
public:
    std::string name() const override { return "filter-pushdown"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

}  // namespace cardinal
