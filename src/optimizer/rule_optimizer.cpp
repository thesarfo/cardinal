#include "optimizer/rule_optimizer.h"

#include "logical/plan_printer.h"
#include "logical/plan_util.h"

namespace cardinal {

namespace {

// Rewrites `node` and everything below it. Untouched parts keep their identity, so
// "same pointer" means "nothing changed".
PlanPtr rewrite(const Rule& rule, const PlanPtr& node) {
    PlanPtr current = node;
    if (PlanPtr input = input_of(*node)) {
        PlanPtr new_input = rewrite(rule, input);
        if (new_input != input) current = with_input(*node, new_input);
    }
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
    for (int pass = 1; pass <= max_passes_; ++pass) {
        bool changed = false;
        for (const std::unique_ptr<Rule>& rule : rules_) {
            PlanPtr after = rewrite(*rule, plan);
            if (after == plan) continue;
            result.trace.push_back({rule->name(), pass, plan, after});
            plan = std::move(after);
            changed = true;
        }
        if (!changed) {
            result.plan = std::move(plan);
            return result;
        }
    }
    result.plan = std::move(plan);
    result.hit_pass_limit = true;
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
