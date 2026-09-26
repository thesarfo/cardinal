#pragma once

#include "optimizer/rule.h"

namespace cardinal {

// Filter[p1] over Filter[p2] over X  ->  Filter[p2 AND p1] over X.
// The lower filter's condition goes first, since that is the one that ran first.
class MergeFilters : public Rule {
public:
    std::string name() const override { return "merge-filters"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

// Filter[TRUE] over X  ->  X.
class RemoveTrueFilter : public Rule {
public:
    std::string name() const override { return "remove-true-filter"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

// Filter[FALSE] over X  ->  Empty, which reads nothing.
// A NULL condition keeps no rows either, so Filter[NULL] gets the same treatment.
class EmptyFalseFilter : public Rule {
public:
    std::string name() const override { return "empty-false-filter"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

}  // namespace cardinal
