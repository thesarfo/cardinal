#pragma once

#include <memory>
#include <vector>

#include "binder/bound_expr.h"
#include "exec/operator.h"

namespace cardinal {

// The simplest join. For each left row, test it against every right row, in order,
// and pass on the left row followed by the right row when the condition is true.
//
// The right input is read once, on the first call, and kept in memory, so it never
// has to be restarted. `rows_scanned` counts the pairs tested.
//
// The condition's column ids are positions in the combined row (left values, then
// right values). With no condition every pairing passes, which is a cross join.
class NestedLoopJoin : public Operator {
public:
    const char* name() const override { return "NestedLoopJoin"; }
    NestedLoopJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, BoundExprPtr condition)
        : left_(std::move(left)), right_(std::move(right)), condition_(std::move(condition)) {}

    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    std::unique_ptr<Operator> left_;
    std::unique_ptr<Operator> right_;
    BoundExprPtr condition_;

    bool right_loaded_ = false;
    std::vector<Row> right_rows_;
    std::optional<Row> current_left_;
    std::size_t next_right_ = 0;
};

}  // namespace cardinal
