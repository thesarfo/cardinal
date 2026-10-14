#pragma once

#include <optional>
#include <vector>

#include "exec/operator.h"
#include "index/ordered_index.h"
#include "storage/table.h"

namespace cardinal {

// Finds the rows of a table whose key is between two bounds through an index, and hands out
// the whole rows, in key order (rows with the same key in the order they were inserted).
// The table must outlive the scan and must not be inserted into while it runs.
class IndexScan : public Operator {
public:
    const char* name() const override { return "IndexScan"; }
    IndexScan(const Table& table, const OrderedIndex& index, std::optional<Bound> lo, std::optional<Bound> hi)
        : table_(table), row_ids_(index.range(lo, hi)) {}

protected:
    std::optional<Row> produce() override;

private:
    const Table& table_;
    std::vector<std::size_t> row_ids_;
    std::size_t next_ = 0;
};

}  // namespace cardinal
