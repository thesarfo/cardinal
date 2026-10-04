#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "binder/bound_expr.h"
#include "binder/scope.h"
#include "catalog/catalog.h"
#include "stats/table_stats.h"

namespace cardinal {

// What to assume when there are no statistics, or a condition is too complicated to read.
// These are guesses, the same kind real databases make. Each is a fraction of the rows.
constexpr double kDefaultEquality = 0.005;   // col = value
constexpr double kDefaultRange = 1.0 / 3.0;  // col < value, and the like
constexpr double kDefaultIsNull = 0.005;     // col IS NULL
constexpr double kDefaultOther = 0.5;        // anything we can't read at all
constexpr std::int64_t kDefaultDistinct = 200;  // distinct values assumed when two columns are compared and one has no statistics

// The statistics for one column, and how many rows its table had when they were taken.
struct ColumnFacts {
    const ColumnStats* column;
    std::int64_t table_rows;
};

// Finds the facts for a ColumnId. A column whose table was never analyzed has none.
class StatsLookup {
public:
    // Looks up each table in the scope by name. Tables with no statistics are skipped.
    static StatsLookup for_scope(const Scope& scope, const Catalog& catalog);

    const ColumnFacts* find(ColumnId id) const {
        return id.value < facts_.size() && facts_[id.value] ? &*facts_[id.value] : nullptr;
    }

private:
    std::vector<std::optional<ColumnFacts>> facts_;  // indexed by ColumnId::value
};

// The fraction of rows (0 to 1) a condition keeps, guessed from the statistics. Every
// formula is written up in docs/cardinality-estimation.md.
double estimate_selectivity(const BoundExpr& predicate, const StatsLookup& stats);

}  // namespace cardinal
