#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "common/value.h"
#include "optimizer/default_rules.h"
#include "optimizer/rule_optimizer.h"

namespace cardinal {

struct QueryResult {
    // Set for SELECT, with one name per output column (even when there are no rows).
    std::vector<std::string> columns;
    std::vector<Row> rows;
    // Set for statements that return no rows: "CREATE TABLE", "INSERT 3".
    std::string message;

    bool returns_rows() const { return !columns.empty(); }
};

// Runs SQL end to end: parse, bind, plan, execute. The shell and the test-file
// runner both go through this.
class Database {
public:
    // Runs one statement. Throws ParseError for SQL it can't read and DbError for SQL
    // it can't carry out. A statement that throws changes nothing.
    QueryResult execute(std::string_view sql);

    const Catalog& catalog() const { return catalog_; }

    // On by default. Turning it off runs plans exactly as the query was written, which
    // is how tests check that the optimizer never changes an answer.
    void set_optimizer_enabled(bool enabled) { optimize_ = enabled; }

private:
    Catalog catalog_;
    RuleOptimizer optimizer_{RuleOptimizer::staged(default_stages())};
    bool optimize_ = true;
};

}  // namespace cardinal
