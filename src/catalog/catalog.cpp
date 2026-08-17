#include "catalog/catalog.h"

#include <set>

#include "common/error.h"

namespace cardinal {

Table& Catalog::create_table(TableInfo info) {
    if (tables_.contains(info.name)) throw DbError("table " + info.name + " already exists");

    std::set<std::string> seen;
    for (const ColumnInfo& column : info.columns)
        if (!seen.insert(column.name).second)
            throw DbError("table " + info.name + " has two columns named " + column.name);

    std::string name = info.name;
    return tables_.emplace(std::move(name), Table(std::move(info))).first->second;
}

Table* Catalog::get_table(const std::string& name) {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

const Table* Catalog::get_table(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

}  // namespace cardinal
