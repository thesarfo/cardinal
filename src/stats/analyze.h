#pragma once

#include <string>

#include "stats/table_stats.h"
#include "storage/table.h"

namespace cardinal {

// Reads every row of the table and works out the facts exactly. Nothing is sampled.
TableStats analyze_table(const Table& table);

// True if rows were added after the statistics were taken.
bool stats_are_stale(const Table& table, const TableStats& stats);

// The statistics as a text table, for `.stats`:
//   users: 1000 rows when analyzed
//   column | nulls | distinct | min | max
//   ...
// Adds a warning line if the table has changed since. No trailing newline.
std::string format_stats(const Table& table, const TableStats& stats);

}  // namespace cardinal
