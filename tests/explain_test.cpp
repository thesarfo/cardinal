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
            "  Project[name]  est_rows=1\n"
            "    Filter[age > 20 + 5 AND TRUE]  est_rows=1\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. boolean-cleanup (pass 1)\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=1\n"
            "    Filter[age > 25]  est_rows=1\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Statistics\n"
            "  users: not analyzed, so the guesses use default numbers");
}

TEST_CASE("explain: nothing to do") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE age > 25 ORDER BY id LIMIT 2") ==
            "Original plan\n"
            "  Limit[2]  est_rows=1\n"
            "    Project[name]  est_rows=1\n"
            "      Sort[id ASC]  est_rows=1\n"
            "        Filter[age > 25]  est_rows=1\n"
            "          Scan[users]  est_rows=4\n"
            "\n"
            "Rules fired\n"
            "  none\n"
            "\n"
            "Final plan\n"
            "  Limit[2]  est_rows=1\n"
            "    Project[name]  est_rows=1\n"
            "      Sort[id ASC]  est_rows=1\n"
            "        Filter[age > 25]  est_rows=1\n"
            "          Scan[users]  est_rows=4\n"
            "\n"
            "Statistics\n"
            "  users: not analyzed, so the guesses use default numbers");
}

TEST_CASE("explain: a filter that can never pass becomes Empty") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE 1 > 2") ==
            "Original plan\n"
            "  Project[name]  est_rows=1\n"
            "    Filter[1 > 2]  est_rows=1\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. empty-false-filter (pass 1)\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=0\n"
            "    Empty  est_rows=0\n"
            "\n"
            "Statistics\n"
            "  users: not analyzed, so the guesses use default numbers");
}

TEST_CASE("explain: a filter that always passes disappears") {
    Database db = users_db();
    REQUIRE(explain(db, "SELECT name FROM users WHERE 1 = 1") ==
            "Original plan\n"
            "  Project[name]  est_rows=1\n"
            "    Filter[1 = 1]  est_rows=1\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. remove-true-filter (pass 1)\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=4\n"
            "    Scan[users]  est_rows=4\n"
            "\n"
            "Statistics\n"
            "  users: not analyzed, so the guesses use default numbers");
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

TEST_CASE("explain: estimates come from the statistics once the table is analyzed") {
    Database db = users_db();
    std::string before = explain(db, "SELECT name FROM users WHERE age > 25");
    db.execute("ANALYZE users");
    std::string after = explain(db, "SELECT name FROM users WHERE age > 25");
    REQUIRE(before.find("Filter[age > 25]  est_rows=1") != std::string::npos);  // 4 rows x 1/3, a guess
    REQUIRE(after.find("Filter[age > 25]  est_rows=") != std::string::npos);
    REQUIRE(after.find("users: analyzed when it had 4 rows") != std::string::npos);
    REQUIRE(after.find("not analyzed") == std::string::npos);
}

TEST_CASE("explain: a table that has changed since ANALYZE gets a warning, and its scan the new size") {
    Database db = users_db();
    db.execute("ANALYZE users");
    db.execute("INSERT INTO users VALUES (5, 'yaa', 33), (6, 'kwame', 29)");
    std::string text = explain(db, "SELECT name FROM users");
    REQUIRE(text.find("Scan[users]  est_rows=6") != std::string::npos);
    REQUIRE(text.find("users: analyzed when it had 4 rows") != std::string::npos);
    REQUIRE(text.find("warning: users has 6 rows now; run ANALYZE users") != std::string::npos);

    db.execute("ANALYZE users");
    REQUIRE(explain(db, "SELECT name FROM users").find("warning") == std::string::npos);
}

TEST_CASE("explain: a self-join lists its table once") {
    Database db = users_db();
    std::string text = explain(db, "SELECT a.name FROM users a JOIN users b ON a.id = b.id");
    std::size_t first = text.find("users: not analyzed");
    REQUIRE(first != std::string::npos);
    REQUIRE(text.find("users: not analyzed", first + 1) == std::string::npos);
}

TEST_CASE("explain: every line of both plans has a guess") {
    Database db = users_db();
    db.execute("ANALYZE users");
    std::string text = explain(db, "SELECT name FROM users WHERE age > 20 + 5 ORDER BY id LIMIT 2");
    std::size_t plan_lines = 0, with_estimate = 0;
    std::size_t statistics = text.find("Statistics");
    std::size_t pos = 0;
    while (pos < statistics) {
        std::size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end - pos);
        if (line.find("Project[") != std::string::npos || line.find("Filter[") != std::string::npos ||
            line.find("Sort[") != std::string::npos || line.find("Limit[") != std::string::npos ||
            line.find("Scan[") != std::string::npos) {
            ++plan_lines;
            if (line.find("est_rows=") != std::string::npos) ++with_estimate;
        }
        pos = end + 1;
    }
    REQUIRE(plan_lines == 10);  // five steps, in the original plan and the final plan
    REQUIRE(with_estimate == plan_lines);
}
