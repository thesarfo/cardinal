#include <catch2/catch_test_macros.hpp>

#include <regex>

#include "common/error.h"
#include "engine/database.h"
#include "engine/datagen.h"
#include "engine/result_format.h"
#include "sql/ast_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

std::string explain(Database& db, const std::string& prefix, const std::string& sql) {
    return format_result(db.execute(prefix + " " + sql));
}

Database small_db() {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41), (4, 'yaw', 25)");
    db.execute("ANALYZE users");
    return db;
}

std::size_t count_of(const std::string& text, const std::string& what) {
    std::size_t n = 0, pos = 0;
    while ((pos = text.find(what, pos)) != std::string::npos) {
        ++n;
        pos += what.size();
    }
    return n;
}

}  // namespace

TEST_CASE("verbose: syntax, in either order") {
    REQUIRE(print(parse_statement("EXPLAIN VERBOSE SELECT * FROM t")) == "(explain-verbose (select (star) (from t)))");
    REQUIRE(print(parse_statement("EXPLAIN ANALYZE VERBOSE SELECT * FROM t")) ==
            "(explain-analyze-verbose (select (star) (from t)))");
    REQUIRE(print(parse_statement("explain verbose analyze select * from t;")) ==
            "(explain-analyze-verbose (select (star) (from t)))");
    REQUIRE(print(parse_statement("EXPLAIN SELECT * FROM t")) == "(explain (select (star) (from t)))");
    REQUIRE_THROWS_AS(parse_statement("EXPLAIN VERBOSE VERBOSE SELECT * FROM t"), ParseError);
    REQUIRE_THROWS_AS(parse_statement("EXPLAIN VERBOSE"), ParseError);
}

TEST_CASE("verbose: every step with its options, the winner starred, and the chosen plan") {
    Database db = small_db();
    REQUIRE(explain(db, "EXPLAIN VERBOSE", "SELECT name FROM users WHERE age > 25") ==
            "Original plan\n"
            "  Project[name]  est_rows=2\n"
            "    Filter[age > 25]  est_rows=2\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Rules fired\n"
            "  none\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=2\n"
            "    Filter[age > 25]  est_rows=2\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Ways to run it\n"
            "  (cost is the estimated work for the step and everything below it, in made-up units; lower is better; * marks the one chosen)\n"
            "  Project[name]  est_rows=2\n"
            "    * Project         cost 1.10\n"
            "    Filter[age > 25]  est_rows=2\n"
            "      * Filter          cost 1.08\n"
            "      Scan[users]  est_rows=4\n"
            "        * SeqScan[users]  cost 1.04\n"
            "  Total estimated cost: 1.10\n"
            "\n"
            "Chosen plan\n"
            "  Project[#1]\n"
            "    Filter[#2 > 25]\n"
            "      SeqScan[users]\n"
            "\n"
            "Statistics\n"
            "  users: analyzed when it had 4 rows");
}

