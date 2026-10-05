#include "engine/database.h"

#include <chrono>
#include <cstdio>
#include <functional>

#include "binder/binder.h"
#include "common/error.h"
#include "engine/explain.h"
#include "cost/cost_planner.h"
#include "exec/build.h"
#include "logical/plan_util.h"
#include "expr/evaluator.h"
#include "logical/planner.h"
#include "physical/physical_planner.h"
#include "physical/physical_printer.h"
#include "stats/analyze.h"
#include "sql/parser.h"

namespace cardinal {

namespace {

QueryResult create_table(Catalog& catalog, const CreateTable& stmt) {
    TableInfo info{stmt.name, {}};
    for (const ColumnDef& column : stmt.columns) info.columns.push_back({column.name, column.type});
    catalog.create_table(std::move(info));
    return {{}, {}, "CREATE TABLE", {}};
}

QueryResult insert(Catalog& catalog, const Insert& stmt) {
    Table* table = catalog.get_table(stmt.table);
    if (!table) throw DbError("unknown table " + stmt.table);

    // Values are constants: bind against no columns, then work each one out.
    Scope no_columns;
    std::vector<Row> rows;
    for (const std::vector<ExprPtr>& values : stmt.rows) {
        Row row;
        for (const ExprPtr& value : values)
            row.push_back(evaluate(*bind_expression(*value, no_columns), {}));
        rows.push_back(std::move(row));
    }
    std::size_t count = rows.size();
    table->insert_rows(std::move(rows));
    return {{}, {}, "INSERT " + std::to_string(count), {}};
}

std::uint64_t work_done(const Operator& op) {
    std::uint64_t total = op.stats().rows_out + op.stats().rows_scanned;
    for (const Operator* child : op.children()) total += work_done(*child);
    return total;
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

QueryResult analyze(Catalog& catalog, const Analyze& stmt) {
    Table* table = catalog.get_table(stmt.table);
    if (!table) throw DbError("unknown table " + stmt.table);
    table->set_stats(analyze_table(*table));
    return {{}, {}, "ANALYZE " + stmt.table + " (" + std::to_string(table->rows().size()) + " rows)", {}};
}

std::string describe_forced(const PlannerOptions& options) {
    if (options.join_method == JoinMethod::NestedLoop) return "nested loop joins";
    return std::string("hash joins, building from the ") + (options.build_left ? "left" : "right") + " input";
}

// Chooses how to run each step: the forced options if there are any, otherwise by cost, and
// keeps what the planner looked at.
VerboseInfo choose_physical(const Catalog& catalog, const BoundSelect& bound, const PlanPtr& logical,
                            const std::optional<PlannerOptions>& forced, const CostModel& model) {
    VerboseInfo info;
    if (forced) {
        info.physical = plan_physical(*logical, *forced);
        info.forced = describe_forced(*forced);
        return info;
    }
    CardinalityEstimator estimator(bound.scope, catalog);
    CostBasedPlan planned = plan_by_cost(logical, estimator, model);
    info.physical = planned.physical;
    info.cost = planned.cost;
    info.trace = std::move(planned.trace);
    return info;
}

QueryResult select(const Catalog& catalog, const RuleOptimizer& optimizer, bool optimize,
                   const std::optional<PlannerOptions>& forced, const CostModel& model, const Select& stmt) {
    auto planning_started = std::chrono::steady_clock::now();
    BoundSelect bound = bind_select(stmt, catalog);
    PlanPtr logical = plan_select(bound);
    if (optimize) logical = optimizer.optimize(logical).plan;
    PhysicalPtr plan = choose_physical(catalog, bound, logical, forced, model).physical;
    std::unique_ptr<Operator> root = build_operator(*plan, catalog);

    QueryResult result;
    result.stats.plan_ms = milliseconds_since(planning_started);
    for (const BoundSelectItem& item : bound.items) result.columns.push_back(item.name);

    auto execution_started = std::chrono::steady_clock::now();
    while (std::optional<Row> row = root->next()) result.rows.push_back(std::move(*row));
    result.stats.exec_ms = milliseconds_since(execution_started);

    result.stats.rows_processed = work_done(*root);
    char hash[17];
    std::snprintf(hash, sizeof hash, "%016zx", std::hash<std::string>{}(print(*plan)));
    result.stats.plan_hash = hash;
    return result;
}

}  // namespace

QueryResult Database::execute(std::string_view sql) { return execute(parse_statement(sql)); }

std::vector<std::string> Database::rule_names() const {
    std::vector<std::string> names;
    for (const auto& stage : rule_factory_())
        for (const auto& rule : stage) names.push_back(rule->name());
    return names;
}

void Database::set_disabled_rules(std::set<std::string> names) {
    disabled_rules_ = std::move(names);
    rebuild_optimizer();
}

void Database::set_rule_factory(std::function<Stages()> factory) {
    rule_factory_ = std::move(factory);
    rebuild_optimizer();
}

void Database::rebuild_optimizer() {
    Stages stages;
    for (auto& stage : rule_factory_()) {
        std::vector<std::unique_ptr<Rule>> kept;
        for (auto& rule : stage)
            if (!disabled_rules_.count(rule->name())) kept.push_back(std::move(rule));
        stages.push_back(std::move(kept));
    }
    optimizer_ = RuleOptimizer::staged(std::move(stages));
}

namespace {

struct Optimized {
    PlanPtr original;
    OptimizeResult result;
};

Optimized optimize_for_explain(const PlanPtr& original, const RuleOptimizer& optimizer, bool optimize) {
    if (!optimize) return {original, OptimizeResult{original, {}, false}};
    return {original, optimizer.optimize(original)};
}

double to_ms(std::chrono::nanoseconds t) { return std::chrono::duration<double, std::milli>(t).count(); }

// The steps of the logical plan and the operators built from it have the same shape, so
// they can be walked together. A step's own time is what is left after taking off its inputs'.
void collect_actuals(const LogicalPlan& step, const Operator& op, StepActuals& out) {
    double self = to_ms(op.stats().time);
    std::vector<PlanPtr> inputs = children_of(step);
    std::vector<const Operator*> operators = op.children();
    for (std::size_t i = 0; i < inputs.size() && i < operators.size(); ++i) {
        self -= to_ms(operators[i]->stats().time);
        collect_actuals(*inputs[i], *operators[i], out);
    }
    out[&step] = {static_cast<std::int64_t>(op.stats().rows_out), std::max(0.0, self)};
}

ExplainAnalyzeOutput explain_analyze_select(const Catalog& catalog, const RuleOptimizer& optimizer, bool optimize,
                                            const std::optional<PlannerOptions>& forced, const CostModel& model,
                                            bool verbose, const Select& stmt) {
    auto planning_started = std::chrono::steady_clock::now();
    BoundSelect bound = bind_select(stmt, catalog);
    Optimized optimized = optimize_for_explain(plan_select(bound), optimizer, optimize);
    VerboseInfo chosen = choose_physical(catalog, bound, optimized.result.plan, forced, model);
    std::unique_ptr<Operator> root = build_operator(*chosen.physical, catalog);
    AnalyzeSummary summary;
    summary.plan_ms = milliseconds_since(planning_started);

    auto execution_started = std::chrono::steady_clock::now();
    while (root->next()) ++summary.rows_returned;
    summary.exec_ms = milliseconds_since(execution_started);

    StepActuals actuals;
    collect_actuals(*optimized.result.plan, *root, actuals);
    return format_explain_analyze(optimized.original, optimized.result, bound.scope, catalog, actuals, summary,
                                  verbose ? &chosen : nullptr);
}

}  // namespace

ExplainAnalyzeOutput Database::explain_analyze(std::string_view sql) {
    Statement statement = parse_statement(sql);
    const auto* select_stmt = std::get_if<Select>(&statement.node);
    if (!select_stmt) throw DbError("EXPLAIN ANALYZE only works on SELECT");
    return explain_analyze_select(catalog_, optimizer_, optimize_, forced_planner_, *cost_model_, false, *select_stmt);
}

QueryResult Database::execute(const Statement& statement) {
    if (const auto* s = std::get_if<CreateTable>(&statement.node)) return create_table(catalog_, *s);
    if (const auto* s = std::get_if<Insert>(&statement.node)) return insert(catalog_, *s);
    if (const auto* s = std::get_if<Select>(&statement.node)) return select(catalog_, optimizer_, optimize_, forced_planner_, *cost_model_, *s);
    if (const auto* s = std::get_if<Analyze>(&statement.node)) return analyze(catalog_, *s);

    const auto& explain = std::get<Explain>(statement.node);
    const auto* select_stmt = std::get_if<Select>(&explain.inner->node);
    if (!select_stmt) throw DbError(explain.analyze ? "EXPLAIN ANALYZE only works on SELECT" : "EXPLAIN only works on SELECT");
    if (explain.analyze)
        return {{}, {}, explain_analyze_select(catalog_, optimizer_, optimize_, forced_planner_, *cost_model_, explain.verbose, *select_stmt).text, {}};

    BoundSelect bound = bind_select(*select_stmt, catalog_);
    Optimized optimized = optimize_for_explain(plan_select(bound), optimizer_, optimize_);
    if (!explain.verbose) return {{}, {}, format_explain(optimized.original, optimized.result, bound.scope, catalog_), {}};
    VerboseInfo chosen = choose_physical(catalog_, bound, optimized.result.plan, forced_planner_, *cost_model_);
    return {{}, {}, format_explain(optimized.original, optimized.result, bound.scope, catalog_, &chosen), {}};
}

}  // namespace cardinal
