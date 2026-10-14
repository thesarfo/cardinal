#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "common/value.h"

namespace cardinal {

// One end of a range. No value means the range is open on that side.
struct Bound {
    Value value;
    bool inclusive = true;
};

// Maps one column's values to the positions of the rows that hold them, in key order.
// NULL is never stored: no comparison finds it, so an index scan never needs it.
class OrderedIndex {
public:
    // Ignores a NULL key.
    void insert(const Value& key, std::size_t row_id);

    // Row ids with exactly this key, in the order they were inserted.
    std::vector<std::size_t> lookup(const Value& key) const;

    // Row ids with a key between `lo` and `hi`, in key order (ties in insertion order).
    std::vector<std::size_t> range(const std::optional<Bound>& lo, const std::optional<Bound>& hi) const;

    std::size_t size() const { return entries_.size(); }

private:
    struct Less {
        bool operator()(const Value& a, const Value& b) const;
    };
    std::multimap<Value, std::size_t, Less> entries_;
};

}  // namespace cardinal
