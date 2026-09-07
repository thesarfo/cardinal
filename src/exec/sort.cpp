#include "exec/sort.h"

#include <algorithm>
#include <numeric>

#include "expr/evaluator.h"

namespace cardinal {

namespace {

// NULL < anything else. Returns -1, 0 or 1.
int compare_nullable(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return static_cast<int>(!a.is_null()) - static_cast<int>(!b.is_null());
    return compare_values(a, b);
}

}  // namespace

std::optional<Row> Sort::produce() {
    if (!loaded_) {
        loaded_ = true;
        std::vector<Row> rows;
        std::vector<std::vector<Value>> key_values;  // one entry per row, one value per key
        while (std::optional<Row> row = input_->next()) {
            std::vector<Value> values;
            for (const SortKey& key : keys_) values.push_back(evaluate(*key.expr, *row));
            key_values.push_back(std::move(values));
            rows.push_back(std::move(*row));
        }

        // Sort positions, not rows, so each key is worked out once per row.
        std::vector<std::size_t> order(rows.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            for (std::size_t k = 0; k < keys_.size(); ++k) {
                int c = compare_nullable(key_values[a][k], key_values[b][k]);
                if (c != 0) return keys_[k].descending ? c > 0 : c < 0;
            }
            return false;
        });

        sorted_.reserve(rows.size());
        for (std::size_t i : order) sorted_.push_back(std::move(rows[i]));
    }

    if (next_ >= sorted_.size()) return std::nullopt;
    return std::move(sorted_[next_++]);
}

}  // namespace cardinal
