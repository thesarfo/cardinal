#include "index/ordered_index.h"

#include "expr/evaluator.h"

namespace cardinal {

bool OrderedIndex::Less::operator()(const Value& a, const Value& b) const {
    return compare_values(a, b) < 0;
}

void OrderedIndex::insert(const Value& key, std::size_t row_id) {
    if (key.is_null()) return;
    entries_.emplace(key, row_id);
}

std::vector<std::size_t> OrderedIndex::lookup(const Value& key) const {
    std::vector<std::size_t> out;
    if (key.is_null()) return out;
    auto [first, last] = entries_.equal_range(key);
    for (auto it = first; it != last; ++it) out.push_back(it->second);
    return out;
}

std::vector<std::size_t> OrderedIndex::range(const std::optional<Bound>& lo,
                                             const std::optional<Bound>& hi) const {
    auto first = entries_.begin();
    auto last = entries_.end();
    if (lo) first = lo->inclusive ? entries_.lower_bound(lo->value) : entries_.upper_bound(lo->value);
    if (hi) last = hi->inclusive ? entries_.upper_bound(hi->value) : entries_.lower_bound(hi->value);
    std::vector<std::size_t> out;
    // An empty or backwards range (lo above hi) walks nothing.
    for (auto it = first; it != entries_.end() && it != last; ++it) {
        if (hi && compare_values(it->first, hi->value) > 0) break;
        out.push_back(it->second);
    }
    return out;
}

}  // namespace cardinal
