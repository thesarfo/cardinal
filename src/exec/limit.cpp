#include "exec/limit.h"

namespace cardinal {

std::optional<Row> Limit::produce() {
    if (remaining_ <= 0) return std::nullopt;
    std::optional<Row> row = input_->next();
    if (row) --remaining_;
    return row;
}

}  // namespace cardinal
