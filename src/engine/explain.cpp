#include "engine/explain.h"

#include <cmath>
#include <cstdio>
#include <set>

#include "logical/plan_printer.h"
#include "stats/analyze.h"
#include "stats/cardinality.h"
#include "stats/q_error.h"

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

namespace {

struct Sections {
    std::string original, rules, statistics;
};

std::string rules_text(const OptimizeResult& result) {
    std::string out;
    if (result.trace.empty()) {
        out += "  none\n";
    } else {
        int n = 0;
        for (const RuleFiring& firing : result.trace)
            out += "  " + std::to_string(++n) + ". " + firing.rule + " (pass " + std::to_string(firing.pass) + ")\n";
    }
    if (result.hit_pass_limit) out += "  stopped at the pass limit; the plan may not be finished\n";
    return out;
}

std::string statistics_text(const Scope& scope, const Catalog& catalog) {
    std::string out;
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
    return out;
}

std::string first_line(const std::string& text) { return text.substr(0, text.find('\n')); }

std::string fixed(double value, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, value);
    return buf;
}

}  // namespace

std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                           const Catalog& catalog) {
    // One estimator for both plans: it saves what it works out, and they share steps.
    CardinalityEstimator estimator(scope, catalog);
    NodeNote note = [&](const LogicalPlan& step) { return "  est_rows=" + estimate_text(estimator.rows_of(step)); };

    std::string out = "Original plan\n" + indented(print(*original, scope, note)) + "\n\nRules fired\n" + rules_text(result);
    out += "\nFinal plan\n" + indented(print(*result.plan, scope, note));
    out += "\n\nStatistics\n" + statistics_text(scope, catalog);
    out.pop_back();
    return out;
}

ExplainAnalyzeOutput format_explain_analyze(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                                            const Catalog& catalog, const StepActuals& actuals,
                                            const AnalyzeSummary& summary) {
    CardinalityEstimator estimator(scope, catalog);
    NodeNote estimate_only = [&](const LogicalPlan& step) { return "  est_rows=" + estimate_text(estimator.rows_of(step)); };

    ExplainAnalyzeOutput output;
    const LogicalPlan* worst_step = nullptr;
    double worst_estimate = 0, worst_actual = 0;
    NodeNote with_actuals = [&](const LogicalPlan& step) {
        double estimate = estimator.rows_of(step);
        auto it = actuals.find(&step);
        if (it == actuals.end()) return "  est_rows=" + estimate_text(estimate);
        double error = q_error(estimate, static_cast<double>(it->second.rows));
        if (!worst_step || error >= output.max_q_error - 1e-9) {
            worst_step = &step;
            output.max_q_error = error;
            worst_estimate = estimate;
            worst_actual = static_cast<double>(it->second.rows);
        }
        return "  est_rows=" + estimate_text(estimate) + "  actual_rows=" + std::to_string(it->second.rows) +
               "  q_error=" + fixed(error, 2) + "  time=" + fixed(it->second.self_ms, 3) + "ms";
    };

    std::string final_plan = print(*result.plan, scope, with_actuals);
    output.root_estimate = estimator.rows(result.plan);
    auto root = actuals.find(result.plan.get());
    output.root_actual = root == actuals.end() ? 0 : root->second.rows;

    std::string out = "Original plan\n" + indented(print(*original, scope, estimate_only)) + "\n\nRules fired\n" + rules_text(result);
    out += "\nFinal plan\n" + indented(final_plan);
    out += "\n\nExecution\n  planning " + fixed(summary.plan_ms, 2) + " ms, execution " + fixed(summary.exec_ms, 2) + " ms, " +
           std::to_string(summary.rows_returned) + (summary.rows_returned == 1 ? " row" : " rows") + " returned\n";
    if (worst_step) {
        out += "  worst guess: " + first_line(print(*worst_step, scope)) + " (est " + estimate_text(worst_estimate) + ", actual " +
               std::to_string(static_cast<long long>(worst_actual)) + ", q-error " + fixed(output.max_q_error, 2) + ")\n";
    }
    out += "\nStatistics\n" + statistics_text(scope, catalog);
    out.pop_back();
    output.text = std::move(out);
    return output;
}

}  // namespace cardinal
