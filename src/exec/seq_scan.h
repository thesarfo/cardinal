#pragma once

#include "exec/operator.h"
#include "storage/table.h"

namespace cardinal {

// Hands out every row of a table, in the order it was inserted. The table must
// outlive the scan and must not be inserted into while it runs.
class SeqScan : public Operator {
public:
    const char* name() const override { return "SeqScan"; }
    explicit SeqScan(const Table& table) : table_(table) {}

protected:
    std::optional<Row> produce() override;

private:
    const Table& table_;
    std::size_t next_ = 0;
};

}  // namespace cardinal
