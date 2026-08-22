#pragma once

#include <string>

#include "sql/ast.h"

namespace cardinal {

// Bracket form, one line, e.g.
//   (select (col name) (from users) (where (> (col age) 25)))
// Parser tests compare against this text.
std::string print(const Expr& expr);
std::string print(const Statement& statement);

// Shared with the bound-expression printer.
const char* binary_op_name(BinaryOp op);
const char* unary_op_name(UnaryOp op);
std::string format_literal(const Value& value);

}  // namespace cardinal
