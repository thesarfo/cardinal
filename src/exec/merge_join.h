#pragma once

#include <memory>
#include <vector>

#include "binder/bound_expr.h"
#include "exec/operator.h"

namespace cardinal {

// An equality join over two inputs that are both already sorted ascending by their join keys
// (NULL keys, which sort first, are fine: they never match). The two inputs are walked side by
// side, so each is read once and nothing is kept but one run of equal right keys.
//
//  - Rows whose key has a NULL never match anything, on either side.
//  - Keys are compared the way `=` compares them, so 1 and 1.0 match.
//  - Output rows are the left values followed by the right values, in left input order and, for
//    one left row, the matching right rows in their input order.
//  - `residual`, if given, is the rest of the join condition over the combined row, checked on
//    each pair the keys bring together. `rows_scanned` counts those pairs.
//  - The inputs must really be sorted. If they are not, rows are silently missed.
class MergeJoin : public Operator {
public:
    const char* name() const override { return "MergeJoin"; }
    MergeJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, std::vector<BoundExprPtr> left_keys,
              std::vector<BoundExprPtr> right_keys, BoundExprPtr residual);

    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    using Key = std::vector<Value>;

    static std::optional<Key> key_of(const std::vector<BoundExprPtr>& exprs, const Row& row);
    static int compare_keys(const Key& a, const Key& b);

    // Moves the right input along until its next row has a non-NULL key (or it ends).
    void advance_right();
    // Collects the run of right rows whose key equals `key`, if the right input is at that key.
    void gather_group(const Key& key);

    std::unique_ptr<Operator> left_;
    std::unique_ptr<Operator> right_;
    std::vector<BoundExprPtr> left_keys_, right_keys_;
    BoundExprPtr residual_;

    std::optional<Row> left_row_;
    std::optional<Row> right_row_;  // the right input's next row not yet in a group
    std::optional<Key> right_key_;  // its key
    bool right_started_ = false;

    std::optional<Key> group_key_;  // key of group_, if there is one
    std::vector<Row> group_;
    std::size_t next_in_group_ = 0;
};

}  // namespace cardinal
