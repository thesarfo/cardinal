#include "exec/project.h"

#include "expr/evaluator.h"

namespace cardinal {

std::optional<Row> Project::produce() {
    std::optional<Row> in = input_->next();
    if (!in) return std::nullopt;

    Row out;
    out.reserve(exprs_.size());
    for (const BoundExprPtr& expr : exprs_) out.push_back(evaluate(*expr, *in));
    return out;
}

}  // namespace cardinal
