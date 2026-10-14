#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/value.h"
#include "index/ordered_index.h"

namespace cardinal {

// A B+ tree from keys to row positions. Same job as OrderedIndex, built the way a database
// builds it: wide nodes, all entries in the leaves, and the leaves chained in key order.
//
// Each entry is a (key, row id) pair, ordered by key and then by row id, so no two entries are
// equal and a key that occurs many times is just many entries in a row. Rows with the same
// key therefore come out in row id order. NULL keys are not stored.
class BPlusTree {
public:
    // `max_keys` is the most entries a node holds before it splits (at least 3). Small values
    // make tall trees, which is what the tests use to exercise splits.
    explicit BPlusTree(std::size_t max_keys = 64);
    ~BPlusTree();
    BPlusTree(BPlusTree&&) noexcept;
    BPlusTree& operator=(BPlusTree&&) noexcept;

    // Ignores a NULL key.
    void insert(const Value& key, std::size_t row_id);

    // Row ids with exactly this key, in row id order.
    std::vector<std::size_t> lookup(const Value& key) const;

    // Row ids with a key between `lo` and `hi`, in key order and then row id order. No bound
    // means open on that side. Walks the leaf chain, so a long range reads no inner nodes.
    std::vector<std::size_t> range(const std::optional<Bound>& lo, const std::optional<Bound>& hi) const;

    // Removes the entry (key, row_id). False if there is no such entry. A node that falls under
    // half full borrows from a neighbour, or merges with it when the neighbour has none to spare.
    bool erase(const Value& key, std::size_t row_id);

    std::size_t size() const { return size_; }

    // Levels, counting the leaves. An empty tree is one empty leaf, height 1.
    int height() const;

    // The tree drawn as indented text, one node per line: separators of an inner node as
    // `[k | k]`, entries of a leaf as `(key:row ...)`.
    std::string to_text() const;

    // Throws std::logic_error naming the first rule that does not hold: entries sorted, node
    // sizes within bounds, separators bounding their subtrees, every leaf at the same depth,
    // and the leaf chain holding every entry once in order. For tests.
    void check() const;

private:
    struct Entry;
    struct Node;
    struct Split;

    std::optional<Split> insert_into(Node& node, Entry entry);
    bool erase_from(Node& node, const Entry& entry);
    void fix_underfull_child(Node& parent, std::size_t at);
    void check_node(const Node& node, const Entry* lo, const Entry* hi, int depth, int& leaf_depth, bool is_root) const;
    void write(const Node& node, int depth, std::string& out) const;

    std::unique_ptr<Node> root_;
    std::size_t max_keys_;
    std::size_t size_ = 0;
};

}  // namespace cardinal
