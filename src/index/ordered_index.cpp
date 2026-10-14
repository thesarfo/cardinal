#include "index/ordered_index.h"

namespace cardinal {

void OrderedIndex::insert(const Value& key, std::size_t row_id) { tree_.insert(key, row_id); }

std::vector<std::size_t> OrderedIndex::lookup(const Value& key) const { return tree_.lookup(key); }

std::vector<std::size_t> OrderedIndex::range(const std::optional<Bound>& lo, const std::optional<Bound>& hi) const {
    return tree_.range(lo, hi);
}

}  // namespace cardinal
