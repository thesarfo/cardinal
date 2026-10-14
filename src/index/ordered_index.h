#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "common/value.h"
#include "index/bound.h"
#include "index/bplus_tree.h"

namespace cardinal {

// Maps one column's values to the positions of the rows that hold them, in key order. This is
// what a table keeps per index and what an index scan reads; the structure behind it is a
// B+ tree (it was a std::multimap until task 9.7). NULL is never stored: no comparison finds
// it, so an index scan never needs it.
class OrderedIndex {
public:
    // Ignores a NULL key.
    void insert(const Value& key, std::size_t row_id);

    // Row ids with exactly this key, in the order they were inserted.
    std::vector<std::size_t> lookup(const Value& key) const;

    // Row ids with a key between `lo` and `hi`, in key order (ties in insertion order).
    std::vector<std::size_t> range(const std::optional<Bound>& lo, const std::optional<Bound>& hi) const;

    // Removes one entry. False if the table never had it.
    bool erase(const Value& key, std::size_t row_id) { return tree_.erase(key, row_id); }

    std::size_t size() const { return tree_.size(); }
    const BPlusTree& tree() const { return tree_; }

private:
    BPlusTree tree_;
};

}  // namespace cardinal
