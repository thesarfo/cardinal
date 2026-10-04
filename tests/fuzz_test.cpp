#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

#include "broken_rules.h"
#include "engine/fuzz.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

int line_count(const QuerySpec& spec) {
    std::string lines = to_lines(spec);
    return 1 + static_cast<int>(std::count(lines.begin(), lines.end(), '\n'));
}

std::optional<FuzzFailure> find_bug(std::function<std::unique_ptr<Rule>()> broken, uint64_t seed, int count) {
    Database db;
    create_fuzz_tables(db, seed);
    with_extra_rule(db, std::move(broken));
    return fuzz(db, seed, count).failure;
}

}  // namespace

TEST_CASE("generator: the same seed and index give the same query") {
    for (int i = 0; i < 50; ++i) REQUIRE(to_sql(generate_query(7, i)) == to_sql(generate_query(7, i)));
    REQUIRE(to_sql(generate_query(7, 0)) != to_sql(generate_query(8, 0)));
    REQUIRE(to_sql(generate_query(7, 0)) != to_sql(generate_query(7, 1)));
}

TEST_CASE("generator: every query is valid and runs") {
    Database db;
    create_fuzz_tables(db, 3);
    for (int i = 0; i < 1000; ++i) {
        std::string sql = to_sql(generate_query(3, i));
        INFO(sql);
        REQUIRE_NOTHROW(db.execute(sql));
    }
}

TEST_CASE("generator: a spread of queries, not one shape") {
    std::set<std::string> seen;
    std::size_t max_tables = 0;
    for (int i = 0; i < 2000; ++i) {
        QuerySpec q = generate_query(11, i);
        max_tables = std::max(max_tables, 1 + q.joins.size());
        if (!q.joins.empty() && q.joins[0].on) seen.insert("join-on");
        if (!q.joins.empty() && !q.joins[0].on) seen.insert("comma-join");
        if (q.where) seen.insert("where");
        if (!q.order_by.empty()) seen.insert("order-by");
        if (q.limit) seen.insert("limit");
        if (q.select == std::vector<std::string>{"*"}) seen.insert("star");
        if (q.where && q.where->kind == BoolExpr::Kind::Or) seen.insert("or");
        if (q.where && q.where->kind == BoolExpr::Kind::And) seen.insert("and");
        if (q.where && q.where->kind == BoolExpr::Kind::Not) seen.insert("not");
    }
    REQUIRE(max_tables == 3);
    REQUIRE(seen == std::set<std::string>{"join-on", "comma-join", "where", "order-by", "limit", "star", "or", "and", "not"});
}

TEST_CASE("generator: tables get random rows, with NULLs, from the seed") {
    Database a, b, c;
    create_fuzz_tables(a, 5);
    create_fuzz_tables(b, 5);
    create_fuzz_tables(c, 6);
    auto dump = [](Database& db) {
        std::string out;
        for (const char* t : {"t1", "t2", "t3"})
            for (const Row& r : db.execute(std::string("SELECT * FROM ") + t).rows)
                for (const Value& v : r) out += v.to_string() + ",";
        return out;
    };
    REQUIRE(dump(a) == dump(b));
    REQUIRE(dump(a) != dump(c));
    REQUIRE(dump(a).find("NULL") != std::string::npos);
}

TEST_CASE("printing: one clause per line, or all on one line") {
    QuerySpec q;
    q.from_table = "t1";
    q.from_alias = "q1";
    q.select = {"q1.a", "q1.b"};
    q.joins.push_back({"t2", "q2", BoolExpr{BoolExpr::Kind::Leaf, "q2.a = q1.a", {}}});
    q.joins.push_back({"t3", "q3", std::nullopt});
    q.where = BoolExpr{BoolExpr::Kind::Or,
                       "",
                       {BoolExpr{BoolExpr::Kind::Leaf, "q1.a > 1", {}},
                        BoolExpr{BoolExpr::Kind::Not, "", {BoolExpr{BoolExpr::Kind::Leaf, "q1.f", {}}}}}};
    q.order_by = {{"q1.a", true}, {"q1.b", false}};
    q.limit = 3;
    REQUIRE(to_lines(q) ==
            "SELECT q1.a, q1.b\n"
            "FROM t1 AS q1\n"
            "JOIN t2 AS q2 ON q2.a = q1.a\n"
            ", t3 AS q3\n"
            "WHERE q1.a > 1 OR (NOT (q1.f))\n"
            "ORDER BY q1.a DESC, q1.b\n"
            "LIMIT 3");
    REQUIRE(to_sql(q) ==
            "SELECT q1.a, q1.b FROM t1 AS q1 JOIN t2 AS q2 ON q2.a = q1.a, t3 AS q3 "
            "WHERE q1.a > 1 OR (NOT (q1.f)) ORDER BY q1.a DESC, q1.b LIMIT 3");
    Database db;
    create_fuzz_tables(db, 1);
    REQUIRE_NOTHROW(db.execute(to_sql(q)));
}

