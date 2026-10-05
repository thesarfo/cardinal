#include <catch2/catch_test_macros.hpp>

#include "binder/binder.h"
#include "engine/answer_checker.h"
#include "engine/database.h"
#include "expr/evaluator.h"
#include "exec/hash_join.h"
#include "exec/seq_scan.h"
#include "logical/planner.h"
#include "physical/join_keys.h"
#include "physical/physical_planner.h"
#include "physical/physical_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

BoundExprPtr column(std::uint32_t position) {
    return std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{ColumnId{position}}, std::nullopt});
}

// Two tables, l(k, tag) and r(k, tag), joined on k. The key of each side is column 0 of its own row.
struct Fixture {
    Catalog catalog;
    Table *l, *r;

    Fixture() {
        l = &catalog.create_table({"l", {{"k", Type::Int}, {"tag", Type::Text}}});
        r = &catalog.create_table({"r", {{"k", Type::Int}, {"tag", Type::Text}}});
    }

    std::vector<Row> run(bool build_left, BoundExprPtr residual = nullptr) {
        HashJoin join(std::make_unique<SeqScan>(*l), std::make_unique<SeqScan>(*r), {column(0)}, {column(0)}, residual, build_left);
        std::vector<Row> rows;
        while (auto row = join.next()) rows.push_back(*row);
        return rows;
    }
};

std::vector<Row> sorted(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (a[k].is_null() != b[k].is_null()) return a[k].is_null();
            if (a[k].is_null()) continue;
            int c = compare_values(a[k], b[k]);
            if (c != 0) return c < 0;
        }
        return false;
    });
    return rows;
}

}  // namespace

TEST_CASE("hash join: matching rows, left values then right values") {
    Fixture f;
    f.l->insert({i(1), s("a")});
    f.l->insert({i(2), s("b")});
    f.l->insert({i(3), s("c")});
    f.r->insert({i(2), s("x")});
    f.r->insert({i(3), s("y")});
    f.r->insert({i(4), s("z")});
    std::vector<Row> expected{{i(2), s("b"), i(2), s("x")}, {i(3), s("c"), i(3), s("y")}};
    REQUIRE(f.run(false) == expected);
    REQUIRE(f.run(true) == expected);  // building the other side changes the order of work, not the columns
}

TEST_CASE("hash join: duplicate keys on both sides pair up every combination") {
    Fixture f;
    f.l->insert({i(1), s("a1")});
    f.l->insert({i(1), s("a2")});
    f.l->insert({i(2), s("b")});
    f.r->insert({i(1), s("x1")});
    f.r->insert({i(1), s("x2")});
    f.r->insert({i(1), s("x3")});
    f.r->insert({i(5), s("z")});
    for (bool build_left : {false, true}) {
        std::vector<Row> rows = f.run(build_left);
        REQUIRE(rows.size() == 6);  // 2 x 3
        REQUIRE(sorted(rows).front() == Row{i(1), s("a1"), i(1), s("x1")});
        REQUIRE(sorted(rows).back() == Row{i(1), s("a2"), i(1), s("x3")});
    }
}

TEST_CASE("hash join: order is probe rows in order, and for each the build rows in arrival order") {
    Fixture f;
    f.l->insert({i(1), s("a1")});
    f.l->insert({i(2), s("b")});
    f.l->insert({i(1), s("a2")});
    f.r->insert({i(1), s("x1")});
    f.r->insert({i(1), s("x2")});
    // building the right side: probe is the left, so left rows come in order, and each pairs with x1 then x2
    std::vector<Row> built_right = f.run(false);
    REQUIRE(built_right.size() == 4);
    REQUIRE(built_right[0][1] == s("a1"));
    REQUIRE(built_right[0][3] == s("x1"));
    REQUIRE(built_right[1][3] == s("x2"));
    REQUIRE(built_right[2][1] == s("a2"));
}

TEST_CASE("hash join: NULL keys never match, on either side") {
    Fixture f;
    f.l->insert({Value(), s("null-left")});
    f.l->insert({i(1), s("a")});
    f.r->insert({Value(), s("null-right")});
    f.r->insert({i(1), s("x")});
    for (bool build_left : {false, true}) {
        std::vector<Row> rows = f.run(build_left);
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0] == Row{i(1), s("a"), i(1), s("x")});
    }
}

