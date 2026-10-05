#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "binder/scope.h"
#include "catalog/catalog.h"
#include "cost/cost_planner.h"
#include "logical/plan.h"
#include "optimizer/rule_optimizer.h"

namespace cardinal {

// What the planner looked at, for EXPLAIN VERBOSE.
struct VerboseInfo {
    PlanTrace trace;     // every option considered for every step; empty if the method was forced
    PhysicalPtr physical;  // the plan that will run
    Cost cost;
    std::string forced;  // set if a join method was forced, saying which; no costs were compared
};

// What EXPLAIN prints: the plan as first written, the rules that fired in order, and the plan
// after them, every line with the optimizer's guess at how many rows the step produces
// (est_rows), then which tables the guesses rest on. No trailing newline.
//   Original plan
//     Project[name]  est_rows=250
//       Filter[age > 20 + 5 AND TRUE]  est_rows=250
//         Scan[users]  est_rows=1000
//
//   Rules fired
//     1. constant-folding (pass 1)
//     2. boolean-cleanup (pass 1)
//
//   Final plan
//     Project[name]  est_rows=250
//       Filter[age > 25]  est_rows=250
//         Scan[users]  est_rows=1000
//
//   Statistics
//     users: analyzed when it had 1000 rows
//
// With `verbose`, two sections follow the final plan: every way each step could run with the
// cost of the plan below it if that way were used (the winner starred), and the physical plan
// that was chosen.
//   Ways to run it
//     ...
//   Chosen plan
//     ...
std::string format_explain(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                           const Catalog& catalog, const VerboseInfo* verbose = nullptr);

// What one step of the final plan really did when the query ran.
struct StepActual {
    std::int64_t rows = 0;
    double self_ms = 0;  // time in this step alone, not counting the steps below it
};
using StepActuals = std::unordered_map<const LogicalPlan*, StepActual>;

struct AnalyzeSummary {
    double plan_ms = 0;
    double exec_ms = 0;
    std::int64_t rows_returned = 0;
};

struct ExplainAnalyzeOutput {
    std::string text;
    double root_estimate = 0;
    std::int64_t root_actual = 0;
    double max_q_error = 1;
};

// What EXPLAIN ANALYZE prints: everything EXPLAIN does, and on each line of the final plan the
// real row count, the q-error and the time, plus a summary of the run.
//   Final plan
//     Project[name]  est_rows=234  actual_rows=211  q_error=1.11  time=0.04ms
//       Join[...]  est_rows=234  actual_rows=211  q_error=1.11  time=1.20ms
//   ...
//   Execution
//     planning 0.05 ms, execution 1.31 ms, 211 rows returned
//     worst guess: Join[...] (est 234, actual 211, q-error 1.11)
//
// With `verbose`, the final plan shows guesses only, the two verbose sections follow, and the
// real counts come last under "Actual run", so the output reads in the order: the plan as
// written, the rules, the plan after them, the options, the choice, and how the guesses did.
ExplainAnalyzeOutput format_explain_analyze(const PlanPtr& original, const OptimizeResult& result, const Scope& scope,
                                            const Catalog& catalog, const StepActuals& actuals,
                                            const AnalyzeSummary& summary, const VerboseInfo* verbose = nullptr);

}  // namespace cardinal
