#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/value.h"

namespace cardinal {

// What an operator did while running. EXPLAIN ANALYZE reads these later to set
// the guessed row counts against the real ones.
struct ExecStats {
    std::uint64_t rows_out = 0;
    // Rows (or row pairs) the operator had to test to decide what to pass on, repeats
    // counted. Only joins set it so far: a nested loop join over 3 and 4 rows tests 12.
    std::uint64_t rows_scanned = 0;
    // Time spent inside next(), counting the operators below it.
    std::chrono::nanoseconds time{0};
};

// One step of a running plan. A query runs by asking the top operator for a row;
// it asks the operator below it, and so on down to the scan. Rows flow up one at
// a time (the "iterator model"), so nothing is built in bulk unless a step has to
// (Sort does).
class Operator {
public:
    virtual ~Operator() = default;

    // The next row, or nothing once the input is used up. Counts rows and time.
    std::optional<Row> next();

    const ExecStats& stats() const { return stats_; }

    // What kind of operator this is: "SeqScan", "Filter", "HashJoin", ...
    virtual const char* name() const = 0;

    // The operators this one reads from, for adding up work across a whole tree.
    virtual std::vector<const Operator*> children() const { return {}; }

protected:
    void count_scanned(std::uint64_t n = 1) { stats_.rows_scanned += n; }

    // Each operator's real work. Not called again after it returns nothing.
    virtual std::optional<Row> produce() = 0;

private:
    ExecStats stats_;
    bool done_ = false;
};

}  // namespace cardinal
