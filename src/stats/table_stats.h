#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/value.h"

namespace cardinal {

// ANALYZE keeps at most this many most-common values per column, and builds a histogram
// of this many buckets for number columns.
constexpr int kMaxCommonValues = 10;
constexpr int kHistogramBuckets = 32;

struct CommonValue {
    Value value;
    std::int64_t count = 0;  // how many rows hold it
};

// An equal-depth histogram: the sorted values are cut into buckets that each hold about
// the same number of rows, and only the cut points are kept. Bucket i covers
// bounds[i] to bounds[i + 1]. Built from the values NOT in the common list, since those
// are already known exactly.
struct Histogram {
    std::vector<double> bounds;  // buckets + 1 cut points, smallest first
    std::int64_t rows = 0;       // how many rows the histogram covers
    int buckets() const { return static_cast<int>(bounds.size()) - 1; }
};

// What ANALYZE learned about one column. NULLs are not counted as a value: they have their
// own share, and the other facts are about the non-NULL values.
struct ColumnStats {
    std::string name;
    double null_fraction = 0;       // NULLs / rows, 0 for an empty table
    std::int64_t distinct = 0;      // exact number of different non-NULL values
    Value min, max;                 // NULL if every value is NULL (or there are no rows)

    // Values that appear in more than one row, most frequent first (equal counts: smaller
    // value first). A value that appears once is never listed, so a column with all-different
    // values has none.
    std::vector<CommonValue> common;
    // Number columns only, and only if some values are left once the common ones are removed.
    std::optional<Histogram> histogram;
};

// What ANALYZE learned about a table, as of one moment.
struct TableStats {
    // How many rows the table had when it was analyzed. If the table has a different
    // number now, the statistics are out of date.
    std::int64_t row_count = 0;
    std::vector<ColumnStats> columns;  // in table order
};

}  // namespace cardinal
