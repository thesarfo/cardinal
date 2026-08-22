#include "binder/scope.h"

#include "common/error.h"

namespace cardinal {

void Scope::add(std::string alias, const TableInfo& info) {
    for (const BoundTable& t : tables_)
        if (t.alias == alias) throw DbError("table name " + alias + " is used twice; give one an alias");

    BoundTable table{alias, &info, {}};
    for (const ColumnInfo& column : info.columns) {
        table.columns.push_back(ColumnId{static_cast<std::uint32_t>(columns_.size())});
        columns_.push_back({alias, column.name, column.type});
    }
    tables_.push_back(std::move(table));
}

ColumnId Scope::resolve(const std::optional<std::string>& table, const std::string& name) const {
    if (table) {
        for (const BoundTable& t : tables_) {
            if (t.alias != *table) continue;
            auto index = t.info->find_column(name);
            if (!index) throw DbError("unknown column " + *table + "." + name);
            return t.columns[*index];
        }
        throw DbError("unknown table or alias " + *table);
    }

    std::optional<ColumnId> found;
    std::string where;
    for (const BoundTable& t : tables_) {
        auto index = t.info->find_column(name);
        if (!index) continue;
        if (found) {
            throw DbError("column " + name + " is ambiguous: it could be " + where + " or " +
                          t.alias + "." + name);
        }
        found = t.columns[*index];
        where = t.alias + "." + name;
    }
    if (!found) throw DbError("unknown column " + name);
    return *found;
}

}  // namespace cardinal
