#pragma once

#include <stdexcept>

namespace cardinal {

// A statement was understood but cannot be carried out: unknown table, wrong
// column count, and so on. Syntax errors are ParseError instead.
class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace cardinal
