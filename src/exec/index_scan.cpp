#include "exec/index_scan.h"

namespace cardinal {

std::optional<Row> IndexScan::produce() {
    if (next_ >= row_ids_.size()) return std::nullopt;
    return table_.rows()[row_ids_[next_++]];
}

}  // namespace cardinal
