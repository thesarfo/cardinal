#include <catch2/catch_test_macros.hpp>

#include "common/error.h"
#include "engine/database.h"
#include "engine/result_format.h"
#include "sql/ast_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Database orders_db() {
    Database db;
    db.execute("CREATE TABLE users (id INT, city TEXT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("INSERT INTO users VALUES (1, 'accra'), (2, 'accra'), (3, 'tamale')");
    db.execute("INSERT INTO orders VALUES (1, 1, 5.0), (2, 1, 2.5), (3, 2, 4.0), (4, 3, NULL), (5, NULL, 1.0)");
    return db;
}

std::string explain(Database& db, const std::string& sql) { return format_result(db.execute("EXPLAIN " + sql)); }

}  // namespace

TEST_CASE("aggregate: the parser reads GROUP BY, HAVING, calls and aliases") {
    REQUIRE(print(parse_statement("SELECT a, COUNT(*) AS n, SUM(b + 1) FROM t GROUP BY a, c HAVING COUNT(*) > 1")) ==
            "(select (col a) (call count *) (as n) (call sum (+ (col b) 1)) (from t) "
            "(group-by (col a) (col c)) (having (> (call count *) 1)))");
}

TEST_CASE("aggregate: a name that is not a function call stays a column") {
    Database db;
    db.execute("CREATE TABLE t (count INT, sum INT)");
    db.execute("INSERT INTO t VALUES (3, 4)");
    QueryResult r = db.execute("SELECT count, sum, COUNT(*) FROM t GROUP BY count, sum");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows[0] == Row{Value(std::int64_t{3}), Value(std::int64_t{4}), Value(std::int64_t{1})});
}

TEST_CASE("aggregate: the result of each total, on a group with a NULL") {
    Database db = orders_db();
    QueryResult r = db.execute("SELECT COUNT(*), COUNT(amount), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM orders");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.columns == std::vector<std::string>{"count(*)", "count(amount)", "sum(amount)", "avg(amount)", "min(amount)", "max(amount)"});
    REQUIRE(r.rows[0] == Row{Value(std::int64_t{5}), Value(std::int64_t{4}), Value(12.5), Value(3.125), Value(1.0), Value(5.0)});
}

TEST_CASE("aggregate: a group with only NULLs totals to NULL, and counts to 0") {
    Database db = orders_db();
    QueryResult r = db.execute("SELECT user_id, COUNT(amount), SUM(amount), AVG(amount), MIN(amount) FROM orders WHERE user_id = 3 GROUP BY user_id");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows[0] == Row{Value(std::int64_t{3}), Value(std::int64_t{0}), Value(), Value(), Value()});
}

TEST_CASE("aggregate: no GROUP BY gives one row from an empty input, GROUP BY gives none") {
    Database db = orders_db();
    QueryResult one = db.execute("SELECT COUNT(*), SUM(amount) FROM orders WHERE id > 100");
    REQUIRE(one.rows.size() == 1);
    REQUIRE(one.rows[0] == Row{Value(std::int64_t{0}), Value()});
    REQUIRE(db.execute("SELECT user_id, COUNT(*) FROM orders WHERE id > 100 GROUP BY user_id").rows.empty());
}

TEST_CASE("aggregate: NULL keys make one group, and a computed key groups by value") {
    Database db;
    db.execute("CREATE TABLE t (a INT, b DOUBLE)");
    db.execute("INSERT INTO t VALUES (NULL, 1.0), (NULL, 2.0), (1, 1.0), (1, 1.5), (2, NULL)");
    QueryResult nulls = db.execute("SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY a");
    REQUIRE(nulls.rows.size() == 3);
    REQUIRE(nulls.rows[0] == Row{Value(), Value(std::int64_t{2})});

    // The key is a + 0.0: 1 and 1.0 would be one group if both appeared (a column has one type).
    QueryResult mixed = db.execute("SELECT COUNT(*) FROM t GROUP BY a + 0.0");
    REQUIRE(mixed.rows.size() == 3);
}

TEST_CASE("aggregate: groups are in the order they first appeared") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    db.execute("INSERT INTO t VALUES (3), (1), (3), (2), (1)");
    QueryResult r = db.execute("SELECT a FROM t GROUP BY a");
    REQUIRE(r.rows == std::vector<Row>{{Value(std::int64_t{3})}, {Value(std::int64_t{1})}, {Value(std::int64_t{2})}});
}

