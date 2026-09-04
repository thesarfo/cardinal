#include "exec/seq_scan.h"

namespace cardinal {

std::optional<Row> SeqScan::produce() {
    const auto& rows = table_.rows();
    if (next_ >= rows.size()) return std::nullopt;
    return rows[next_++];
}

}  // namespace cardinal
