#pragma once

#include <algorithm>
#include <compare>

namespace cardinal {

// How expensive a plan is expected to be, as one number in made-up units. Only the order
// matters: a cost of 20 is worth 20 only against another cost. A struct, not a bare double,
// so a separate startup cost (the work before the first row appears) can be added later
// without changing every caller.
struct Cost {
    double total = 0;

    Cost operator+(Cost other) const { return {total + other.total}; }
    Cost& operator+=(Cost other) {
        total += other.total;
        return *this;
    }
    auto operator<=>(const Cost&) const = default;
};

// The numbers the formulas are built from. They started as PostgreSQL's cost settings (page
// 1.0, row 0.01, check 0.0025) and the check cost has since been fitted to this engine by
// bench/analyze_calibration.py (task 6.5; see docs/cost-model.md). The page cost and the row
// cost are still PostgreSQL's, because nothing the planner decides depends on them yet.
struct CostParams {
    double page_cost = 1.0;        // reading one page of a table
    double per_row_cost = 0.01;    // handling one row: reading, building or passing it on
    double per_check_cost = 0.01;  // one comparison or hash of a value, about the same as handling a row here
    // The tables here live in memory and have no pages. Pretending they do keeps the
    // formulas in the usual shape; this is how many rows fit on a pretend page.
    double rows_per_page = 100;
    // Fetching one row found through an index: handling it, plus the jump to wherever it
    // lives instead of the next row along. Fitted by experiment E2 (task 9.4, docs/benchmarks.md).
    double index_row_cost = 0.07;
};

// What each step costs. Every function returns the cost of that step ALONE, not counting the
// steps below it, so the planner adds a step's own cost to the cost of its inputs. Row counts
// are the estimator's guesses, so they can be fractions.
//
// This is an interface so a different model can be swapped in, for tests or experiments.
class CostModel {
public:
    virtual ~CostModel() = default;

    virtual Cost scan(double rows) const = 0;
    // `conditions` is how many conditions are checked on each row (the parts of an AND).
    virtual Cost filter(double input_rows, int conditions) const = 0;
    // Finds `matching_rows` of a table of `table_rows` through an index: one descent to the
    // first match, then each match fetched. Conditions the index does not answer are a filter above.
    virtual Cost index_scan(double table_rows, double matching_rows) const = 0;
    virtual Cost project(double rows) const = 0;
    virtual Cost sort(double rows) const = 0;
    // Tests every left row against every right row. The right input is read once and kept.
    virtual Cost nested_loop_join(double left_rows, double right_rows, double output_rows) const = 0;
    // Walks two inputs that are already sorted by the join key side by side. Sorting them, when
    // they are not, is priced separately with sort().
    virtual Cost merge_join(double left_rows, double right_rows, double output_rows) const = 0;
    // Loads `build_rows` into a lookup table, then looks up each of `probe_rows`.
    virtual Cost hash_join(double build_rows, double probe_rows, double output_rows) const = 0;
    // Hashes each input row's group key into a table of groups, and passes on one row per group.
    virtual Cost hash_aggregate(double input_rows, double groups) const = 0;
    // Sorts the input by the group key, then passes over it once comparing neighbours.
    virtual Cost sort_aggregate(double input_rows, double groups) const = 0;
    // The same when the input already arrives sorted by the group key, so there is nothing to
    // sort: the pass over the groups is all that is left.
    virtual Cost sort_aggregate_presorted(double input_rows, double groups) const {
        return {std::max(0.0, sort_aggregate(input_rows, groups).total - sort(input_rows).total)};
    }
};

// The formulas in docs/cost-model.md.
class DefaultCostModel : public CostModel {
public:
    explicit DefaultCostModel(CostParams params = {}) : params_(params) {}

    Cost scan(double rows) const override;
    Cost filter(double input_rows, int conditions) const override;
    Cost index_scan(double table_rows, double matching_rows) const override;
    Cost project(double rows) const override;
    Cost sort(double rows) const override;
    Cost nested_loop_join(double left_rows, double right_rows, double output_rows) const override;
    Cost merge_join(double left_rows, double right_rows, double output_rows) const override;
    Cost hash_join(double build_rows, double probe_rows, double output_rows) const override;
    Cost hash_aggregate(double input_rows, double groups) const override;
    Cost sort_aggregate(double input_rows, double groups) const override;

    const CostParams& params() const { return params_; }

private:
    CostParams params_;
};

}  // namespace cardinal
