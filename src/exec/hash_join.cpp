#include "exec/hash_join.h"

#include <functional>

#include "expr/evaluator.h"

namespace cardinal {

namespace {

std::size_t hash_value(const Value& v) {
    switch (v.type()) {
        case Type::Int:
        case Type::Double: {
            // 1 and 1.0 are equal, so they must hash alike: hash every number as a double.
            double d = v.type() == Type::Int ? static_cast<double>(v.as_int()) : v.as_double();
            if (d == 0) d = 0;  // -0.0 equals 0.0
            return std::hash<double>{}(d);
        }
        case Type::Text: return std::hash<std::string>{}(v.as_text());
        case Type::Bool: return std::hash<bool>{}(v.as_bool());
    }
    return 0;
}

}  // namespace

std::size_t HashJoin::KeyHash::operator()(const std::vector<Value>& key) const {
    std::size_t h = 0;
    for (const Value& v : key) h = h * 1000003u ^ hash_value(v);
    return h;
}

bool HashJoin::KeyEqual::operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (compare_values(a[i], b[i]) != 0) return false;
    return true;
}

HashJoin::HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, std::vector<BoundExprPtr> left_keys,
                   std::vector<BoundExprPtr> right_keys, BoundExprPtr residual, bool build_left)
    : left_(std::move(left)),
      right_(std::move(right)),
      left_keys_(std::move(left_keys)),
      right_keys_(std::move(right_keys)),
      residual_(std::move(residual)),
      build_left_(build_left) {}

std::optional<std::vector<Value>> HashJoin::key_of(const std::vector<BoundExprPtr>& exprs, const Row& row) {
    std::vector<Value> key;
    key.reserve(exprs.size());
    for (const BoundExprPtr& e : exprs) {
        Value v = evaluate(*e, row);
        if (v.is_null()) return std::nullopt;
        key.push_back(std::move(v));
    }
    return key;
}

void HashJoin::build() {
    Operator& side = build_left_ ? *left_ : *right_;
    const std::vector<BoundExprPtr>& keys = build_left_ ? left_keys_ : right_keys_;
    while (std::optional<Row> row = side.next()) {
        if (std::optional<std::vector<Value>> key = key_of(keys, *row)) table_[std::move(*key)].push_back(std::move(*row));
    }
    built_ = true;
}

std::optional<Row> HashJoin::produce() {
    if (!built_) build();

    Operator& probe = build_left_ ? *right_ : *left_;
    const std::vector<BoundExprPtr>& probe_keys = build_left_ ? right_keys_ : left_keys_;

    for (;;) {
        while (matches_ && next_match_ < matches_->size()) {
            const Row& match = (*matches_)[next_match_++];
            count_scanned();
            Row combined;
            combined.reserve(match.size() + probe_row_->size());
            const Row& left = build_left_ ? match : *probe_row_;
            const Row& right = build_left_ ? *probe_row_ : match;
            combined.insert(combined.end(), left.begin(), left.end());
            combined.insert(combined.end(), right.begin(), right.end());
            if (!residual_ || is_true(evaluate(*residual_, combined))) return combined;
        }

        matches_ = nullptr;
        probe_row_ = probe.next();
        if (!probe_row_) return std::nullopt;
        std::optional<std::vector<Value>> key = key_of(probe_keys, *probe_row_);
        if (!key) continue;
        if (auto it = table_.find(*key); it != table_.end()) {
            matches_ = &it->second;
            next_match_ = 0;
        }
    }
}

}  // namespace cardinal
