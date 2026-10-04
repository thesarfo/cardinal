#pragma once

#include <memory>
#include <vector>

#include "binder/bound_expr.h"
#include "exec/operator.h"

namespace cardinal {

// Turns each input row into a new row with one value per expression.
// Column ids in the expressions are row positions (see physical/plan.h).
class Project : public Operator {
public:
    Project(std::unique_ptr<Operator> input, std::vector<BoundExprPtr> exprs)
        : input_(std::move(input)), exprs_(std::move(exprs)) {}

    std::vector<const Operator*> children() const override { return {input_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    std::unique_ptr<Operator> input_;
    std::vector<BoundExprPtr> exprs_;
};

}  // namespace cardinal
