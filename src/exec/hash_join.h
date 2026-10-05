#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "binder/bound_expr.h"
#include "exec/operator.h"

namespace cardinal {

// An equality join that does not test every pair. One input (the build side) is read whole
// and put in a lookup table by its key. Each row of the other input (the probe side) is
// then looked up, and only rows with an equal key meet.
//
//  - Rows whose key has a NULL never match anything, on either side.
//  - The key values are compared the way `=` compares them, so 1 and 1.0 match.
//  - Output rows are always the left input's values followed by the right input's, whichever
//    side was built, so the steps above need not care.
//  - Output order: probe rows in order, and for each, the build rows with that key in the
//    order they arrived.
//  - `residual`, if given, is the rest of the join condition. It is checked on each pair the
//    keys bring together and its column ids are positions in the combined row.
//  - `rows_scanned` counts those pairs.
class HashJoin : public Operator {
public:
    // Key expressions read one input's row each, by position in that row.
    HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, std::vector<BoundExprPtr> left_keys,
             std::vector<BoundExprPtr> right_keys, BoundExprPtr residual, bool build_left);

    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    struct KeyHash {
        std::size_t operator()(const std::vector<Value>& key) const;
    };
    struct KeyEqual {
        bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const;
    };
    using Table = std::unordered_map<std::vector<Value>, std::vector<Row>, KeyHash, KeyEqual>;

    // The key of a row, or nothing if any part of it is NULL.
    static std::optional<std::vector<Value>> key_of(const std::vector<BoundExprPtr>& exprs, const Row& row);
    void build();

    std::unique_ptr<Operator> left_;
    std::unique_ptr<Operator> right_;
    std::vector<BoundExprPtr> left_keys_;
    std::vector<BoundExprPtr> right_keys_;
    BoundExprPtr residual_;
    bool build_left_;

    bool built_ = false;
    Table table_;
    std::optional<Row> probe_row_;
    const std::vector<Row>* matches_ = nullptr;
    std::size_t next_match_ = 0;
};

}  // namespace cardinal
