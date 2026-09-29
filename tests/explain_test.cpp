#include <catch2/catch_test_macros.hpp>

#include "common/error.h"
#include "engine/database.h"
#include "engine/result_format.h"

using namespace cardinal;

namespace {

std::string explain(Database& db, const std::string& sql) {
    QueryResult r = db.execute("EXPLAIN " + sql);
    REQUIRE_FALSE(r.returns_rows());
    return format_result(r);
}

Database users_db() {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41), (4, 'yaw', NULL)");
    return db;
}

}  // namespace

TEST_CASE("explain: the demo query") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE age > 20 + 5 AND TRUE") ==
            "Original plan\n"
            "  Project[name]\n"
            "    Filter[age > 20 + 5 AND TRUE]\n"
            "      Scan[users]\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. boolean-cleanup (pass 1)\n"
            "\n"
            "Final plan\n"
            "  Project[name]\n"
            "    Filter[age > 25]\n"
            "      Scan[users]");
}

TEST_CASE("explain: nothing to do") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE age > 25 ORDER BY id LIMIT 2") ==
            "Original plan\n"
            "  Limit[2]\n"
            "    Project[name]\n"
            "      Sort[id ASC]\n"
            "        Filter[age > 25]\n"
            "          Scan[users]\n"
            "\n"
            "Rules fired\n"
            "  none\n"
            "\n"
            "Final plan\n"
            "  Limit[2]\n"
            "    Project[name]\n"
            "      Sort[id ASC]\n"
            "        Filter[age > 25]\n"
            "          Scan[users]");
}

TEST_CASE("explain: a filter that can never pass becomes Empty") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE 1 > 2") ==
            "Original plan\n"
            "  Project[name]\n"
            "    Filter[1 > 2]\n"
            "      Scan[users]\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. empty-false-filter (pass 1)\n"
            "\n"
            "Final plan\n"
            "  Project[name]\n"
            "    Empty");
}

TEST_CASE("explain: a filter that always passes disappears") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE 1 = 1") ==
            "Original plan\n"
            "  Project[name]\n"
            "    Filter[1 = 1]\n"
            "      Scan[users]\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. remove-true-filter (pass 1)\n"
            "\n"
            "Final plan\n"
            "  Project[name]\n"
            "    Scan[users]");
}

TEST_CASE("explain: does not run the query, and reports mistakes") {
    Database db = users_db();
    explain(db, "SELECT name FROM users WHERE 1 / 0 = 1");  // would throw if it ran
    REQUIRE_THROWS_AS(db.execute("EXPLAIN SELECT nope FROM users"), DbError);
    REQUIRE_THROWS_AS(db.execute("EXPLAIN SELECT * FROM nope"), DbError);
}

TEST_CASE("the optimizer never changes what a query returns") {
    Database on = users_db();
    Database off = users_db();
    off.set_optimizer_enabled(false);
    const char* queries[] = {"SELECT name FROM users WHERE age > 20 + 5 AND TRUE",
                             "SELECT name FROM users WHERE 1 > 2",
                             "SELECT name FROM users WHERE 1 = 1 ORDER BY id DESC",
                             "SELECT id, 2 * 3 FROM users WHERE age > 25 OR FALSE ORDER BY id LIMIT 2",
                             "SELECT name FROM users WHERE age IS NULL OR age = age",
                             "SELECT name FROM users WHERE NOT (age > 25 AND TRUE)",
                             "SELECT name FROM users WHERE age BETWEEN 10 + 10 AND 40 AND age BETWEEN 10 + 10 AND 40"};
    for (const char* sql : queries) {
        INFO(sql);
        QueryResult a = on.execute(sql);
        QueryResult b = off.execute(sql);
        REQUIRE(a.columns == b.columns);
        REQUIRE(a.rows == b.rows);
    }
}

TEST_CASE("the optimizer runs on SELECT") {
    Database db = users_db();
    REQUIRE(db.execute("SELECT name FROM users WHERE 1 > 2").rows.empty());
    REQUIRE(db.execute("SELECT name FROM users WHERE 1 > 2").columns == std::vector<std::string>{"name"});
}
