#pragma once

#include <string>

#include "sql/ast.h"

namespace cardinal {

// Bracket form, one line, e.g.
//   (select (col name) (from users) (where (> (col age) 25)))
// Parser tests compare against this text.
std::string print(const Expr& expr);
std::string print(const Statement& statement);

}  // namespace cardinal
