#include "exec/merge_join.h"

#include "expr/evaluator.h"

namespace cardinal {

MergeJoin::MergeJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, std::vector<BoundExprPtr> left_keys,
                     std::vector<BoundExprPtr> right_keys, BoundExprPtr residual)
    : left_(std::move(left)),
      right_(std::move(right)),
      left_keys_(std::move(left_keys)),
      right_keys_(std::move(right_keys)),
      residual_(std::move(residual)) {}

std::optional<MergeJoin::Key> MergeJoin::key_of(const std::vector<BoundExprPtr>& exprs, const Row& row) {
    Key key;
    key.reserve(exprs.size());
    for (const BoundExprPtr& e : exprs) {
        Value v = evaluate(*e, row);
        if (v.is_null()) return std::nullopt;
        key.push_back(std::move(v));
    }
    return key;
}

int MergeJoin::compare_keys(const Key& a, const Key& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (int c = compare_values(a[i], b[i])) return c;
    return 0;
}

void MergeJoin::advance_right() {
    right_row_.reset();
    right_key_.reset();
    while (std::optional<Row> row = right_->next()) {
        if (std::optional<Key> key = key_of(right_keys_, *row)) {
            right_row_ = std::move(row);
            right_key_ = std::move(key);
            return;
        }
    }
}

void MergeJoin::gather_group(const Key& key) {
    group_.clear();
    group_key_.reset();
    // Rows with a smaller key can never match this left row or any later one.
    while (right_row_ && compare_keys(*right_key_, key) < 0) advance_right();
    if (!right_row_ || compare_keys(*right_key_, key) != 0) return;
    group_key_ = key;
    while (right_row_ && compare_keys(*right_key_, key) == 0) {
        group_.push_back(std::move(*right_row_));
        advance_right();
    }
}

std::optional<Row> MergeJoin::produce() {
    if (!right_started_) {
        right_started_ = true;
        advance_right();
    }
    for (;;) {
        while (left_row_ && next_in_group_ < group_.size()) {
            const Row& right = group_[next_in_group_++];
            count_scanned();
            Row combined;
            combined.reserve(left_row_->size() + right.size());
            combined.insert(combined.end(), left_row_->begin(), left_row_->end());
            combined.insert(combined.end(), right.begin(), right.end());
            if (!residual_ || is_true(evaluate(*residual_, combined))) return combined;
        }

        left_row_ = left_->next();
        if (!left_row_) return std::nullopt;
        std::optional<Key> key = key_of(left_keys_, *left_row_);
        if (!key) {
            group_.clear();
            next_in_group_ = 0;
            continue;
        }
        // The same key as the last left row reuses the group; a new key looks for its own.
        if (!group_key_ || compare_keys(*group_key_, *key) != 0) gather_group(*key);
        next_in_group_ = 0;
    }
}

}  // namespace cardinal
