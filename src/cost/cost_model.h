#pragma once

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

// The numbers the formulas are built from. The defaults are PostgreSQL's cost settings
// (seq_page_cost, cpu_tuple_cost and cpu_operator_cost), copied as they are. They are not
// tuned to this engine; calibration (task 6.5) is where that happens.
struct CostParams {
    double page_cost = 1.0;        // reading one page of a table
    double per_row_cost = 0.01;    // handling one row: reading, building or passing it on
    double per_check_cost = 0.0025;  // one comparison or hash of a value
    // The tables here live in memory and have no pages. Pretending they do keeps the
    // formulas in the usual shape; this is how many rows fit on a pretend page.
    double rows_per_page = 100;
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
    virtual Cost project(double rows) const = 0;
    virtual Cost sort(double rows) const = 0;
    // Tests every left row against every right row. The right input is read once and kept.
    virtual Cost nested_loop_join(double left_rows, double right_rows, double output_rows) const = 0;
    // Loads `build_rows` into a lookup table, then looks up each of `probe_rows`.
    virtual Cost hash_join(double build_rows, double probe_rows, double output_rows) const = 0;
};

// The formulas in docs/cost-model.md.
class DefaultCostModel : public CostModel {
public:
    explicit DefaultCostModel(CostParams params = {}) : params_(params) {}

    Cost scan(double rows) const override;
    Cost filter(double input_rows, int conditions) const override;
    Cost project(double rows) const override;
    Cost sort(double rows) const override;
    Cost nested_loop_join(double left_rows, double right_rows, double output_rows) const override;
    Cost hash_join(double build_rows, double probe_rows, double output_rows) const override;

    const CostParams& params() const { return params_; }

private:
    CostParams params_;
};

}  // namespace cardinal