TEST_CASE("hash join: an empty input on either side gives nothing") {
    Fixture f;
    f.l->insert({i(1), s("a")});
    for (bool build_left : {false, true}) REQUIRE(f.run(build_left).empty());  // empty right
    Fixture g;
    g.r->insert({i(1), s("x")});
    for (bool build_left : {false, true}) REQUIRE(g.run(build_left).empty());  // empty left
    Fixture h;
    for (bool build_left : {false, true}) REQUIRE(h.run(build_left).empty());
}

TEST_CASE("hash join: the other parts of the condition are checked on the matched pairs") {
    Fixture f;
    f.l->insert({i(1), s("a")});
    f.l->insert({i(1), s("b")});
    f.r->insert({i(1), s("a")});
    f.r->insert({i(1), s("z")});
    // residual over the combined row (l.k, l.tag, r.k, r.tag): l.tag = r.tag, positions 1 and 3
    BoundExprPtr same_tag = std::make_shared<const BoundExpr>(
        BoundExpr{BoundBinary{BinaryOp::Eq, column(1), column(3)}, Type::Bool});
    for (bool build_left : {false, true}) {
        std::vector<Row> rows = f.run(build_left, same_tag);
        REQUIRE(rows == std::vector<Row>{{i(1), s("a"), i(1), s("a")}});
    }
}

TEST_CASE("hash join: 1 and 1.0 are the same key, and text keys work") {
    Catalog catalog;
    Table& a = catalog.create_table({"a", {{"x", Type::Int}}});
    Table& b = catalog.create_table({"b", {{"y", Type::Double}}});
    a.insert({i(1)});
    a.insert({i(2)});
    b.insert({Value(1.0)});
    b.insert({Value(2.5)});
    HashJoin join(std::make_unique<SeqScan>(a), std::make_unique<SeqScan>(b), {column(0)}, {column(0)}, nullptr, false);
    std::vector<Row> rows;
    while (auto row = join.next()) rows.push_back(*row);
    REQUIRE(rows == std::vector<Row>{{i(1), Value(1.0)}});

    Table& t = catalog.create_table({"t", {{"s", Type::Text}}});
    Table& u = catalog.create_table({"u", {{"s", Type::Text}}});
    t.insert({s("ama")});
    t.insert({s("")});
    u.insert({s("")});
    u.insert({s("kofi")});
    HashJoin text(std::make_unique<SeqScan>(t), std::make_unique<SeqScan>(u), {column(0)}, {column(0)}, nullptr, true);
    rows.clear();
    while (auto row = text.next()) rows.push_back(*row);
    REQUIRE(rows == std::vector<Row>{{s(""), s("")}});
}

TEST_CASE("hash join: several keys must all match") {
    Catalog catalog;
    Table& a = catalog.create_table({"a", {{"x", Type::Int}, {"y", Type::Int}}});
    Table& b = catalog.create_table({"b", {{"x", Type::Int}, {"y", Type::Int}}});
    a.insert({i(1), i(1)});
    a.insert({i(1), i(2)});
    b.insert({i(1), i(2)});
    b.insert({i(2), i(2)});
    HashJoin join(std::make_unique<SeqScan>(a), std::make_unique<SeqScan>(b), {column(0), column(1)}, {column(0), column(1)}, nullptr, false);
    std::vector<Row> rows;
    while (auto row = join.next()) rows.push_back(*row);
    REQUIRE(rows == std::vector<Row>{{i(1), i(2), i(1), i(2)}});
}

TEST_CASE("hash join: counts the pairs the keys brought together, not every pair") {
    Fixture f;
    for (int n = 0; n < 100; ++n) f.l->insert({i(n), s("l")});
    for (int n = 0; n < 100; ++n) f.r->insert({i(n % 10), s("r")});
    HashJoin join(std::make_unique<SeqScan>(*f.l), std::make_unique<SeqScan>(*f.r), {column(0)}, {column(0)}, nullptr, false);
    int out = 0;
    while (join.next()) ++out;
    REQUIRE(out == 100);  // each right key 0..9 matches one left row
    REQUIRE(join.stats().rows_out == 100);
    REQUIRE(join.stats().rows_scanned == 100);  // against 10,000 for a nested loop
}

