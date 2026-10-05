#pragma once

#include <memory>

#include "binder/bound_expr.h"
#include "exec/operator.h"

namespace cardinal {

// Passes on the rows for which the predicate is true. False and NULL are dropped.
// Column ids in the predicate are row positions (see physical/plan.h).
class Filter : public Operator {
public:
    const char* name() const override { return "Filter"; }
    Filter(std::unique_ptr<Operator> input, BoundExprPtr predicate)
        : input_(std::move(input)), predicate_(std::move(predicate)) {}

    std::vector<const Operator*> children() const override { return {input_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    std::unique_ptr<Operator> input_;
    BoundExprPtr predicate_;
};

}  // namespace cardinal
