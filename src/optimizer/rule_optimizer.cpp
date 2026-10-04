#include "optimizer/rule_optimizer.h"

#include "logical/plan_printer.h"
#include "logical/plan_util.h"

namespace cardinal {

namespace {

// Rewrites `node` and everything below it. Untouched parts keep their identity, so
// "same pointer" means "nothing changed".
PlanPtr rewrite(const Rule& rule, const PlanPtr& node) {
    PlanPtr current = node;
    std::vector<PlanPtr> children = children_of(*node);
    bool child_changed = false;
    for (PlanPtr& child : children) {
        PlanPtr rewritten = rewrite(rule, child);
        if (rewritten != child) {
            child = std::move(rewritten);
            child_changed = true;
        }
    }
    if (child_changed) current = with_children(*node, std::move(children));
    if (std::optional<PlanPtr> replaced = rule.apply(current)) {
        if (*replaced != current) return *replaced;
    }
    return current;
}

std::string indent_block(const std::string& text, const std::string& indent) {
    std::string out = indent;
    for (char c : text) {
        out += c;
        if (c == '\n') out += indent;
    }
    return out;
}

}  // namespace

OptimizeResult RuleOptimizer::optimize(PlanPtr plan) const {
    OptimizeResult result;
    int pass_number = 1;
    for (const std::vector<std::unique_ptr<Rule>>& stage : stages_) {
        bool settled = false;
        for (int pass = 1; pass <= max_passes_ && !settled; ++pass) {
            bool changed = false;
            for (const std::unique_ptr<Rule>& rule : stage) {
                PlanPtr after = rewrite(*rule, plan);
                if (after == plan) continue;
                result.trace.push_back({rule->name(), pass_number, plan, after});
                plan = std::move(after);
                changed = true;
            }
            if (changed) ++pass_number;
            settled = !changed;
        }
        if (!settled) result.hit_pass_limit = true;
    }
    result.plan = std::move(plan);
    return result;
}

std::string format_trace(const RuleTrace& trace, const Scope& scope) {
    if (trace.empty()) return "no rules fired";
    std::string out;
    int n = 0;
    for (const RuleFiring& firing : trace) {
        if (n > 0) out += "\n";
        out += std::to_string(++n) + ". " + firing.rule + " (pass " + std::to_string(firing.pass) + ")\n";
        out += "   before:\n" + indent_block(print(*firing.before, scope), "     ") + "\n";
        out += "   after:\n" + indent_block(print(*firing.after, scope), "     ");
        if (n < static_cast<int>(trace.size())) out += "\n";
    }
    return out;
}

}  // namespace cardinal
