#include "storage/table.h"

#include <string>

#include "common/error.h"

namespace cardinal {

void Table::fit(Row& row) const {
    if (row.size() != info_.columns.size())
        throw DbError("table " + info_.name + " has " + std::to_string(info_.columns.size()) +
                      " columns but " + std::to_string(row.size()) + " values were given");

    for (std::size_t i = 0; i < row.size(); ++i) {
        const ColumnInfo& column = info_.columns[i];
        Value& value = row[i];
        if (value.is_null() || value.type() == column.type) continue;
        if (value.type() == Type::Int && column.type == Type::Double) {
            value = Value(static_cast<double>(value.as_int()));
            continue;
        }
        throw DbError("column " + column.name + " is " + type_name(column.type) + " but got " +
                      type_name(value.type()) + " " + value.to_string());
    }
}

void Table::insert(Row row) {
    fit(row);
    for (auto& [name, entry] : indexes_) entry.index.insert(row[entry.column_index], rows_.size());
    rows_.push_back(std::move(row));
}

void Table::insert_rows(std::vector<Row> rows) {
    for (Row& row : rows) fit(row);
    for (Row& row : rows) {
        for (auto& [name, entry] : indexes_) entry.index.insert(row[entry.column_index], rows_.size());
        rows_.push_back(std::move(row));
    }
}

void Table::create_index(const std::string& name, const std::string& column) {
    if (indexes_.contains(name)) throw DbError("index " + name + " already exists");
    std::optional<std::size_t> at = info_.find_column(column);
    if (!at) throw DbError("table " + info_.name + " has no column " + column);
    IndexEntry entry{column, *at, {}};
    for (std::size_t i = 0; i < rows_.size(); ++i) entry.index.insert(rows_[i][*at], i);
    indexes_.emplace(name, std::move(entry));
}

const Table::IndexEntry* Table::index_on(const std::string& column) const {
    for (const auto& [name, entry] : indexes_)
        if (entry.column == column) return &entry;
    return nullptr;
}

}  // namespace cardinal
