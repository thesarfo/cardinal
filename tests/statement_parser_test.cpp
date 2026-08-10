#include <catch2/catch_test_macros.hpp>

#include "sql/ast_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

std::string parsed(const char* sql) { return print(parse_statement(sql)); }

void require_error(const char* sql, const char* message, int line, int column) {
    INFO(sql);
    try {
        parse_statement(sql);
        FAIL("expected a ParseError");
    } catch (const ParseError& e) {
        REQUIRE(e.message() == message);
        REQUIRE(e.line() == line);
        REQUIRE(e.column() == column);
    }
}

}  // namespace

TEST_CASE("statement: create table") {
    REQUIRE(parsed("CREATE TABLE users (id INT, name TEXT, score DOUBLE, active BOOL)") ==
            "(create-table users (id int) (name text) (score double) (active bool))");
    REQUIRE(parsed("create table t (a int);") == "(create-table t (a int))");
}

TEST_CASE("statement: create table errors") {
    require_error("CREATE TABLE (a INT)", "expected a table name", 1, 14);
    require_error("CREATE TABLE t ()", "expected a column name", 1, 17);
    require_error("CREATE TABLE t (a)", "expected a column type (INT, DOUBLE, TEXT or BOOL)", 1, 18);
    require_error("CREATE TABLE t (a INT", "expected ')'", 1, 22);
    require_error("CREATE t (a INT)", "expected 'TABLE'", 1, 8);
}

TEST_CASE("statement: insert") {
    REQUIRE(parsed("INSERT INTO t VALUES (1, 'x', NULL, TRUE, 2.5)") ==
            "(insert t (row 1 'x' null true 2.5))");
    REQUIRE(parsed("INSERT INTO t VALUES (1, -2), (3, 4 + 5);") ==
            "(insert t (row 1 (neg 2)) (row 3 (+ 4 5)))");
}

TEST_CASE("statement: insert errors") {
    require_error("INSERT t VALUES (1)", "expected 'INTO'", 1, 8);
    require_error("INSERT INTO t (1)", "expected 'VALUES'", 1, 15);
    require_error("INSERT INTO t VALUES 1", "expected '('", 1, 22);
    require_error("INSERT INTO t VALUES ()", "expected an expression", 1, 23);
    require_error("INSERT INTO t VALUES (1,)", "expected an expression", 1, 25);
    require_error("INSERT INTO t VALUES (1) (2)", "expected end of input", 1, 26);
}

TEST_CASE("statement: select") {
    REQUIRE(parsed("SELECT * FROM users") == "(select (star) (from users))");
    REQUIRE(parsed("SELECT name FROM users WHERE age > 25") ==
            "(select (col name) (from users) (where (> (col age) 25)))");
    REQUIRE(parsed("SELECT a, b + 1 FROM t") == "(select (col a) (+ (col b) 1) (from t))");
    REQUIRE(parsed("SELECT a FROM t ORDER BY a") == "(select (col a) (from t) (order-by (asc (col a))))");
    REQUIRE(parsed("SELECT * FROM t ORDER BY a DESC, b ASC, c LIMIT 5;") ==
            "(select (star) (from t) (order-by (desc (col a)) (asc (col b)) (asc (col c))) "
            "(limit 5))");
    REQUIRE(parsed("SELECT * FROM t WHERE a = 1 AND b IN (1, 2) ORDER BY a LIMIT 0") ==
            "(select (star) (from t) (where (and (= (col a) 1) (in (col b) 1 2))) "
            "(order-by (asc (col a))) (limit 0))");
    REQUIRE(parsed("select 1 from t") == "(select 1 (from t))");
}

TEST_CASE("statement: select errors") {
    require_error("SELECT FROM t", "expected an expression", 1, 8);
    require_error("SELECT * t", "expected 'FROM'", 1, 10);
    require_error("SELECT * FROM", "expected a table name", 1, 14);
    require_error("SELECT * FROM t WHERE", "expected an expression", 1, 22);
    require_error("SELECT * FROM t ORDER a", "expected 'BY'", 1, 23);
    require_error("SELECT * FROM t LIMIT x", "expected a whole number after LIMIT", 1, 23);
    require_error("SELECT * FROM t LIMIT 1.5", "expected a whole number after LIMIT", 1, 23);
    require_error("SELECT * FROM t LIMIT -1", "expected a whole number after LIMIT", 1, 23);
    require_error("SELECT *, a FROM t", "expected 'FROM'", 1, 9);
    require_error("SELECT * FROM t WHERE a = 1 ORDER BY a LIMIT 1 2", "expected end of input", 1, 48);
}

TEST_CASE("statement: explain") {
    REQUIRE(parsed("EXPLAIN SELECT * FROM t") == "(explain (select (star) (from t)))");
    REQUIRE(parsed("explain select a from t where a = 1;") ==
            "(explain (select (col a) (from t) (where (= (col a) 1))))");
    require_error("EXPLAIN EXPLAIN SELECT * FROM t", "expected a statement", 1, 9);
    require_error("EXPLAIN", "expected a statement", 1, 8);
}

TEST_CASE("statement: not a statement") {
    require_error("", "expected a statement", 1, 1);
    require_error("SELEC * FROM t", "expected a statement", 1, 1);
    require_error("42", "expected a statement", 1, 1);
    require_error("DROP TABLE t", "expected a statement", 1, 1);
}

TEST_CASE("statement: errors on later lines") {
    require_error("SELECT *\nFROM t\nWHERE", "expected an expression", 3, 6);
}
