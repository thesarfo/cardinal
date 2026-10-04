#include "stats/analyze.h"

#include <algorithm>
#include <cstdio>

#include "expr/evaluator.h"

namespace cardinal {

namespace {

// `values` is sorted and has no NULLs.
void build_common_and_histogram(const std::vector<Value>& values, Type type, ColumnStats& column) {
    struct Run {
        std::size_t start;
        std::int64_t count;
    };
    std::vector<Run> runs;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0 && compare_values(values[i - 1], values[i]) == 0) {
            ++runs.back().count;
        } else {
            runs.push_back({i, 1});
        }
    }

    // The most frequent runs that repeat. stable_sort keeps smaller values first among equals.
    std::vector<std::size_t> order;
    for (std::size_t r = 0; r < runs.size(); ++r)
        if (runs[r].count > 1) order.push_back(r);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return runs[a].count > runs[b].count; });
    if (order.size() > static_cast<std::size_t>(kMaxCommonValues)) order.resize(kMaxCommonValues);

    std::vector<bool> is_common(runs.size(), false);
    for (std::size_t r : order) {
        is_common[r] = true;
        column.common.push_back({values[runs[r].start], runs[r].count});
    }

    if (type != Type::Int && type != Type::Double) return;
    std::vector<double> rest;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        if (is_common[r]) continue;
        for (std::int64_t k = 0; k < runs[r].count; ++k) {
            const Value& v = values[runs[r].start + static_cast<std::size_t>(k)];
            rest.push_back(v.type() == Type::Int ? static_cast<double>(v.as_int()) : v.as_double());
        }
    }
    if (rest.empty()) return;

    const std::size_t n = rest.size();
    // Never more buckets than gaps between values, so no bucket has zero width.
    const std::size_t buckets = std::max<std::size_t>(1, std::min<std::size_t>(kHistogramBuckets, n - 1));
    Histogram histogram;
    histogram.rows = static_cast<std::int64_t>(n);
    for (std::size_t i = 0; i <= buckets; ++i) histogram.bounds.push_back(rest[i * (n - 1) / buckets]);
    column.histogram = std::move(histogram);
}

}  // namespace

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
        build_common_and_histogram(values, info.columns[c].type, column);
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
    auto number = [](double d) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", d);
        return std::string(buf);
    };
    for (const ColumnStats& c : stats.columns) {
        if (!c.common.empty()) {
            std::string line = c.name + " common values:";
            for (std::size_t i = 0; i < c.common.size(); ++i) {
                char share[32];
                std::snprintf(share, sizeof share, "%.1f%%", 100.0 * static_cast<double>(c.common[i].count) / static_cast<double>(stats.row_count));
                line += std::string(i ? "," : "") + " " + c.common[i].value.to_string() + " x" + std::to_string(c.common[i].count) + " (" + share + ")";
            }
            out += line + "\n";
        }
        if (c.histogram) {
            std::string line = c.name + " histogram (" + std::to_string(c.histogram->buckets()) + (c.histogram->buckets() == 1 ? " bucket, " : " buckets, ") +
                               std::to_string(c.histogram->rows) + " values):";
            for (double b : c.histogram->bounds) line += " " + number(b);
            out += line + "\n";
        }
    }
    if (stats_are_stale(table, stats))
        out += "warning: the table has " + std::to_string(table.rows().size()) + " rows now; run ANALYZE " + info.name + " again\n";
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace cardinal
