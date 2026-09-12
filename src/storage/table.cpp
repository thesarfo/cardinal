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
    rows_.push_back(std::move(row));
}

void Table::insert_rows(std::vector<Row> rows) {
    for (Row& row : rows) fit(row);
    for (Row& row : rows) rows_.push_back(std::move(row));
}

}  // namespace cardinal
