#pragma once

#include <optional>
#include <vector>

#include "catalog/table_info.h"
#include "common/value.h"
#include "stats/table_stats.h"

namespace cardinal {

// A table held in memory as a list of rows, in insertion order.
class Table {
public:
    explicit Table(TableInfo info) : info_(std::move(info)) {}

    const TableInfo& info() const { return info_; }
    const std::vector<Row>& rows() const { return rows_; }

    // Throws DbError if the row has the wrong number of values, or a value that
    // doesn't fit its column. NULL fits anywhere, and an INT fits a DOUBLE column
    // (stored as a double).
    void insert(Row row);

    // Same checks for every row first, then adds them all. If any row is bad, none are added.
    void insert_rows(std::vector<Row> rows);

    // Set by ANALYZE. None until then.
    const std::optional<TableStats>& stats() const { return stats_; }
    void set_stats(TableStats stats) { stats_ = std::move(stats); }

private:
    // Checks one row against the columns and converts INT to DOUBLE where needed.
    void fit(Row& row) const;

    TableInfo info_;
    std::vector<Row> rows_;
    std::optional<TableStats> stats_;
};

}  // namespace cardinal
