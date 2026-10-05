#pragma once

#include <algorithm>

namespace cardinal {

// How many times too high or too low a guess was, never below 1. Both counts are treated
// as at least 1, so a guess of 0 rows against 0 real rows is a perfect 1, and a guess of
// 0.3 against 200 is judged as 1 against 200.
//   q_error(100, 100) = 1     q_error(10, 100) = 10     q_error(100, 10) = 10
inline double q_error(double estimate, double actual) {
    estimate = std::max(1.0, estimate);
    actual = std::max(1.0, actual);
    return std::max(estimate / actual, actual / estimate);
}

}  // namespace cardinal
