#include <catch2/catch_test_macros.hpp>

#include "common/error.h"
#include "engine/database.h"
#include "engine/result_format.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

}  // namespace

TEST_CASE("database: create, insert, select") {
    Database db;
    REQUIRE(db.execute("CREATE TABLE t (id INT, name TEXT)").message == "CREATE TABLE");
    REQUIRE(db.execute("INSERT INTO t VALUES (1, 'ama'), (2, 'kofi')").message == "INSERT 2");

    QueryResult r = db.execute("SELECT name, id * 10 FROM t WHERE id > 1");
    REQUIRE(r.returns_rows());
    REQUIRE(r.columns == std::vector<std::string>{"name", "?column?"});
    REQUIRE(r.rows == std::vector<Row>{{s("kofi"), i(20)}});
}

TEST_CASE("database: a select with no matches still has its columns") {
    Database db;
    db.execute("CREATE TABLE t (id INT)");
    QueryResult r = db.execute("SELECT id FROM t");
    REQUIRE(r.returns_rows());
    REQUIRE(r.rows.empty());
}

TEST_CASE("database: insert values are expressions") {
    Database db;
    db.execute("CREATE TABLE t (a INT, b DOUBLE, c TEXT)");
    db.execute("INSERT INTO t VALUES (1 + 2, -4, NULL)");
    QueryResult r = db.execute("SELECT * FROM t");
    REQUIRE(r.rows == std::vector<Row>{{i(3), Value(-4.0), Value()}});
}

TEST_CASE("database: a failed insert adds nothing") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    db.execute("INSERT INTO t VALUES (1)");
    REQUIRE_THROWS_AS(db.execute("INSERT INTO t VALUES (2), (3), ('x')"), DbError);
    REQUIRE_THROWS_AS(db.execute("INSERT INTO t VALUES (4), (5, 6)"), DbError);
    REQUIRE_THROWS_AS(db.execute("INSERT INTO t VALUES (1 / 0)"), DbError);
    REQUIRE(db.execute("SELECT a FROM t").rows.size() == 1);
}

TEST_CASE("database: errors") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    REQUIRE_THROWS_AS(db.execute("SELEC * FROM t"), ParseError);
    REQUIRE_THROWS_AS(db.execute("SELECT * FROM nope"), DbError);
    REQUIRE_THROWS_AS(db.execute("INSERT INTO nope VALUES (1)"), DbError);
    REQUIRE_THROWS_AS(db.execute("CREATE TABLE t (a INT)"), DbError);
    REQUIRE_THROWS_AS(db.execute("INSERT INTO t VALUES (a)"), DbError);  // no columns to read
    REQUIRE_THROWS_AS(db.execute("EXPLAIN SELECT * FROM nope"), DbError);
    REQUIRE_THROWS_AS(db.execute("EXPLAIN CREATE TABLE u (a INT)"), DbError);
}

TEST_CASE("database: a runtime error comes out of the query") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    db.execute("INSERT INTO t VALUES (1), (0)");
    REQUIRE_THROWS_AS(db.execute("SELECT 10 / a FROM t"), DbError);
}

TEST_CASE("database: the M2 demo, a hundred rows filtered, sorted and limited") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    std::string insert = "INSERT INTO users VALUES ";
    for (int n = 1; n <= 100; ++n) {
        if (n > 1) insert += ", ";
        insert += "(" + std::to_string(n) + ", 'user" + std::to_string(n) + "', " + std::to_string(18 + n % 40) + ")";
    }
    REQUIRE(db.execute(insert).message == "INSERT 100");

    QueryResult r = db.execute("SELECT name, age FROM users WHERE age > 50 ORDER BY age DESC, id LIMIT 3");
    // ages run 18..57; the oldest are 57 (n % 40 == 39), so ids 39 and 79, then 56 (ids 38, 78)
    REQUIRE(r.rows == std::vector<Row>{{s("user39"), i(57)}, {s("user79"), i(57)}, {s("user38"), i(56)}});
}

TEST_CASE("format: table layout") {
    QueryResult r;
    r.columns = {"id", "name"};
    r.rows = {{i(1), s("ama")}, {i(20), Value()}};
    REQUIRE(format_result(r) ==
            "id | name\n"
            "---+-----\n"
            "1  | ama\n"
            "20 | NULL\n"
            "(2 rows)");
}

TEST_CASE("format: wide values, one row, no rows, messages") {
    QueryResult r;
    r.columns = {"n", "x"};
    r.rows = {{i(1), Value(2.5)}};
    REQUIRE(format_result(r) ==
            "n | x\n"
            "--+----\n"
            "1 | 2.5\n"
            "(1 row)");
    r.rows.clear();
    REQUIRE(format_result(r) ==
            "n | x\n"
            "--+--\n"
            "(0 rows)");
    QueryResult m;
    m.message = "INSERT 3";
    REQUIRE(format_result(m) == "INSERT 3");
}
