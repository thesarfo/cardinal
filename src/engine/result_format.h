#pragma once

#include <string>

#include "engine/database.h"

namespace cardinal {

// A result as an aligned text table, ending with "(N rows)", or just the message
// for statements that return no rows. No trailing newline.
//   id | name
//   ---+-----
//   1  | ama
//   (1 row)
std::string format_result(const QueryResult& result);

}  // namespace cardinal