TEST_CASE("join keys: equalities between the two sides, either way round, and the rest") {
    Database db;
    db.execute("CREATE TABLE a (x INT, y INT)");
    db.execute("CREATE TABLE b (x INT, y INT)");
    Scope scope;
    scope.add("a", db.table("a")->info());
    scope.add("b", db.table("b")->info());
    std::vector<ColumnId> left = scope.tables()[0].columns, right = scope.tables()[1].columns;
    auto split = [&](const char* condition) {
        return split_join_condition(bind_expression(*parse_expression(condition), scope), left, right);
    };

    JoinKeys one = split("a.x = b.x");
    REQUIRE(one.keys.size() == 1);
    REQUIRE(one.residual.empty());

    JoinKeys flipped = split("b.x = a.x");
    REQUIRE(flipped.keys.size() == 1);
    REQUIRE(print(*flipped.keys[0].first) == "#0");   // the left expression is always the left input's
    REQUIRE(print(*flipped.keys[0].second) == "#2");

    JoinKeys mixed = split("a.x = b.x AND a.y = b.y AND a.y > 3 AND b.x + 1 < a.x");
    REQUIRE(mixed.keys.size() == 2);
    REQUIRE(mixed.residual.size() == 2);

    JoinKeys expressions = split("a.x + 1 = b.y * 2");
    REQUIRE(expressions.keys.size() == 1);

    REQUIRE(split("a.x < b.x").keys.empty());
    REQUIRE(split("a.x = a.y").keys.empty());        // both sides from one input
    REQUIRE(split("a.x = 5").keys.empty());
    REQUIRE(split("a.x = b.x OR a.y = b.y").keys.empty());  // an OR is not a key
    REQUIRE(split_join_condition(nullptr, left, right).keys.empty());
}

TEST_CASE("planner: a hash join where there is an equality, a nested loop otherwise") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    auto plan = [&](const char* sql, PlannerOptions options) {
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        return print(*plan_physical(*plan_select(bound), options));
    };
    PlannerOptions hash_right{JoinMethod::Hash, false};
    PlannerOptions hash_left{JoinMethod::Hash, true};

    REQUIRE(plan("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id", hash_right) ==
            "Project[#1]\n"
            "  HashJoin[build right, l#0 = r#1]\n"
            "    SeqScan[users]\n"
            "    SeqScan[orders]");
    REQUIRE(plan("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id", hash_left).find("build left") !=
            std::string::npos);
    REQUIRE(plan("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id AND o.amount > 5", hash_right).find(", then #4 > 5") !=
            std::string::npos);
    // no equality: stays a nested loop
    REQUIRE(plan("SELECT u.name FROM users u JOIN orders o ON u.id < o.user_id", hash_right).find("NestedLoopJoin") !=
            std::string::npos);
    REQUIRE(plan("SELECT u.name FROM users u, orders o", hash_right).find("NestedLoopJoin[CROSS]") != std::string::npos);
    // not asked for: nested loop
    REQUIRE(plan("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id", {}).find("NestedLoopJoin") != std::string::npos);
}

TEST_CASE("hash joins give the same rows as nested loop joins through the engine") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41), (NULL, 'ghost', 5), (3, 'esi2', 22)");
    db.execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 70.5), (12, 3, 2.0), (13, NULL, 1.0), (14, 3, 9.5)");
    const char* queries[] = {
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id AND o.amount > 3",
        "SELECT u.name, o.id FROM users u JOIN orders o ON o.user_id = u.id AND u.age < o.amount",
        "SELECT u.name, o.id FROM users u, orders o WHERE u.id = o.user_id",
        "SELECT a.name, b.name FROM users a JOIN users b ON a.id = b.id",
        "SELECT a.name FROM users a JOIN users b ON a.age = b.age + 8",
    };
    for (const char* sql : queries) {
        INFO(sql);
        CheckOutcome outcome = check_query(db, sql);
        INFO(outcome.detail);
        REQUIRE(outcome.ok);
    }
}
