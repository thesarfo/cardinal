#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "sql/ast.h"

namespace cardinal {

// Thrown by the lexer-to-tree steps. `line` and `column` are 1-based and point at
// the token that could not be used. what() reads like "expected ')' at 1:34".
class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, int line, int column)
        : std::runtime_error(message + " at " + std::to_string(line) + ":" +
                             std::to_string(column)),
          message_(std::move(message)),
          line_(line),
          column_(column) {}

    const std::string& message() const { return message_; }
    int line() const { return line_; }
    int column() const { return column_; }

private:
    std::string message_;
    int line_;
    int column_;
};

// Parses one expression and requires nothing after it. Throws ParseError.
ExprPtr parse_expression(std::string_view sql);

// Parses one statement, with an optional trailing ';'. Throws ParseError.
Statement parse_statement(std::string_view sql);

// Shows an error with the offending line and a caret under the bad token:
//   error: expected ')' at 1:7
//     (1 + 2
//           ^
std::string format_error(std::string_view sql, const ParseError& error);

}  // namespace cardinal
