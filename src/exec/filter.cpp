#include "exec/filter.h"

#include "expr/evaluator.h"

namespace cardinal {

std::optional<Row> Filter::produce() {
    while (std::optional<Row> row = input_->next()) {
        if (is_true(evaluate(*predicate_, *row))) return row;
    }
    return std::nullopt;
}

}  // namespace cardinal