TEST_CASE("aggregate: HAVING and ORDER BY can use totals that are not selected") {
    Database db = orders_db();
    QueryResult r = db.execute("SELECT user_id FROM orders GROUP BY user_id HAVING COUNT(*) > 1 ORDER BY SUM(amount) DESC");
    REQUIRE(r.rows == std::vector<Row>{{Value(std::int64_t{1})}});
}

TEST_CASE("aggregate: an alias can be sorted by, and a select-list expression over a key works") {
    Database db = orders_db();
    QueryResult r = db.execute("SELECT user_id + 100 AS shifted, COUNT(*) AS n FROM orders GROUP BY user_id ORDER BY n DESC, shifted");
    REQUIRE(r.columns == std::vector<std::string>{"shifted", "n"});
    REQUIRE(r.rows.front() == Row{Value(std::int64_t{101}), Value(std::int64_t{2})});
}

TEST_CASE("aggregate: whole-number SUM that overflows is an error, AVG is not") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    db.execute("INSERT INTO t VALUES (9223372036854775807), (1)");
    REQUIRE_THROWS_AS(db.execute("SELECT SUM(a) FROM t"), DbError);
    QueryResult avg = db.execute("SELECT AVG(a) FROM t");
    REQUIRE(avg.rows.size() == 1);
    REQUIRE(avg.rows[0][0].type() == Type::Double);
}

TEST_CASE("aggregate: bad queries say what is wrong") {
    Database db = orders_db();
    auto message = [&](const std::string& sql) {
        try {
            db.execute(sql);
        } catch (const DbError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    REQUIRE(message("SELECT user_id, amount FROM orders GROUP BY user_id") ==
            "column orders.amount must appear in GROUP BY or be used in an aggregate");
    REQUIRE(message("SELECT id FROM orders WHERE SUM(amount) > 1") == "aggregate functions are not allowed in WHERE");
    REQUIRE(message("SELECT * FROM orders GROUP BY SUM(amount)") == "aggregate functions are not allowed in GROUP BY");
    REQUIRE(message("SELECT SUM(COUNT(*)) FROM orders") == "aggregate functions are not allowed in an aggregate's argument");
    REQUIRE(message("SELECT id FROM orders JOIN users ON COUNT(*) > 1") == "aggregate functions are not allowed in ON");
    REQUIRE(message("SELECT AVG(city) FROM users") == "avg needs numbers, got TEXT");
    REQUIRE(message("SELECT MAX(*) FROM orders") == "only COUNT can take *, not MAX");
    REQUIRE(message("SELECT NOPE(id) FROM orders") == "unknown function NOPE");
}

TEST_CASE("aggregate: EXPLAIN shows the Aggregate with its row guess") {
    Database db = orders_db();
    db.execute("ANALYZE orders");
    std::string out = explain(db, "SELECT user_id, SUM(amount) FROM orders GROUP BY user_id HAVING COUNT(*) > 1");
    REQUIRE(out.find("Aggregate[by user_id; sum(amount), count(*)]  est_rows=4") != std::string::npos);
    // 3 distinct user_ids and a NULL one.
    REQUIRE(explain(db, "SELECT COUNT(*) FROM orders").find("Aggregate[one group; count(*)]  est_rows=1") != std::string::npos);
}

TEST_CASE("aggregate: a WHERE condition on one table still goes below the join") {
    Database db = orders_db();
    std::string out = explain(db, "SELECT u.city, COUNT(*) FROM users u JOIN orders o ON u.id = o.user_id WHERE u.city = 'accra' GROUP BY u.city");
    std::size_t final_plan = out.find("Final plan");
    REQUIRE(final_plan != std::string::npos);
    std::string tail = out.substr(final_plan);
    std::size_t join = tail.find("Join[");
    std::size_t filter = tail.find("Filter[city = 'accra'");
    REQUIRE(join != std::string::npos);
    REQUIRE(filter != std::string::npos);
    REQUIRE(filter > join);  // the filter is printed under the join, so it runs first
}

TEST_CASE("aggregate: the same answer with the optimizer off") {
    Database db = orders_db();
    const std::string sql =
        "SELECT u.city, COUNT(*), SUM(o.amount) FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 1 GROUP BY u.city ORDER BY u.city";
    std::vector<Row> on = db.execute(sql).rows;
    db.set_optimizer_enabled(false);
    REQUIRE(db.execute(sql).rows == on);
}
