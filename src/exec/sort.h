#pragma once

#include <memory>
#include <vector>

#include "exec/operator.h"
#include "logical/plan.h"

namespace cardinal {

// Orders rows by the keys, in memory. The first next() reads the whole input.
//
// Rules, written down because tests and SQLite comparisons depend on them:
//  - Rows with equal keys keep the order they arrived in (the sort is stable).
//  - NULL is smaller than every other value, so NULLs come first when ascending
//    and last when descending. SQLite does the same.
// Column ids in the key expressions are row positions (see physical/plan.h).
class Sort : public Operator {
public:
    Sort(std::unique_ptr<Operator> input, std::vector<SortKey> keys)
        : input_(std::move(input)), keys_(std::move(keys)) {}

    std::vector<const Operator*> children() const override { return {input_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    std::unique_ptr<Operator> input_;
    std::vector<SortKey> keys_;
    std::vector<Row> sorted_;
    std::size_t next_ = 0;
    bool loaded_ = false;
};

}  // namespace cardinal
