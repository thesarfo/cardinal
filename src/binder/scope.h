#pragma once

#include <optional>
#include <string>
#include <vector>

#include "binder/bound_expr.h"
#include "catalog/table_info.h"

namespace cardinal {

struct ColumnMeta {
    std::string table;  // the alias the query used
    std::string name;
    Type type;
};

// A table as used in one query, under an alias (its own name if none was given).
struct BoundTable {
    std::string alias;
    const TableInfo* info;
    std::vector<ColumnId> columns;  // same order as info->columns
};

// The tables a query can see and the ids handed out for their columns.
class Scope {
public:
    // Gives every column of `info` a fresh id. Throws DbError if `alias` is taken.
    void add(std::string alias, const TableInfo& info);

    // Finds a column by `name` or `table.name`. Throws DbError if it is unknown or,
    // for a bare name, matches more than one table.
    ColumnId resolve(const std::optional<std::string>& table, const std::string& name) const;

    const std::vector<BoundTable>& tables() const { return tables_; }
    // Indexed by ColumnId::value.
    const std::vector<ColumnMeta>& columns() const { return columns_; }
    const ColumnMeta& meta(ColumnId id) const { return columns_[id.value]; }

private:
    std::vector<BoundTable> tables_;
    std::vector<ColumnMeta> columns_;
};

}  // namespace cardinal
