#include <catch2/catch_test_macros.hpp>

#include "common/error.h"
#include "engine/database.h"
#include "sql/ast_printer.h"
#include "sql/parser.h"
#include "stats/analyze.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

const ColumnStats& column(const TableStats& stats, const char* name) {
    for (const ColumnStats& c : stats.columns)
        if (c.name == name) return c;
    FAIL(std::string("no column ") + name);
    return stats.columns.front();
}

// Six rows, counted by hand:
//   id   1 2 3 4 5 6            no NULLs, 6 distinct, 1..6
//   a    1 1 2 NULL 3 3         1 NULL of 6, 3 distinct, 1..3
//   x    2.5 NULL NULL 0.5 2.5 9.0   2 NULLs of 6, 3 distinct, 0.5..9.0
//   name 'ama' 'kofi' 'ama' 'esi' 'kofi' 'ama'   no NULLs, 3 distinct, 'ama'..'kofi'
//   f    TRUE TRUE FALSE TRUE NULL TRUE   1 NULL of 6, 2 distinct, false..true
//   z    all NULL               6 NULLs, 0 distinct, no min or max
Database sample() {
    Database db;
    db.execute("CREATE TABLE t (id INT, a INT, x DOUBLE, name TEXT, f BOOL, z INT)");
    db.execute("INSERT INTO t VALUES (1, 1, 2.5, 'ama', TRUE, NULL), (2, 1, NULL, 'kofi', TRUE, NULL), "
               "(3, 2, NULL, 'ama', FALSE, NULL), (4, NULL, 0.5, 'esi', TRUE, NULL), "
               "(5, 3, 2.5, 'kofi', NULL, NULL), (6, 3, 9.0, 'ama', TRUE, NULL)");
    return db;
}

}  // namespace

TEST_CASE("analyze: row count, NULL share, distinct values, min and max") {
    Database db = sample();
    TableStats stats = analyze_table(*db.table("t"));
    REQUIRE(stats.row_count == 6);
    REQUIRE(stats.columns.size() == 6);

    const ColumnStats& id = column(stats, "id");
    REQUIRE(id.null_fraction == 0.0);
    REQUIRE(id.distinct == 6);
    REQUIRE(id.min == i(1));
    REQUIRE(id.max == i(6));

    const ColumnStats& a = column(stats, "a");
    REQUIRE(a.null_fraction == 1.0 / 6);
    REQUIRE(a.distinct == 3);
    REQUIRE(a.min == i(1));
    REQUIRE(a.max == i(3));

    const ColumnStats& x = column(stats, "x");
    REQUIRE(x.null_fraction == 2.0 / 6);
    REQUIRE(x.distinct == 3);
    REQUIRE(x.min == Value(0.5));
    REQUIRE(x.max == Value(9.0));

    const ColumnStats& name = column(stats, "name");
    REQUIRE(name.null_fraction == 0.0);
    REQUIRE(name.distinct == 3);
    REQUIRE(name.min == s("ama"));
    REQUIRE(name.max == s("kofi"));

    const ColumnStats& f = column(stats, "f");
    REQUIRE(f.null_fraction == 1.0 / 6);
    REQUIRE(f.distinct == 2);
    REQUIRE(f.min == Value(false));
    REQUIRE(f.max == Value(true));
}

TEST_CASE("analyze: a column of only NULLs has no min or max and no distinct values") {
    Database db = sample();
    TableStats stats = analyze_table(*db.table("t"));
    const ColumnStats& z = column(stats, "z");
    REQUIRE(z.null_fraction == 1.0);
    REQUIRE(z.distinct == 0);
    REQUIRE(z.min.is_null());
    REQUIRE(z.max.is_null());
}

TEST_CASE("analyze: an empty table") {
    Database db;
    db.execute("CREATE TABLE e (a INT, b TEXT)");
    TableStats stats = analyze_table(*db.table("e"));
    REQUIRE(stats.row_count == 0);
    REQUIRE(stats.columns.size() == 2);
    REQUIRE(stats.columns[0].null_fraction == 0.0);
    REQUIRE(stats.columns[0].distinct == 0);
    REQUIRE(stats.columns[0].min.is_null());
}

