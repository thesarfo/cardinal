#include "engine/explain.h"

#include "logical/plan_printer.h"

namespace cardinal {

namespace {

std::string indented(const std::string& text) {
    std::string out = "  ";
    for (char c : text) {
        out += c;
        if (c == '\n') out += "  ";
    }
    return out;
}

}  // namespace

std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope) {
    std::string out = "Original plan\n" + indented(print(*original, scope)) + "\n\nRules fired\n";
    if (result.trace.empty()) {
        out += "  none\n";
    } else {
        int n = 0;
        for (const RuleFiring& firing : result.trace)
            out += "  " + std::to_string(++n) + ". " + firing.rule + " (pass " + std::to_string(firing.pass) + ")\n";
    }
    if (result.hit_pass_limit) out += "  stopped at the pass limit; the plan may not be finished\n";
    out += "\nFinal plan\n" + indented(print(*result.plan, scope));
    return out;
}

}  // namespace cardinal
