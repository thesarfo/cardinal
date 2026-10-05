#pragma once

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "common/value.h"
#include "optimizer/default_rules.h"
#include <memory>
#include <optional>

#include "cost/cost_model.h"
#include "cost/cost_planner.h"
#include "engine/explain.h"
#include "optimizer/rule_optimizer.h"
#include "physical/physical_planner.h"

namespace cardinal {

// What one operator of the running plan did.
struct OperatorStat {
    std::string name;  // "SeqScan", "HashJoin", ...
    std::uint64_t rows_out = 0;
    std::uint64_t rows_scanned = 0;  // see ExecStats
    double self_ms = 0;              // time in this operator alone
    std::vector<std::uint64_t> input_rows;  // rows each of its inputs handed it, in order
};

// What running a SELECT cost. Filled in for SELECT only.
struct QueryStats {
    double plan_ms = 0;  // bind, plan, optimize and build the operators; parsing is not counted
    double exec_ms = 0;  // pulling every row through the operators
    // Rows every operator handed up, plus the row pairs joins tested: a measure of work
    // that does not depend on the machine.
    std::uint64_t rows_processed = 0;
    std::string plan_hash;  // of the final physical plan; equal hashes mean equal plans
    std::vector<OperatorStat> operators;  // every operator, the root first, then its inputs depth first
};

struct QueryResult {
    // Set for SELECT, with one name per output column (even when there are no rows).
    std::vector<std::string> columns;
    std::vector<Row> rows;
    // Set for statements that return no rows: "CREATE TABLE", "INSERT 3".
    std::string message;
    QueryStats stats;

    bool returns_rows() const { return !columns.empty(); }
};

// Runs SQL end to end: parse, bind, plan, execute. The shell and the test-file
// runner both go through this.
class Database {
public:
    using Stages = std::vector<std::vector<std::unique_ptr<Rule>>>;

    Database() { rebuild_optimizer(); }

    // Runs one statement. Throws ParseError for SQL it can't read and DbError for SQL
    // it can't carry out. A statement that throws changes nothing.
    QueryResult execute(std::string_view sql);
    QueryResult execute(const Statement& statement);

    // What the cost-based planner thinks of a SELECT: the cost of the plan it would choose, and every
    // option it priced. The `step` pointers in the trace are not valid after this returns.
    CostBasedPlan cost_of(std::string_view sql);

    // EXPLAIN ANALYZE for a SELECT: runs it, and returns the text along with the numbers
    // behind it (the root's estimate and real row count, and the worst q-error of any step).
    ExplainAnalyzeOutput explain_analyze(std::string_view sql);

    const Catalog& catalog() const { return catalog_; }
    // For loading data without going through SQL. Null if there is no such table.
    Table* table(const std::string& name) { return catalog_.get_table(name); }

    // On by default. Turning it off runs plans exactly as the query was written, which
    // is how tests check that the optimizer never changes an answer.
    void set_optimizer_enabled(bool enabled) { optimize_ = enabled; }
    bool optimizer_enabled() const { return optimize_; }

    // Names of the rules in use, for switching them off one at a time.
    std::vector<std::string> rule_names() const;
    // Rules with these names are left out. Empty by default.
    void set_disabled_rules(std::set<std::string> names);

    // Joins are run by whichever method the cost model prices cheaper. To force one instead
    // (all hash, say, or all nested loop), set planner options; the rows are the same either
    // way. use_cost_based_planner() goes back to choosing.
    void set_planner_options(PlannerOptions options) { forced_planner_ = std::move(options); }
    void use_cost_based_planner() { forced_planner_.reset(); }
    const std::optional<PlannerOptions>& forced_planner() const { return forced_planner_; }

    // Swap the cost model, for experiments. The default is DefaultCostModel.
    void set_cost_model(std::unique_ptr<CostModel> model) { cost_model_ = std::move(model); }
    const CostModel& cost_model() const { return *cost_model_; }

    // Replaces the rule set (the default is default_stages). For tests that want to try
    // a rule of their own, such as one that is deliberately wrong.
    void set_rule_factory(std::function<Stages()> factory);

private:
    void rebuild_optimizer();

    Catalog catalog_;
    std::function<Stages()> rule_factory_ = default_stages;
    std::set<std::string> disabled_rules_;
    RuleOptimizer optimizer_{{}};
    bool optimize_ = true;
    std::optional<PlannerOptions> forced_planner_;
    std::unique_ptr<CostModel> cost_model_ = std::make_unique<DefaultCostModel>();
};

}  // namespace cardinal
