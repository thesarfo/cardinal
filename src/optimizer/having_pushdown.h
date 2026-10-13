#pragma once

#include "optimizer/rule.h"

namespace cardinal {

// Moves the parts of a HAVING that read only group keys below the Aggregate, where they drop
// rows before they are grouped. A group's key is the same for every row in it, so a test on
// the key keeps or drops whole groups, the same groups as before.
//
// The filter is split at its ANDs. A part goes down if it reads at least one column and
// every column it reads is a group key's output; the key's expression takes the column's
// place. A part that reads a total stays, and so does a part that reads nothing: with no
// group keys the input is one group, and a constant HAVING keeps or drops that group even
// when it has no rows, which a filter below would not.
class HavingPushdown : public Rule {
public:
    std::string name() const override { return "having-pushdown"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

}  // namespace cardinal
