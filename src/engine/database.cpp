#include "engine/database.h"

#include "binder/binder.h"
#include "common/error.h"
#include "engine/explain.h"
#include "exec/build.h"
#include "expr/evaluator.h"
#include "logical/planner.h"
#include "physical/physical_planner.h"
#include "sql/parser.h"

namespace cardinal {

namespace {

QueryResult create_table(Catalog& catalog, const CreateTable& stmt) {
    TableInfo info{stmt.name, {}};
    for (const ColumnDef& column : stmt.columns) info.columns.push_back({column.name, column.type});
    catalog.create_table(std::move(info));
    return {{}, {}, "CREATE TABLE"};
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
    return {{}, {}, "INSERT " + std::to_string(count)};
}

QueryResult select(const Catalog& catalog, const RuleOptimizer& optimizer, bool optimize, const Select& stmt) {
    BoundSelect bound = bind_select(stmt, catalog);
    PlanPtr logical = plan_select(bound);
    if (optimize) logical = optimizer.optimize(logical).plan;
    PhysicalPtr plan = plan_physical(*logical);
    std::unique_ptr<Operator> root = build_operator(*plan, catalog);

    QueryResult result;
    for (const BoundSelectItem& item : bound.items) result.columns.push_back(item.name);
    while (std::optional<Row> row = root->next()) result.rows.push_back(std::move(*row));
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

QueryResult Database::execute(const Statement& statement) {
    if (const auto* s = std::get_if<CreateTable>(&statement.node)) return create_table(catalog_, *s);
    if (const auto* s = std::get_if<Insert>(&statement.node)) return insert(catalog_, *s);
    if (const auto* s = std::get_if<Select>(&statement.node)) return select(catalog_, optimizer_, optimize_, *s);

    const auto& inner = std::get<Explain>(statement.node).inner->node;
    const auto* select_stmt = std::get_if<Select>(&inner);
    if (!select_stmt) throw DbError("EXPLAIN only works on SELECT");
    BoundSelect bound = bind_select(*select_stmt, catalog_);
    PlanPtr original = plan_select(bound);
    return {{}, {}, format_explain(original, optimizer_.optimize(original), bound.scope)};
}

}  // namespace cardinal