TEST_CASE("analyze: counts are exact on a large table") {
    Database db;
    db.execute("CREATE TABLE big (v INT)");
    std::vector<Row> rows;
    for (int n = 0; n < 5000; ++n) rows.push_back({n % 7 == 0 ? Value() : i(n % 100)});
    db.table("big")->insert_rows(std::move(rows));
    TableStats stats = analyze_table(*db.table("big"));
    const ColumnStats& v = column(stats, "v");
    // 5000 values; every 7th is NULL: 0, 7, ..., 4998 is 715 NULLs
    REQUIRE(v.null_fraction == 715.0 / 5000);
    REQUIRE(v.distinct == 100);
    REQUIRE(v.min == i(0));
    REQUIRE(v.max == i(99));
}

TEST_CASE("analyze: the ANALYZE statement stores the statistics on the table") {
    Database db = sample();
    REQUIRE_FALSE(db.table("t")->stats().has_value());
    QueryResult r = db.execute("ANALYZE t");
    REQUIRE(r.message == "ANALYZE t (6 rows)");
    REQUIRE(db.table("t")->stats().has_value());
    REQUIRE(db.table("t")->stats()->row_count == 6);
    REQUIRE(db.execute("analyze t;").message == "ANALYZE t (6 rows)");
}

TEST_CASE("analyze: errors") {
    Database db = sample();
    REQUIRE_THROWS_AS(db.execute("ANALYZE nope"), DbError);
    REQUIRE_THROWS_AS(db.execute("ANALYZE"), ParseError);
    REQUIRE_THROWS_AS(db.execute("ANALYZE t u"), ParseError);
}

TEST_CASE("analyze: statistics go stale when rows are added, and ANALYZE again fixes it") {
    Database db = sample();
    db.execute("ANALYZE t");
    const Table& t = *db.table("t");
    REQUIRE_FALSE(stats_are_stale(t, *t.stats()));

    db.execute("INSERT INTO t VALUES (7, 4, 1.0, 'yaw', FALSE, NULL)");
    REQUIRE(stats_are_stale(t, *t.stats()));
    REQUIRE(t.stats()->row_count == 6);       // still what it was
    REQUIRE(t.stats()->columns[1].max == i(3));

    db.execute("ANALYZE t");
    REQUIRE_FALSE(stats_are_stale(t, *t.stats()));
    REQUIRE(t.stats()->row_count == 7);
    REQUIRE(t.stats()->columns[1].max == i(4));
}

TEST_CASE("analyze: a failed insert does not make the statistics stale") {
    Database db = sample();
    db.execute("ANALYZE t");
    REQUIRE_THROWS_AS(db.execute("INSERT INTO t VALUES (7, 'oops', 1.0, 'yaw', FALSE, NULL)"), DbError);
    REQUIRE_FALSE(stats_are_stale(*db.table("t"), *db.table("t")->stats()));
}

TEST_CASE("analyze: parse") {
    REQUIRE(print(parse_statement("ANALYZE users")) == "(analyze users)");
    REQUIRE(print(parse_statement("analyze users;")) == "(analyze users)");
}

TEST_CASE("analyze: the .stats text") {
    Database db;
    db.execute("CREATE TABLE people (id INT, name TEXT, score DOUBLE)");
    db.execute("INSERT INTO people VALUES (1, 'ama', 9.5), (2, 'kofi', NULL), (3, 'ama', 3.0), (4, NULL, 3.0)");
    db.execute("ANALYZE people");
    const Table& t = *db.table("people");
    REQUIRE(format_stats(t, *t.stats()) ==
            "people: 4 rows when analyzed\n"
            "column | nulls | distinct | min | max\n"
            "-------+-------+----------+-----+-----\n"
            "id     | 0.0%  | 4        | 1   | 4\n"
            "name   | 25.0% | 2        | ama | kofi\n"
            "score  | 25.0% | 2        | 3.0 | 9.5\n"
            "id histogram (3 buckets, 4 values): 1 2 3 4\n"
            "name common values: ama x2 (50.0%)\n"
            "score common values: 3.0 x2 (50.0%)\n"
            "score histogram (1 bucket, 1 values): 9.5 9.5");

    db.execute("INSERT INTO people VALUES (5, 'esi', 1.0)");
    REQUIRE(format_stats(t, *t.stats()).find("warning: the table has 5 rows now; run ANALYZE people again") !=
            std::string::npos);
}
