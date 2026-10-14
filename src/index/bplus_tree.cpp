#include "index/bplus_tree.h"

#include <algorithm>
#include <stdexcept>

#include "expr/evaluator.h"

namespace cardinal {

struct BPlusTree::Entry {
    Value key;
    std::size_t row = 0;
};

// A leaf holds entries and points at the next leaf. An inner node holds its entries as
// separators, and one more child than separators: child i has the entries e with
// separators[i-1] <= e < separators[i].
struct BPlusTree::Node {
    bool leaf = true;
    std::vector<Entry> entries;
    std::vector<std::unique_ptr<Node>> children;
    Node* next = nullptr;  // leaves only
};

// What a full node hands to its parent: the entry that separates it from its new right sibling.
struct BPlusTree::Split {
    Entry separator;
    std::unique_ptr<Node> right;
};

namespace {

template <class Entry>
bool entry_less(const Entry& a, const Entry& b) {
    int c = compare_values(a.key, b.key);
    return c < 0 || (c == 0 && a.row < b.row);
}

}  // namespace

BPlusTree::BPlusTree(std::size_t max_keys) : root_(std::make_unique<Node>()), max_keys_(std::max<std::size_t>(3, max_keys)) {}
BPlusTree::~BPlusTree() = default;
BPlusTree::BPlusTree(BPlusTree&&) noexcept = default;
BPlusTree& BPlusTree::operator=(BPlusTree&&) noexcept = default;

std::optional<BPlusTree::Split> BPlusTree::insert_into(Node& node, Entry entry) {
    auto first_greater = [&](const std::vector<Entry>& entries) {
        return std::upper_bound(entries.begin(), entries.end(), entry, entry_less<Entry>);
    };

    if (node.leaf) {
        node.entries.insert(first_greater(node.entries), std::move(entry));
        if (node.entries.size() <= max_keys_) return std::nullopt;
        // Full: the upper half moves to a new right sibling, and its first entry is copied up.
        std::size_t mid = node.entries.size() / 2;
        auto right = std::make_unique<Node>();
        right->entries.assign(node.entries.begin() + static_cast<std::ptrdiff_t>(mid), node.entries.end());
        node.entries.resize(mid);
        right->next = node.next;
        node.next = right.get();
        Entry separator = right->entries.front();
        return Split{std::move(separator), std::move(right)};
    }

    std::size_t at = static_cast<std::size_t>(first_greater(node.entries) - node.entries.begin());
    std::optional<Split> below = insert_into(*node.children[at], std::move(entry));
    if (!below) return std::nullopt;
    node.entries.insert(node.entries.begin() + static_cast<std::ptrdiff_t>(at), std::move(below->separator));
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(at) + 1, std::move(below->right));
    if (node.entries.size() <= max_keys_) return std::nullopt;

