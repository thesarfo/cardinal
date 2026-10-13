#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "exec/operator.h"
#include "physical/plan.h"

namespace cardinal {

// Groups its input by the key expressions and totals each group, with a lookup table from key
// to group. The first next() reads the whole input.
//
//  - Two keys are the same group if every part compares equal, so 1 and 1.0 are, and NULL is
//    a group of its own (it does not match anything but another NULL).
//  - With no keys the whole input is one group, and one row comes out even from an empty
//    input. With keys, an empty input gives no rows.
//  - Each output row is the group's key values followed by its totals, in the order the
//    plan lists them. Groups come out in the order they first appeared.
//  - COUNT(*) counts rows; COUNT(x) counts rows where x is not NULL. SUM, AVG, MIN and MAX
//    skip NULLs and give NULL for a group with no other value. SUM of whole numbers stays a
//    whole number and throws DbError("integer overflow") if it does not fit; AVG is always a
//    DOUBLE. MIN and MAX compare the way ORDER BY does.
//  - `rows_scanned` counts the input rows folded into groups.
// Key and argument column ids are positions in the input's rows (see physical/plan.h).
class HashAggregate : public Operator {
public:
    const char* name() const override { return "HashAggregate"; }
    HashAggregate(std::unique_ptr<Operator> input, std::vector<BoundExprPtr> keys, std::vector<PhysicalAggregateCall> calls);

    std::vector<const Operator*> children() const override { return {input_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    std::unique_ptr<Operator> input_;
    std::vector<BoundExprPtr> keys_;
    std::vector<PhysicalAggregateCall> calls_;

    std::vector<Row> output_;
    std::size_t next_ = 0;
    bool loaded_ = false;
};

// The running total of one aggregate over one group. Shared with the sort-based version.
struct AggregateState {
    std::int64_t count = 0;  // rows seen (CountStar) or non-NULL values seen
    std::int64_t int_sum = 0;
    double double_sum = 0;
    bool all_int = true;
    Value best;  // MIN / MAX so far; NULL until a value arrives

    void add(AggFunc func, const Value& value);
    Value result(AggFunc func, const BoundExprPtr& arg) const;
};

}  // namespace cardinal
