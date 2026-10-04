#include "stats/analyze.h"

#include <algorithm>
#include <cstdio>

#include "expr/evaluator.h"

namespace cardinal {

TableStats analyze_table(const Table& table) {
    const std::vector<Row>& rows = table.rows();
    TableStats stats;
    stats.row_count = static_cast<std::int64_t>(rows.size());

    const TableInfo& info = table.info();
    for (std::size_t c = 0; c < info.columns.size(); ++c) {
        std::vector<Value> values;
        for (const Row& row : rows)
            if (!row[c].is_null()) values.push_back(row[c]);

        ColumnStats column;
        column.name = info.columns[c].name;
        column.null_fraction = rows.empty() ? 0.0 : static_cast<double>(rows.size() - values.size()) / static_cast<double>(rows.size());

        std::sort(values.begin(), values.end(), [](const Value& a, const Value& b) { return compare_values(a, b) < 0; });
        for (std::size_t i = 0; i < values.size(); ++i)
            if (i == 0 || compare_values(values[i - 1], values[i]) != 0) ++column.distinct;
        if (!values.empty()) {
            column.min = values.front();
            column.max = values.back();
        }
        stats.columns.push_back(std::move(column));
    }
    return stats;
}

bool stats_are_stale(const Table& table, const TableStats& stats) {
    return static_cast<std::int64_t>(table.rows().size()) != stats.row_count;
}

std::string format_stats(const Table& table, const TableStats& stats) {
    const TableInfo& info = table.info();
    std::vector<std::vector<std::string>> cells{{"column", "nulls", "distinct", "min", "max"}};
    for (const ColumnStats& c : stats.columns) {
        char nulls[32];
        std::snprintf(nulls, sizeof nulls, "%.1f%%", c.null_fraction * 100);
        cells.push_back({c.name, nulls, std::to_string(c.distinct), c.min.to_string(), c.max.to_string()});
    }
    std::vector<std::size_t> widths(5, 0);
    for (const auto& row : cells)
        for (std::size_t i = 0; i < row.size(); ++i) widths[i] = std::max(widths[i], row[i].size());

    std::string out = info.name + ": " + std::to_string(stats.row_count) + " rows when analyzed\n";
    for (std::size_t r = 0; r < cells.size(); ++r) {
        std::string line;
        for (std::size_t i = 0; i < cells[r].size(); ++i) {
            if (i > 0) line += " | ";
            line += i + 1 < cells[r].size() ? cells[r][i] + std::string(widths[i] - cells[r][i].size(), ' ') : cells[r][i];
        }
        out += line + "\n";
        if (r == 0) {
            std::string rule;
            for (std::size_t i = 0; i < widths.size(); ++i) rule += (i ? "-+-" : "") + std::string(widths[i], '-');
            out += rule + "\n";
        }
    }
    if (stats_are_stale(table, stats))
        out += "warning: the table has " + std::to_string(table.rows().size()) + " rows now; run ANALYZE " + info.name + " again\n";
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace cardinal
