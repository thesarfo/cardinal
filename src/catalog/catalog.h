#pragma once

#include <map>
#include <string>

#include "catalog/table_info.h"
#include "storage/table.h"

namespace cardinal {

// Owns every table. Table names are matched exactly, case included.
class Catalog {
public:
    // Throws DbError if the name is taken, or a column name appears twice.
    Table& create_table(TableInfo info);

    // Null if there is no such table.
    Table* get_table(const std::string& name);
    const Table* get_table(const std::string& name) const;

private:
    std::map<std::string, Table> tables_;
};

}  // namespace cardinal
