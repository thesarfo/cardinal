#include "engine/database.h"

#include "binder/binder.h"
#include "common/error.h"
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

QueryResult select(const Catalog& catalog, const Select& stmt) {
    BoundSelect bound = bind_select(stmt, catalog);
    PhysicalPtr plan = plan_physical(*plan_select(bound));
    std::unique_ptr<Operator> root = build_operator(*plan, catalog);

    QueryResult result;
    for (const BoundSelectItem& item : bound.items) result.columns.push_back(item.name);
    while (std::optional<Row> row = root->next()) result.rows.push_back(std::move(*row));
    return result;
}

}  // namespace

QueryResult Database::execute(std::string_view sql) {
    Statement statement = parse_statement(sql);
    if (const auto* s = std::get_if<CreateTable>(&statement.node)) return create_table(catalog_, *s);
    if (const auto* s = std::get_if<Insert>(&statement.node)) return insert(catalog_, *s);
    if (const auto* s = std::get_if<Select>(&statement.node)) return select(catalog_, *s);
    throw DbError("EXPLAIN is not supported yet");
}

}  // namespace cardinal