TEST_CASE("shrinker: cuts everything the failure doesn't need") {
    QuerySpec q;
    q.from_table = "t1";
    q.from_alias = "q1";
    q.select = {"q1.a", "q1.b", "q1.c"};
    q.joins.push_back({"t2", "q2", BoolExpr{BoolExpr::Kind::Leaf, "q2.a = q1.a", {}}});
    auto leaf = [](const char* text) { return BoolExpr{BoolExpr::Kind::Leaf, text, {}}; };
    q.where = BoolExpr{BoolExpr::Kind::And, "",
                       {leaf("q1.a = 1"),
                        BoolExpr{BoolExpr::Kind::Or, "", {leaf("q1.b = 2"), leaf("q1.c > 0.5")}},
                        BoolExpr{BoolExpr::Kind::Not, "", {leaf("q1.f")}}}};
    q.order_by = {{"q1.a", false}, {"q1.b", true}};
    q.limit = 4;

    // "Fails" as long as the text still mentions the cause.
    auto fails = [](const QuerySpec& s) { return to_sql(s).find("q1.b = 2") != std::string::npos; };
    QuerySpec small = shrink(q, fails);
    REQUIRE(to_lines(small) ==
            "SELECT q1.c\n"
            "FROM t1 AS q1\n"
            "WHERE q1.b = 2");
}

TEST_CASE("shrinker: keeps what the failure needs") {
    QuerySpec q;
    q.from_table = "t1";
    q.from_alias = "q1";
    q.select = {"q1.a", "q1.b"};
    q.joins.push_back({"t2", "q2", BoolExpr{BoolExpr::Kind::Leaf, "q2.a = q1.a", {}}});
    q.order_by = {{"q1.a", true}};
    q.limit = 2;
    auto fails = [](const QuerySpec& s) { return !s.order_by.empty() && s.limit && !s.joins.empty(); };
    QuerySpec small = shrink(q, fails);
    REQUIRE(small.order_by.size() == 1);
    REQUIRE(small.limit == 2);
    REQUIRE(small.joins.size() == 1);
    REQUIRE_FALSE(small.joins[0].on.has_value());  // the condition wasn't needed
    REQUIRE(small.select.size() == 1);
}

TEST_CASE("shrinker: nothing to cut leaves the query alone") {
    QuerySpec q = generate_query(1, 0);
    QuerySpec same = shrink(q, [](const QuerySpec&) { return false; });
    REQUIRE(to_sql(same) == to_sql(q));
}

TEST_CASE("fuzz: the real rules agree with the unoptimized plan on random queries") {
    Database db;
    create_fuzz_tables(db, 42);
    FuzzReport report = fuzz(db, 42, 500);
    if (report.failure) FAIL(describe(*report.failure));
    REQUIRE(report.queries == 500);
    REQUIRE(report.compared == 500);
    REQUIRE(report.skipped == 0);
}

TEST_CASE("fuzz: a rule that weakens filters is found and shrunk to a few lines") {
    auto failure = find_bug([] { return std::make_unique<DropsConjunct>(); }, 1, 3000);
    REQUIRE(failure.has_value());
    INFO(describe(*failure));
    REQUIRE(line_count(failure->shrunk) < 5);
    REQUIRE(to_sql(failure->shrunk).find(" AND ") != std::string::npos);
    REQUIRE(failure->detail.find("drops-conjunct") != std::string::npos);
}

TEST_CASE("fuzz: a rule that drops a join condition is found and shrunk") {
    auto failure = find_bug([] { return std::make_unique<DropsJoinCondition>(); }, 1, 3000);
    REQUIRE(failure.has_value());
    INFO(describe(*failure));
    REQUIRE(line_count(failure->shrunk) < 5);
    REQUIRE(failure->shrunk.joins.size() == 1);
    REQUIRE(failure->detail.find("drops-join-condition") != std::string::npos);
}

TEST_CASE("fuzz: a rule that flips a sort is found and shrunk") {
    auto failure = find_bug([] { return std::make_unique<FlipsSort>(); }, 1, 3000);
    REQUIRE(failure.has_value());
    INFO(describe(*failure));
    REQUIRE(line_count(failure->shrunk) < 5);
    REQUIRE(failure->shrunk.order_by.size() == 1);
    REQUIRE(failure->detail.find("flips-sort") != std::string::npos);
}

TEST_CASE("fuzz: the NULL mistake x = x is found too") {
    auto failure = find_bug([] { return std::make_unique<EqualsSelfIsTrue>(); }, 2, 8000);
    REQUIRE(failure.has_value());
    INFO(describe(*failure));
    REQUIRE(line_count(failure->shrunk) < 5);
    REQUIRE(failure->detail.find("x-equals-x") != std::string::npos);
}

TEST_CASE("fuzz: a rule that only changes how ties are broken is not a bug") {
    Database db;
    create_fuzz_tables(db, 9);
    with_extra_rule(db, [] { return std::make_unique<BreaksTiesBackwards>(); });
    FuzzReport report = fuzz(db, 9, 600);
    if (report.failure) FAIL(describe(*report.failure));
}

TEST_CASE("fuzz: a failure can be reproduced from its seed and index alone") {
    auto failure = find_bug([] { return std::make_unique<DropsConjunct>(); }, 1, 3000);
    REQUIRE(failure.has_value());
    REQUIRE(to_sql(generate_query(failure->seed, failure->index)) == to_sql(failure->original));
}
