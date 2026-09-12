#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "common/value.h"

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

private:
    Catalog catalog_;
};

}  // namespace cardinal
