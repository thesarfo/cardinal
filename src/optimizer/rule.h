#pragma once

#include <optional>
#include <string>

#include "logical/plan.h"

namespace cardinal {

// One rewrite: a way to turn a plan into a different plan that gives the same rows.
// Rules never look at costs or table sizes; each one is a move that is always safe.
class Rule {
public:
    virtual ~Rule() = default;

    // Short and stable, like "constant-folding". It shows up in the trace.
    virtual std::string name() const = 0;

    // Looks at one node. Its inputs have already been rewritten by this rule in the
    // current pass. Returns the replacement, or nothing if the node is fine as it is.
    // Plans are immutable: build a new node, don't change `node`.
    virtual std::optional<PlanPtr> apply(const PlanPtr& node) const = 0;
};

}  // namespace cardinal
