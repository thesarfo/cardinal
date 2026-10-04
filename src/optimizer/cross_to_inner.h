#pragma once

#include "optimizer/rule.h"

namespace cardinal {

// A cross join with a filter on top that says `a.x = b.y` is an inner join in disguise.
// `FROM a, b WHERE a.x = b.y` becomes the same plan as `FROM a JOIN b ON a.x = b.y`.
//
// It fires when the filter has at least one equality whose two sides each read only one
// input of the join (one the left, one the right). Then every part of the filter that
// reads both inputs moves into the join's condition. Parts that read one input stay in
// the filter, for filter pushdown to move down.
class CrossToInnerJoin : public Rule {
public:
    std::string name() const override { return "cross-to-inner-join"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

}  // namespace cardinal
