#pragma once

#include <memory>
#include <string>
#include <vector>

#include "binder/scope.h"
#include "logical/plan.h"
#include "optimizer/rule.h"

namespace cardinal {

// One time a rule changed the plan: which rule, and the whole plan on either side.
struct RuleFiring {
    std::string rule;
    int pass;  // starting at 1
    PlanPtr before;
    PlanPtr after;
};
using RuleTrace = std::vector<RuleFiring>;

struct OptimizeResult {
    PlanPtr plan;
    RuleTrace trace;
    // True if the last allowed pass was still changing the plan, so it may not be finished.
    bool hit_pass_limit = false;
};

// Applies the rules to every node, bottom up, pass after pass, until a whole pass
// changes nothing. Rules run in the order given within a pass. A pass limit keeps a
// pair of rules that undo each other from looping forever.
class RuleOptimizer {
public:
    static constexpr int kDefaultMaxPasses = 20;

    explicit RuleOptimizer(std::vector<std::unique_ptr<Rule>> rules, int max_passes = kDefaultMaxPasses)
        : rules_(std::move(rules)), max_passes_(max_passes) {}

    OptimizeResult optimize(PlanPtr plan) const;

private:
    std::vector<std::unique_ptr<Rule>> rules_;
    int max_passes_;
};

// The trace as text, one firing after another:
//   1. constant-folding (pass 1)
//      before:
//        Filter[...]
//      after:
//        Filter[...]
// `scope` supplies column names. Says "no rules fired" for an empty trace.
std::string format_trace(const RuleTrace& trace, const Scope& scope);

}  // namespace cardinal
