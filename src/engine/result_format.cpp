#include "engine/result_format.h"

#include <algorithm>

namespace cardinal {

namespace {

std::string pad(const std::string& s, std::size_t width) {
    return s + std::string(width > s.size() ? width - s.size() : 0, ' ');
}

}  // namespace

std::string format_result(const QueryResult& result) {
    if (!result.returns_rows()) return result.message;

    std::vector<std::size_t> widths;
    for (const std::string& name : result.columns) widths.push_back(name.size());
    std::vector<std::vector<std::string>> cells;
    for (const Row& row : result.rows) {
        std::vector<std::string> line;
        for (std::size_t i = 0; i < row.size(); ++i) {
            line.push_back(row[i].to_string());
            widths[i] = std::max(widths[i], line.back().size());
        }
        cells.push_back(std::move(line));
    }

    auto join = [&](const std::vector<std::string>& line) {
        std::string out;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (i > 0) out += " | ";
            // The last column needs no padding.
            out += i + 1 < line.size() ? pad(line[i], widths[i]) : line[i];
        }
        return out;
    };

    std::string out = join(result.columns) + "\n";
    for (std::size_t i = 0; i < widths.size(); ++i) {
        if (i > 0) out += "-+-";
        out += std::string(widths[i], '-');
    }
    out += "\n";
    for (const auto& line : cells) out += join(line) + "\n";
    out += "(" + std::to_string(result.rows.size()) + (result.rows.size() == 1 ? " row)" : " rows)");
    return out;
}

}  // namespace cardinal