TEST_CASE("verbose: a join lists the three ways to run it") {
    Database db;
    generate_users_orders(db, {.users = 1000});
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    std::string text = explain(db, "EXPLAIN VERBOSE", "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE(text.find("* HashJoin (build left)") != std::string::npos);  // the 1000 users are the smaller side
    REQUIRE(text.find("NestedLoopJoin") != std::string::npos);
    REQUIRE(text.find("HashJoin (build right)") != std::string::npos);
    REQUIRE(text.find("HashJoin[build left, ") != std::string::npos);

    // the nested loop costs many times the winner
    std::smatch nested, winner;
    REQUIRE(std::regex_search(text, nested, std::regex("NestedLoopJoin +cost ([0-9.]+)")));
    REQUIRE(std::regex_search(text, winner, std::regex("\\* HashJoin \\(build left\\) +cost ([0-9.]+)")));
    REQUIRE(std::stod(nested[1]) > 10 * std::stod(winner[1]));  // each cost includes the scans below
}

TEST_CASE("verbose: changing the table sizes changes the winner") {
    auto chosen_join = [](int users) {
        Database db;
        generate_users_orders(db, {.users = users});
        db.execute("ANALYZE users");
        db.execute("ANALYZE orders");
        std::string text = explain(db, "EXPLAIN VERBOSE", "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
        std::smatch m;
        REQUIRE(std::regex_search(text, m, std::regex("\\* (NestedLoopJoin|HashJoin \\(build (?:left|right)\\))")));
        return m[1].str();
    };
    REQUIRE(chosen_join(1) == "NestedLoopJoin");
    REQUIRE(chosen_join(1000) == "HashJoin (build left)");
}

TEST_CASE("verbose: exactly one winner for every step") {
    Database db;
    generate_users_orders(db, {.users = 300});
    std::string text = explain(db, "EXPLAIN VERBOSE",
                               "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 500 "
                               "ORDER BY u.name LIMIT 3");
    std::size_t ways = text.find("Ways to run it");
    std::size_t chosen = text.find("Chosen plan");
    std::string section = text.substr(ways, chosen - ways);
    section = section.substr(section.find('\n'));  // the first line explains the star
    std::size_t steps = count_of(section, "est_rows=");
    REQUIRE(steps >= 8);
    std::regex star_lines("\n *\\* ");
    REQUIRE(static_cast<std::size_t>(std::distance(std::sregex_iterator(section.begin(), section.end(), star_lines),
                                                   std::sregex_iterator())) == steps);
}

TEST_CASE("verbose: a nested-loop-only join says why there is no hash join in the trace") {
    Database db;
    generate_users_orders(db, {.users = 300});
    std::string text = explain(db, "EXPLAIN VERBOSE", "SELECT u.name FROM users u JOIN orders o ON u.id < o.user_id");
    REQUIRE(text.find("* NestedLoopJoin") != std::string::npos);
    REQUIRE(text.find("HashJoin (build") == std::string::npos);
}

TEST_CASE("verbose: the join method can be forced, and then no costs are compared") {
    Database db;
    generate_users_orders(db, {.users = 300});
    db.set_planner_options(PlannerOptions{JoinMethod::Hash, false});
    std::string text = explain(db, "EXPLAIN VERBOSE", "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE(text.find("The join method was forced (hash joins, building from the right input), so no costs were compared.") !=
            std::string::npos);
    REQUIRE(text.find("HashJoin[build right") != std::string::npos);
    REQUIRE(text.find("cost ") == std::string::npos);
}

TEST_CASE("verbose analyze: all six stages, in order") {
    Database db;
    generate_users_orders(db, {.users = 500});
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    std::string text = explain(db, "EXPLAIN ANALYZE VERBOSE",
                               "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id "
                               "WHERE u.country = 'Ghana' AND o.amount > 100");
    const char* headings[] = {"Original plan", "Rules fired", "Final plan", "Ways to run it",
                              "Chosen plan",   "Actual run",  "Execution",  "Statistics"};
    std::size_t last = 0;
    for (const char* heading : headings) {
        std::size_t at = text.find(std::string("\n") + heading);
        if (std::string(heading) == "Original plan") at = text.find(heading);
        INFO(heading);
        REQUIRE(at != std::string::npos);
        REQUIRE(at >= last);
        last = at;
    }
    // 1: the plan as written (before any rule)    2: the rewrites    3: the plan after them
    // 4: the other ways to run it, with costs      5: the one it picked
    // 6: for every step, the guess against the real count
    REQUIRE(text.find("filter-pushdown") != std::string::npos);
    // the real counts come last, under "Actual run", and not on the earlier plans
    REQUIRE(text.find("actual_rows=") > text.find("Actual run"));
    std::string actual_run = text.substr(text.find("Actual run"), text.find("Execution") - text.find("Actual run"));
    REQUIRE(count_of(actual_run, "actual_rows=") == count_of(actual_run, "est_rows="));
    REQUIRE(count_of(text, "actual_rows=") == count_of(actual_run, "actual_rows="));
}

TEST_CASE("verbose analyze: without VERBOSE the actual numbers sit on the final plan as before") {
    Database db = small_db();
    std::string text = explain(db, "EXPLAIN ANALYZE", "SELECT name FROM users WHERE age > 25");
    REQUIRE(text.find("Actual run") == std::string::npos);
    REQUIRE(text.find("Ways to run it") == std::string::npos);
    REQUIRE(text.find("Final plan\n  Project[name]  est_rows=2  actual_rows=2") != std::string::npos);
}

TEST_CASE("verbose: errors") {
    Database db = small_db();
    REQUIRE_THROWS_AS(db.execute("EXPLAIN VERBOSE SELECT * FROM nope"), DbError);
    REQUIRE_THROWS_AS(db.execute("EXPLAIN VERBOSE CREATE TABLE t (a INT)"), DbError);
}
