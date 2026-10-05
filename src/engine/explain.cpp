#include "engine/explain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "logical/plan_printer.h"
#include "logical/plan_util.h"
#include "physical/physical_printer.h"
#include "stats/analyze.h"
#include "stats/cardinality.h"
#include "stats/q_error.h"

namespace cardinal {

namespace {

std::string indented(const std::string& text, int spaces = 2) {
    std::string pad(static_cast<std::size_t>(spaces), ' ');
    std::string out = pad;
    for (char c : text) {
        out += c;
        if (c == '\n') out += pad;
    }
    return out;
}

// A step guessed to produce rows shows at least 1, so a small guess does not read as zero.
std::string estimate_text(double rows) {
    if (rows <= 0) return "0";
    return std::to_string(std::max<long long>(1, std::llround(rows)));
}

std::string first_line(const std::string& text) { return text.substr(0, text.find('\n')); }

std::string fixed(double value, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, value);
    return buf;
}

std::string cost_text(double cost) {
    if (cost == 0) return "0";
    return fixed(cost, cost < 0.01 ? 4 : 2);
}

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

// The options of every step, laid out like the plan: each step, then its ways to run below it.
void write_options(const PlanPtr& step, const Scope& scope, CardinalityEstimator& estimator,
                   const std::unordered_map<const LogicalPlan*, const StepOptions*>& by_step, std::size_t name_width,
                   int depth, std::string& out) {
    std::string indent(static_cast<std::size_t>(depth) * 2 + 2, ' ');
    out += indent + first_line(print(*step, scope)) + "  est_rows=" + estimate_text(estimator.rows(step)) + "\n";
    if (auto it = by_step.find(step.get()); it != by_step.end()) {
        for (const PlanOption& option : it->second->options) {
            out += indent + "  " + (option.chosen ? "* " : "  ") + option.name +
                   std::string(name_width - option.name.size(), ' ') + "  cost " + cost_text(option.cost.total) + "\n";
        }
    }
    for (const PlanPtr& child : children_of(*step)) write_options(child, scope, estimator, by_step, name_width, depth + 1, out);
}

std::string options_text(const PlanPtr& plan, const Scope& scope, CardinalityEstimator& estimator, const VerboseInfo& verbose) {
    if (!verbose.forced.empty())
        return "  The join method was forced (" + verbose.forced + "), so no costs were compared.\n";

    std::unordered_map<const LogicalPlan*, const StepOptions*> by_step;
    std::size_t width = 0;
    for (const StepOptions& step : verbose.trace) {
        by_step[step.step] = &step;
        for (const PlanOption& option : step.options) width = std::max(width, option.name.size());
    }
    std::string out = "  (cost is the estimated work for the step and everything below it, in made-up units; lower is better; * marks the one chosen)\n";
    write_options(plan, scope, estimator, by_step, width, 0, out);
    out += "  Total estimated cost: " + cost_text(verbose.cost.total) + "\n";
    return out;
}

std::string chosen_text(const VerboseInfo& verbose) { return indented(print(*verbose.physical)) + "\n"; }

}  // namespace

std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                           const Catalog& catalog, const VerboseInfo* verbose) {
    // One estimator for both plans: it saves what it works out, and they share steps.
    CardinalityEstimator estimator(scope, catalog);
    NodeNote note = [&](const LogicalPlan& step) { return "  est_rows=" + estimate_text(estimator.rows_of(step)); };

    std::string out = "Original plan\n" + indented(print(*original, scope, note)) + "\n\nRules fired\n" + rules_text(result);
    out += "\nFinal plan\n" + indented(print(*result.plan, scope, note)) + "\n";
    if (verbose) {
        out += "\nWays to run it\n" + options_text(result.plan, scope, estimator, *verbose);
        out += "\nChosen plan\n" + chosen_text(*verbose);
    }
    out += "\nStatistics\n" + statistics_text(scope, catalog);
    out.pop_back();
    return out;
}

ExplainAnalyzeOutput format_explain_analyze(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                                            const Catalog& catalog, const StepActuals& actuals,
                                            const AnalyzeSummary& summary, const VerboseInfo* verbose) {
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

    std::string with_real_counts = print(*result.plan, scope, with_actuals);
    output.root_estimate = estimator.rows(result.plan);
    auto root = actuals.find(result.plan.get());
    output.root_actual = root == actuals.end() ? 0 : root->second.rows;

    std::string out = "Original plan\n" + indented(print(*original, scope, estimate_only)) + "\n\nRules fired\n" + rules_text(result);
    if (verbose) {
        out += "\nFinal plan\n" + indented(print(*result.plan, scope, estimate_only));
        out += "\n\nWays to run it\n" + options_text(result.plan, scope, estimator, *verbose);
        out += "\nChosen plan\n" + chosen_text(*verbose);
        out += "\nActual run\n" + indented(with_real_counts);
    } else {
        out += "\nFinal plan\n" + indented(with_real_counts);
    }
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