    // Full: the middle separator moves up (it is not kept below), the upper half moves right.
    std::size_t mid = node.entries.size() / 2;
    auto right = std::make_unique<Node>();
    right->leaf = false;
    Entry up = std::move(node.entries[mid]);
    right->entries.assign(std::make_move_iterator(node.entries.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                          std::make_move_iterator(node.entries.end()));
    right->children.assign(std::make_move_iterator(node.children.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                           std::make_move_iterator(node.children.end()));
    node.entries.resize(mid);
    node.children.resize(mid + 1);
    return Split{std::move(up), std::move(right)};
}

void BPlusTree::insert(const Value& key, std::size_t row_id) {
    if (key.is_null()) return;
    std::optional<Split> split = insert_into(*root_, Entry{key, row_id});
    ++size_;
    if (!split) return;
    auto root = std::make_unique<Node>();
    root->leaf = false;
    root->entries.push_back(std::move(split->separator));
    root->children.push_back(std::move(root_));
    root->children.push_back(std::move(split->right));
    root_ = std::move(root);
}

std::vector<std::size_t> BPlusTree::lookup(const Value& key) const {
    std::vector<std::size_t> out;
    if (key.is_null()) return out;
    const Entry probe{key, 0};  // no row id is smaller, so this lands on the first entry with the key

    const Node* node = root_.get();
    while (!node->leaf) {
        auto at = std::upper_bound(node->entries.begin(), node->entries.end(), probe, entry_less<Entry>);
        node = node->children[static_cast<std::size_t>(at - node->entries.begin())].get();
    }
    auto it = std::lower_bound(node->entries.begin(), node->entries.end(), probe, entry_less<Entry>);
    // The run of equal keys can continue into the next leaf, and the first match can be there.
    while (node) {
        for (; it != node->entries.end(); ++it) {
            if (compare_values(it->key, key) != 0) return out;
            out.push_back(it->row);
        }
        node = node->next;
        if (node) it = node->entries.begin();
    }
    return out;
}

std::vector<std::size_t> BPlusTree::range(const std::optional<Bound>& lo, const std::optional<Bound>& hi) const {
    std::vector<std::size_t> out;
    const Node* node = root_.get();
    const Entry probe{lo ? lo->value : Value(), 0};
    while (!node->leaf) {
        std::size_t at = 0;
        if (lo) at = static_cast<std::size_t>(std::upper_bound(node->entries.begin(), node->entries.end(), probe, entry_less<Entry>) - node->entries.begin());
        node = node->children[at].get();
    }
    auto it = node->entries.begin();
    if (lo) it = std::lower_bound(node->entries.begin(), node->entries.end(), probe, entry_less<Entry>);
    bool skipping = lo && !lo->inclusive;  // an exclusive lower bound passes over the entries equal to it
    while (node) {
        for (; it != node->entries.end(); ++it) {
            if (skipping) {
                if (compare_values(it->key, lo->value) == 0) continue;
                skipping = false;
            }
            if (hi) {
                int c = compare_values(it->key, hi->value);
                if (c > 0 || (c == 0 && !hi->inclusive)) return out;
            }
            out.push_back(it->row);
        }
        node = node->next;
        if (node) it = node->entries.begin();
    }
    return out;
}

bool BPlusTree::erase_from(Node& node, const Entry& entry) {
    if (node.leaf) {
        auto it = std::lower_bound(node.entries.begin(), node.entries.end(), entry, entry_less<Entry>);
        if (it == node.entries.end() || entry_less(entry, *it)) return false;
        node.entries.erase(it);
        return true;
    }
    std::size_t at = static_cast<std::size_t>(std::upper_bound(node.entries.begin(), node.entries.end(), entry, entry_less<Entry>) - node.entries.begin());
    if (!erase_from(*node.children[at], entry)) return false;
    fix_underfull_child(node, at);
    return true;
}

// Child `at` of `parent` may have fallen under half full. Borrow from the left neighbour, else
// from the right, else merge with one of them. A merge removes a separator from `parent`, which
// can leave `parent` itself under half full for its own parent to fix.
void BPlusTree::fix_underfull_child(Node& parent, std::size_t at) {
    Node& child = *parent.children[at];
    const std::size_t min = max_keys_ / 2;
    if (child.entries.size() >= min) return;
    Node* left = at > 0 ? parent.children[at - 1].get() : nullptr;
    Node* right = at + 1 < parent.children.size() ? parent.children[at + 1].get() : nullptr;

    if (left && left->entries.size() > min) {
        if (child.leaf) {
            child.entries.insert(child.entries.begin(), std::move(left->entries.back()));
            left->entries.pop_back();
            parent.entries[at - 1] = child.entries.front();
        } else {
            child.entries.insert(child.entries.begin(), std::move(parent.entries[at - 1]));
            parent.entries[at - 1] = std::move(left->entries.back());
            left->entries.pop_back();
            child.children.insert(child.children.begin(), std::move(left->children.back()));
            left->children.pop_back();
        }
        return;
    }
    if (right && right->entries.size() > min) {
        if (child.leaf) {
            child.entries.push_back(std::move(right->entries.front()));
            right->entries.erase(right->entries.begin());
            parent.entries[at] = right->entries.front();
        } else {
            child.entries.push_back(std::move(parent.entries[at]));
            parent.entries[at] = std::move(right->entries.front());
            right->entries.erase(right->entries.begin());
            child.children.push_back(std::move(right->children.front()));
            right->children.erase(right->children.begin());
        }
        return;
    }

    // Merge: always the right one of the pair into the left one, then drop the right.
    std::size_t l = left ? at - 1 : at;
    Node& into = *parent.children[l];
    Node& from = *parent.children[l + 1];
    if (!into.leaf) into.entries.push_back(std::move(parent.entries[l]));  // the separator comes down
    into.entries.insert(into.entries.end(), std::make_move_iterator(from.entries.begin()), std::make_move_iterator(from.entries.end()));
    for (auto& c : from.children) into.children.push_back(std::move(c));
    into.next = from.next;
    parent.entries.erase(parent.entries.begin() + static_cast<std::ptrdiff_t>(l));
    parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(l) + 1);
}

bool BPlusTree::erase(const Value& key, std::size_t row_id) {
    if (key.is_null()) return false;
    if (!erase_from(*root_, Entry{key, row_id})) return false;
    --size_;
    // An inner root left with one child is just that child.
    if (!root_->leaf && root_->entries.empty()) root_ = std::move(root_->children.front());
    return true;
}

int BPlusTree::height() const {
    int levels = 1;
    for (const Node* node = root_.get(); !node->leaf; node = node->children.front().get()) ++levels;
    return levels;
}

void BPlusTree::write(const Node& node, int depth, std::string& out) const {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    if (node.leaf) {
        out += "(";
        for (std::size_t i = 0; i < node.entries.size(); ++i)
            out += (i ? " " : "") + node.entries[i].key.to_string() + ":" + std::to_string(node.entries[i].row);
        out += ")\n";
        return;
    }
    out += "[";
    for (std::size_t i = 0; i < node.entries.size(); ++i)
        out += (i ? " | " : "") + node.entries[i].key.to_string() + ":" + std::to_string(node.entries[i].row);
    out += "]\n";
    for (const auto& child : node.children) write(*child, depth + 1, out);
}

std::string BPlusTree::to_text() const {
    std::string out;
    write(*root_, 0, out);
    return out;
}

void BPlusTree::check_node(const Node& node, const Entry* lo, const Entry* hi, int depth, int& leaf_depth, bool is_root) const {
    auto fail = [&](const std::string& why) { throw std::logic_error("B+ tree broken: " + why); };
    if (node.entries.size() > max_keys_) fail("a node holds " + std::to_string(node.entries.size()) + " entries, over the limit");
    if (!is_root && node.entries.size() < max_keys_ / 2) fail("a node holds " + std::to_string(node.entries.size()) + " entries, under half full");
    for (std::size_t i = 0; i < node.entries.size(); ++i) {
        const Entry& e = node.entries[i];
        if (i && !entry_less(node.entries[i - 1], e)) fail("entries out of order");
        if (lo && entry_less(e, *lo)) fail("an entry is below its subtree's lower bound");
        if (hi && !entry_less(e, *hi)) fail("an entry is at or above its subtree's upper bound");
    }
    if (node.leaf) {
        if (leaf_depth < 0) leaf_depth = depth;
        else if (leaf_depth != depth) fail("leaves at different depths");
        return;
    }
    if (node.children.size() != node.entries.size() + 1) fail("an inner node does not have one more child than separators");
    if (is_root && node.entries.empty()) fail("an inner root with no separators");
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        const Entry* child_lo = i ? &node.entries[i - 1] : lo;
        const Entry* child_hi = i < node.entries.size() ? &node.entries[i] : hi;
        check_node(*node.children[i], child_lo, child_hi, depth + 1, leaf_depth, false);
    }
}

void BPlusTree::check() const {
    int leaf_depth = -1;
    check_node(*root_, nullptr, nullptr, 0, leaf_depth, true);

    // The chain, from the leftmost leaf, must hold every entry once, in order.
    const Node* node = root_.get();
    while (!node->leaf) node = node->children.front().get();
    std::size_t count = 0;
    const Entry* previous = nullptr;
    for (; node; node = node->next) {
        for (const Entry& e : node->entries) {
            if (previous && !entry_less(*previous, e)) throw std::logic_error("B+ tree broken: the leaf chain is out of order");
            previous = &e;
            ++count;
        }
    }
    if (count != size_) throw std::logic_error("B+ tree broken: the leaf chain holds " + std::to_string(count) + " entries, expected " + std::to_string(size_));
}

}  // namespace cardinal
