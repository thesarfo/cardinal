#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/value.h"

namespace cardinal {

// What ANALYZE learned about one column. NULLs are not counted as a value: they have their
// own share, and the other facts are about the non-NULL values.
struct ColumnStats {
    std::string name;
    double null_fraction = 0;       // NULLs / rows, 0 for an empty table
    std::int64_t distinct = 0;      // exact number of different non-NULL values
    Value min, max;                 // NULL if every value is NULL (or there are no rows)
};

// What ANALYZE learned about a table, as of one moment.
struct TableStats {
    // How many rows the table had when it was analyzed. If the table has a different
    // number now, the statistics are out of date.
    std::int64_t row_count = 0;
    std::vector<ColumnStats> columns;  // in table order
};

}  // namespace cardinal
