#include <catch2/catch_test_macros.hpp>

#include "sql/parser.h"

using namespace cardinal;

namespace {

std::string shown(const char* sql) {
    try {
        parse_statement(sql);
    } catch (const ParseError& e) {
        return format_error(sql, e);
    }
    return "no error";
}

}  // namespace

TEST_CASE("error display: caret under the bad token") {
    REQUIRE(shown("SELEC * FROM t") ==
            "error: expected a statement at 1:1\n"
            "  SELEC * FROM t\n"
            "  ^");
    REQUIRE(shown("SELECT * FROM t WHERE (a = 1") ==
            "error: expected ')' at 1:29\n"
            "  SELECT * FROM t WHERE (a = 1\n"
            "                              ^");
}

TEST_CASE("error display: later lines show only their own line") {
    REQUIRE(shown("SELECT *\nFROM t\nWHERE a +") ==
            "error: expected an expression at 3:10\n"
            "  WHERE a +\n"
            "           ^");
    REQUIRE(shown("SELECT *\nFROM") ==
            "error: expected a table name at 2:5\n"
            "  FROM\n"
            "      ^");
}

TEST_CASE("error display: tabs keep the caret aligned") {
    REQUIRE(shown("SELECT\t* FROM") ==
            "error: expected a table name at 1:14\n"
            "  SELECT * FROM\n"
            "               ^");
}

TEST_CASE("error display: lexer errors") {
    REQUIRE(shown("SELECT 'oops") ==
            "error: unterminated string at 1:8\n"
            "  SELECT 'oops\n"
            "         ^");
}
