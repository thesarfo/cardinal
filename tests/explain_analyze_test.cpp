#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <regex>

#include "common/error.h"
#include "engine/database.h"
#include "engine/datagen.h"
#include "engine/result_format.h"
#include "sql/ast_printer.h"
#include "sql/parser.h"
#include "stats/q_error.h"

using namespace cardinal;
using Catch::Approx;

namespace {

Database make_db() {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41), (4, 'yaw', 25)");
    db.execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 70.5), (12, 3, 2.0), (13, 4, 9.0), (14, 4, 100.0)");
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    return db;
}

// Timings change from run to run, so they are blanked before comparing.
std::string without_times(std::string text) {
    text = std::regex_replace(text, std::regex("time=[0-9.]+ms"), "time=T");
    text = std::regex_replace(text, std::regex("planning [0-9.]+ ms, execution [0-9.]+ ms"), "planning P ms, execution E ms");
    return text;
}

std::string analyze(Database& db, const std::string& sql) { return format_result(db.execute("EXPLAIN ANALYZE " + sql)); }

}  // namespace

TEST_CASE("q-error: how many times off, never below 1") {
    REQUIRE(q_error(100, 100) == 1);
    REQUIRE(q_error(10, 100) == 10);
    REQUIRE(q_error(100, 10) == 10);
    REQUIRE(q_error(1, 1) == 1);
}

TEST_CASE("q-error: both counts count as at least 1") {
    REQUIRE(q_error(0, 0) == 1);
    REQUIRE(q_error(0.3, 0) == 1);
    REQUIRE(q_error(0, 200) == 200);
    REQUIRE(q_error(0.3, 200) == 200);
    REQUIRE(q_error(200, 0) == 200);
    REQUIRE(q_error(50, 0.5) == 50);
}

TEST_CASE("explain analyze: syntax") {
    REQUIRE(print(parse_statement("EXPLAIN ANALYZE SELECT * FROM t")) == "(explain-analyze (select (star) (from t)))");
    REQUIRE(print(parse_statement("explain analyze select a from t;")) == "(explain-analyze (select (col a) (from t)))");
    REQUIRE(print(parse_statement("EXPLAIN SELECT * FROM t")) == "(explain (select (star) (from t)))");
    REQUIRE_THROWS_AS(parse_statement("EXPLAIN ANALYZE"), ParseError);
    REQUIRE_THROWS_AS(parse_statement("EXPLAIN ANALYZE EXPLAIN SELECT * FROM t"), ParseError);
}

TEST_CASE("explain analyze: each line of the final plan has the guess, the real count, the q-error and the time") {
    Database db = make_db();
    // The filter's real estimate is a little over 2 rows (shown rounded), so its q-error against 2 is 1.20.
    REQUIRE(without_times(analyze(db, "SELECT name FROM users WHERE age > 25")) ==
            "Original plan\n"
            "  Project[name]  est_rows=2\n"
            "    Filter[age > 25]  est_rows=2\n"
            "      Scan[users]  est_rows=4\n"
            "\n"
            "Rules fired\n"
            "  none\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=2  actual_rows=2  q_error=1.20  time=T\n"
            "    Filter[age > 25]  est_rows=2  actual_rows=2  q_error=1.20  time=T\n"
            "      Scan[users]  est_rows=4  actual_rows=4  q_error=1.00  time=T\n"
            "\n"
            "Execution\n"
            "  planning P ms, execution E ms, 2 rows returned\n"
            "  worst guess: Filter[age > 25] (est 2, actual 2, q-error 1.20)\n"
            "\n"
            "Statistics\n"
            "  users: analyzed when it had 4 rows");
}

TEST_CASE("explain analyze: the real counts are what the query really produced") {
    Database db = make_db();
    struct Case {
        const char* sql;
        const char* join_line;  // the text of the line to check
        int actual;
    };
    // users joined to orders: user 1 has 2 orders, user 3 has 1, user 4 has 2: five rows
    std::string text = analyze(db, "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE(text.find("Join[INNER ON u.id = user_id]  est_rows=5  actual_rows=5") != std::string::npos);
    REQUIRE(text.find("Scan[users AS u]  est_rows=4  actual_rows=4") != std::string::npos);
    REQUIRE(text.find("Scan[orders AS o]  est_rows=5  actual_rows=5") != std::string::npos);
    REQUIRE(text.find("5 rows returned") != std::string::npos);
}

TEST_CASE("explain analyze: a wrong guess shows up as a q-error above 1, on the step where it went wrong") {
    Database db = make_db();
    // Statistics say user_id has 3 distinct values; the filter on amount keeps 4 of 5, guessed as 3.
    std::string text = analyze(db, "SELECT o.id FROM orders o WHERE o.amount > 4");
    std::smatch m;
    REQUIRE(std::regex_search(text, m, std::regex("Filter\\[amount > 4\\]  est_rows=(\\d+)  actual_rows=(\\d+)  q_error=([0-9.]+)")));
    double estimate = std::stod(m[1]);
    double actual = std::stod(m[2]);
    REQUIRE(actual == 4);
    REQUIRE(std::stod(m[3]) == Approx(q_error(estimate, actual)).margin(0.5));
    REQUIRE(text.find("worst guess:") != std::string::npos);
}

TEST_CASE("explain analyze: the q-error on every line agrees with its two counts") {
    Database db = make_db();
    std::string text = analyze(db,
                               "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id "
                               "WHERE o.amount > 4 AND u.age > 20 ORDER BY u.name LIMIT 5");
    std::regex line("est_rows=(\\d+)  actual_rows=(\\d+)  q_error=([0-9.]+)  time=");
    int checked = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), line), end; it != end; ++it) {
        double est = std::stod((*it)[1]);
        double actual = std::stod((*it)[2]);
        double shown = std::stod((*it)[3]);
        // est_rows is rounded for display, so the q-error was worked out from a number close to it
        REQUIRE(shown >= 1.0);
        REQUIRE(shown <= q_error(est, actual) * 1.5 + 0.5);
        REQUIRE(shown >= q_error(est, actual) / 1.5 - 0.5);
        ++checked;
    }
    REQUIRE(checked == 11);  // every step of the final plan
}

