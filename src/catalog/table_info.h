#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/value.h"

namespace cardinal {

struct ColumnInfo {
    std::string name;
    Type type;
};

// What a table looks like. Names are matched exactly, case included.
struct TableInfo {
    std::string name;
    std::vector<ColumnInfo> columns;

    std::optional<std::size_t> find_column(const std::string& column) const {
        for (std::size_t i = 0; i < columns.size(); ++i)
            if (columns[i].name == column) return i;
        return std::nullopt;
    }
};

}  // namespace cardinal
