#include "engine/explain.h"

#include <cmath>
#include <set>

#include "logical/plan_printer.h"
#include "stats/analyze.h"
#include "stats/cardinality.h"

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

// A step guessed to produce rows shows at least 1, so a small guess does not read as zero.
std::string estimate_text(double rows) {
    if (rows <= 0) return "0";
    return std::to_string(std::max<long long>(1, std::llround(rows)));
}

}  // namespace

std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                           const Catalog& catalog) {
    // One estimator for both plans: it saves what it works out, and they share steps.
    CardinalityEstimator estimator(scope, catalog);
    NodeNote note = [&](const LogicalPlan& step) { return "  est_rows=" + estimate_text(estimator.rows_of(step)); };

    std::string out = "Original plan\n" + indented(print(*original, scope, note)) + "\n\nRules fired\n";
    if (result.trace.empty()) {
        out += "  none\n";
    } else {
        int n = 0;
        for (const RuleFiring& firing : result.trace)
            out += "  " + std::to_string(++n) + ". " + firing.rule + " (pass " + std::to_string(firing.pass) + ")\n";
    }
    if (result.hit_pass_limit) out += "  stopped at the pass limit; the plan may not be finished\n";
    out += "\nFinal plan\n" + indented(print(*result.plan, scope, note));

    out += "\n\nStatistics\n";
    std::set<std::string> seen;
    for (const BoundTable& t : scope.tables()) {
        const std::string& name = t.info->name;
        if (!seen.insert(name).second) continue;
        const Table* table = catalog.get_table(name);
        if (!table || !table->stats()) {
            out += "  " + name + ": not analyzed, so the guesses use default numbers\n";
            continue;
        }
        out += "  " + name + ": analyzed when it had " + std::to_string(table->stats()->row_count) + " rows\n";
        if (stats_are_stale(*table, *table->stats()))
            out += "  warning: " + name + " has " + std::to_string(table->rows().size()) + " rows now; run ANALYZE " + name + "\n";
    }
    out.pop_back();
    return out;
}

}  // namespace cardinal