TEST_CASE("explain analyze: a LIMIT stops the steps below it early") {
    Database db = make_db();
    std::string text = analyze(db, "SELECT id FROM users LIMIT 2");
    REQUIRE(text.find("Limit[2]  est_rows=2  actual_rows=2") != std::string::npos);
    REQUIRE(text.find("Scan[users]  est_rows=4  actual_rows=2") != std::string::npos);
    REQUIRE(text.find("2 rows returned") != std::string::npos);
}

TEST_CASE("explain analyze: a step the optimizer emptied guessed 0 and got 0, a perfect 1") {
    Database db = make_db();
    std::string text = analyze(db, "SELECT name FROM users WHERE 1 > 2");
    REQUIRE(text.find("Empty  est_rows=0  actual_rows=0  q_error=1.00") != std::string::npos);
    REQUIRE(text.find("0 rows returned") != std::string::npos);
}

TEST_CASE("explain analyze: the plain statistics warning still appears") {
    Database db = make_db();
    db.execute("INSERT INTO users VALUES (5, 'yaa', 33)");
    std::string text = analyze(db, "SELECT name FROM users");
    REQUIRE(text.find("warning: users has 5 rows now; run ANALYZE users") != std::string::npos);
    // the scan is guessed from the real size now, and is right
    REQUIRE(text.find("Scan[users]  est_rows=5  actual_rows=5  q_error=1.00") != std::string::npos);
}

TEST_CASE("explain analyze: works without ANALYZE, with the default guesses") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    db.execute("INSERT INTO t VALUES (1), (2), (3), (4), (5), (6)");
    std::string text = analyze(db, "SELECT a FROM t WHERE a > 3");
    REQUIRE(text.find("Filter[a > 3]  est_rows=2  actual_rows=3") != std::string::npos);  // 6 x 1/3, against 3
    REQUIRE(text.find("t: not analyzed") != std::string::npos);
}

TEST_CASE("explain analyze: runs the query but changes no data") {
    Database db = make_db();
    analyze(db, "SELECT name FROM users WHERE age > 25");
    REQUIRE(db.execute("SELECT id FROM users").rows.size() == 4);
    REQUIRE(db.table("users")->stats()->row_count == 4);
}

TEST_CASE("explain analyze: respects the optimizer switch") {
    Database db = make_db();
    db.set_optimizer_enabled(false);
    std::string text = analyze(db, "SELECT name FROM users WHERE age > 20 + 5");
    REQUIRE(text.find("Rules fired\n  none") != std::string::npos);
    db.set_optimizer_enabled(true);
    REQUIRE(analyze(db, "SELECT name FROM users WHERE age > 20 + 5").find("constant-folding") != std::string::npos);
}

TEST_CASE("explain analyze: errors") {
    Database db = make_db();
    REQUIRE_THROWS_AS(db.execute("EXPLAIN ANALYZE SELECT * FROM nope"), DbError);
    REQUIRE_THROWS_AS(db.execute("EXPLAIN ANALYZE CREATE TABLE x (a INT)"), DbError);
    REQUIRE_THROWS_AS(db.execute("EXPLAIN ANALYZE SELECT 1 / 0 FROM users"), DbError);
}

TEST_CASE("explain analyze: the numbers behind the text") {
    Database db = make_db();
    ExplainAnalyzeOutput out = db.explain_analyze("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE(out.root_actual == 5);
    REQUIRE(out.root_estimate == Approx(5));
    REQUIRE(out.max_q_error >= 1.0);
    REQUIRE(out.text.find("Final plan") != std::string::npos);
    REQUIRE_THROWS_AS(db.explain_analyze("CREATE TABLE y (a INT)"), DbError);
}

TEST_CASE("explain analyze: on the demo query, the guesses are close") {
    Database db;
    generate_users_orders(db, {.users = 1000});
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    ExplainAnalyzeOutput out = db.explain_analyze(
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.country = 'Ghana' AND o.amount > 100");
    INFO(out.text);
    REQUIRE(out.max_q_error < 1.5);
    REQUIRE(q_error(out.root_estimate, static_cast<double>(out.root_actual)) < 1.5);
}
