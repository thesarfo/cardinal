#include "exec/nested_loop_join.h"

#include "expr/evaluator.h"

namespace cardinal {

std::optional<Row> NestedLoopJoin::produce() {
    if (!right_loaded_) {
        right_loaded_ = true;
        while (std::optional<Row> row = right_->next()) right_rows_.push_back(std::move(*row));
    }

    for (;;) {
        if (!current_left_) {
            current_left_ = left_->next();
            if (!current_left_) return std::nullopt;
            next_right_ = 0;
        }

        while (next_right_ < right_rows_.size()) {
            const Row& right = right_rows_[next_right_++];
            count_scanned();

            Row combined = *current_left_;
            combined.insert(combined.end(), right.begin(), right.end());
            if (!condition_ || is_true(evaluate(*condition_, combined))) return combined;
        }
        current_left_.reset();
    }
}

}  // namespace cardinal
