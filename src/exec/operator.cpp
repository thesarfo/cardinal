#include "exec/operator.h"

namespace cardinal {

std::optional<Row> Operator::next() {
    if (done_) return std::nullopt;

    auto start = std::chrono::steady_clock::now();
    std::optional<Row> row = produce();
    stats_.time += std::chrono::steady_clock::now() - start;

    if (row) {
        ++stats_.rows_out;
    } else {
        done_ = true;
    }
    return row;
}

}  // namespace cardinal
