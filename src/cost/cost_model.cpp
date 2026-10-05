#include "cost/cost_model.h"

#include <algorithm>
#include <cmath>

namespace cardinal {

namespace {

// A guess can come out fractional or, from an odd formula, negative; a cost never goes below zero.
double rows_or_zero(double rows) { return std::max(0.0, rows); }

}  // namespace

Cost DefaultCostModel::scan(double rows) const {
    rows = rows_or_zero(rows);
    double pages = std::ceil(rows / params_.rows_per_page);
    return {pages * params_.page_cost + rows * params_.per_row_cost};
}

Cost DefaultCostModel::filter(double input_rows, int conditions) const {
    return {rows_or_zero(input_rows) * params_.per_check_cost * std::max(1, conditions)};
}

Cost DefaultCostModel::project(double rows) const { return {rows_or_zero(rows) * params_.per_row_cost}; }

Cost DefaultCostModel::sort(double rows) const {
    rows = rows_or_zero(rows);
    if (rows < 2) return {0};
    return {rows * std::log2(rows) * params_.per_check_cost};
}

Cost DefaultCostModel::nested_loop_join(double left_rows, double right_rows, double output_rows) const {
    // Every pair is checked once; each row that passes is built and passed on.
    return {rows_or_zero(left_rows) * rows_or_zero(right_rows) * params_.per_check_cost +
            rows_or_zero(output_rows) * params_.per_row_cost};
}

Cost DefaultCostModel::hash_join(double build_rows, double probe_rows, double output_rows) const {
    // Building: hash the key (a check) and store the row (a row) for each build row.
    // Probing: hash the key and look it up (a check) for each probe row.
    return {rows_or_zero(build_rows) * (params_.per_row_cost + params_.per_check_cost) +
            rows_or_zero(probe_rows) * params_.per_check_cost + rows_or_zero(output_rows) * params_.per_row_cost};
}

}  // namespace cardinal
