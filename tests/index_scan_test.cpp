#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "common/error.h"
#include "engine/answer_checker.h"
#include "engine/database.h"
#include "engine/result_format.h"
#include "sql/ast_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Database numbers_db() {
    Database db;
    db.execute("CREATE TABLE t (id INT, v INT, w DOUBLE, s TEXT)");
    db.execute("INSERT INTO t VALUES (1, 10, 0.5, 'a'), (2, 20, 1.5, 'b'), (3, 20, NULL, 'c'), "
               "(4, NULL, 2.5, 'd'), (5, 30, 3.5, NULL)");
    db.execute("CREATE INDEX t_v ON t (v)");
    return db;
}

std::string plan(Database& db, const std::string& sql) { return format_result(db.execute("EXPLAIN VERBOSE " + sql)); }

std::vector<std::string> ids(Database& db, const std::string& sql) {
    std::vector<std::string> out;
    for (const Row& r : db.execute(sql).rows) out.push_back(r[0].to_string());
    return out;
}

}  // namespace

TEST_CASE("create index: the parser reads it") {
    REQUIRE(print(parse_statement("CREATE INDEX t_v ON t (v)")) == "(create-index t_v t v)");
}

TEST_CASE("create index: unknown table, unknown column and a repeated name are errors") {
    Database db = numbers_db();
    REQUIRE_THROWS_AS(db.execute("CREATE INDEX x ON nope (v)"), DbError);
    REQUIRE_THROWS_AS(db.execute("CREATE INDEX x ON t (nope)"), DbError);
    REQUIRE_THROWS_AS(db.execute("CREATE INDEX t_v ON t (w)"), DbError);
    REQUIRE_NOTHROW(db.execute("CREATE INDEX t_w ON t (w)"));
}

TEST_CASE("index scan: an equality on an indexed column reads through the index") {
    Database db = numbers_db();
    REQUIRE(plan(db, "SELECT id FROM t WHERE v = 20").find("IndexScan[t using t_v, v = 20]") != std::string::npos);
    REQUIRE(ids(db, "SELECT id FROM t WHERE v = 20") == std::vector<std::string>{"2", "3"});
}

TEST_CASE("index scan: ranges, both open and closed, and a constant on the left") {
    Database db = numbers_db();
    REQUIRE(plan(db, "SELECT id FROM t WHERE v > 10 AND v <= 20").find("IndexScan[t using t_v, v > 10 AND v <= 20]") != std::string::npos);
    REQUIRE(ids(db, "SELECT id FROM t WHERE v > 10 AND v <= 20") == std::vector<std::string>{"2", "3"});
    REQUIRE(ids(db, "SELECT id FROM t WHERE 20 < v") == std::vector<std::string>{"5"});
    REQUIRE(ids(db, "SELECT id FROM t WHERE v >= 20") == std::vector<std::string>{"2", "3", "5"});
}

TEST_CASE("index scan: other conditions stay in a filter above it") {
    Database db = numbers_db();
    std::string text = plan(db, "SELECT id FROM t WHERE v = 20 AND s = 'c'");
    REQUIRE(text.find("IndexScan[t using t_v, v = 20]") != std::string::npos);
    REQUIRE(text.find("Filter") != std::string::npos);
    REQUIRE(ids(db, "SELECT id FROM t WHERE v = 20 AND s = 'c'") == std::vector<std::string>{"3"});
}

TEST_CASE("index scan: a NULL is never found, and a comparison with NULL finds nothing") {
    Database db = numbers_db();
    REQUIRE(ids(db, "SELECT id FROM t WHERE v < 100") == std::vector<std::string>{"1", "2", "3", "5"});
    REQUIRE(ids(db, "SELECT id FROM t WHERE v = NULL").empty());
    REQUIRE(ids(db, "SELECT id FROM t WHERE v IS NULL") == std::vector<std::string>{"4"});
}

TEST_CASE("index scan: an INT constant against a DOUBLE column") {
    Database db = numbers_db();
    db.execute("CREATE INDEX t_w ON t (w)");
    REQUIRE(ids(db, "SELECT id FROM t WHERE w >= 2") == std::vector<std::string>{"4", "5"});
    REQUIRE(ids(db, "SELECT id FROM t WHERE w = 1.5") == std::vector<std::string>{"2"});
}

TEST_CASE("index scan: rows added after the index was made are found") {
    Database db = numbers_db();
    db.execute("INSERT INTO t VALUES (6, 20, 9.5, 'f')");
    REQUIRE(ids(db, "SELECT id FROM t WHERE v = 20") == std::vector<std::string>{"2", "3", "6"});
}

TEST_CASE("index scan: it can be switched off, and then the plan is a scan") {
    Database db = numbers_db();
    db.set_use_indexes(false);
    REQUIRE(plan(db, "SELECT id FROM t WHERE v = 20").find("IndexScan") == std::string::npos);
}

TEST_CASE("index scan: a join over an indexed filter gives the same answer as without indexes") {
    Database db = numbers_db();
    db.execute("CREATE TABLE u (id INT, t_id INT)");
    db.execute("INSERT INTO u VALUES (1, 2), (2, 3), (3, 3), (4, 9)");
    for (const char* sql : {"SELECT t.id, u.id FROM t JOIN u ON u.t_id = t.id WHERE t.v = 20",
                            "SELECT t.id FROM t, u WHERE t.v >= 20 AND u.t_id = t.id ORDER BY t.id DESC",
                            "SELECT v, COUNT(*) FROM t WHERE v > 10 GROUP BY v"}) {
        INFO(sql);
        CheckOutcome outcome = check_query(db, sql);
        REQUIRE(outcome.ok);
    }
}
