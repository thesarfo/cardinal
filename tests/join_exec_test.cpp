#include <catch2/catch_test_macros.hpp>

#include "binder/binder.h"
#include "common/error.h"
#include "engine/database.h"
#include "exec/nested_loop_join.h"
#include "exec/seq_scan.h"
#include "logical/planner.h"
#include "physical/physical_planner.h"
#include "physical/physical_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

Database make_db() {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("CREATE TABLE items (id INT, order_id INT, sku TEXT)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41)");
    db.execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 7.5), (12, 3, 2.0), (13, NULL, 1.0)");
    db.execute("INSERT INTO items VALUES (100, 10, 'x'), (101, 10, 'y'), (102, 12, 'z')");
    return db;
}

std::vector<Row> rows(Database& db, const char* sql) { return db.execute(sql).rows; }

}  // namespace

TEST_CASE("join: matching rows, left rows in order, right rows in order") {
    Database db = make_db();
    REQUIRE(rows(db, "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id") ==
            std::vector<Row>{{s("ama"), i(10)}, {s("ama"), i(11)}, {s("esi"), i(12)}});
}

TEST_CASE("join: a left row with no match is dropped, and NULL never matches") {
    Database db = make_db();
    // kofi has no orders; order 13 has a NULL user_id
    REQUIRE(rows(db, "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.name = 'kofi'").empty());
    REQUIRE(rows(db, "SELECT o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE o.id = 13").empty());
}

TEST_CASE("join: a comma join pairs every row with every row") {
    Database db = make_db();
    auto all = rows(db, "SELECT u.id, o.id FROM users u, orders o");
    REQUIRE(all.size() == 12);
    REQUIRE(all[0] == Row{i(1), i(10)});
    REQUIRE(all[3] == Row{i(1), i(13)});
    REQUIRE(all[4] == Row{i(2), i(10)});
    REQUIRE(all[11] == Row{i(3), i(13)});
}

TEST_CASE("join: an equality in WHERE over a comma join gives the same rows as JOIN ... ON") {
    Database db = make_db();
    REQUIRE(rows(db, "SELECT u.name, o.id FROM users u, orders o WHERE u.id = o.user_id") ==
            rows(db, "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id"));
}

TEST_CASE("join: conditions on one side, on both, and always true") {
    Database db = make_db();
    REQUIRE(rows(db, "SELECT u.id, o.id FROM users u JOIN orders o ON o.amount > 5") ==
            std::vector<Row>{{i(1), i(11)}, {i(2), i(11)}, {i(3), i(11)}});
    REQUIRE(rows(db, "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id AND o.amount > 3") ==
            std::vector<Row>{{s("ama"), i(10)}, {s("ama"), i(11)}});
    REQUIRE(rows(db, "SELECT u.id, o.id FROM users u JOIN orders o ON TRUE").size() == 12);
    REQUIRE(rows(db, "SELECT u.id FROM users u JOIN orders o ON FALSE").empty());
    REQUIRE(rows(db, "SELECT u.id FROM users u JOIN orders o ON NULL").empty());
}

TEST_CASE("join: WHERE, ORDER BY and LIMIT apply to the joined rows") {
    Database db = make_db();
    REQUIRE(rows(db, "SELECT u.name, o.amount FROM users u JOIN orders o ON u.id = o.user_id "
                     "WHERE o.amount < 7 ORDER BY o.amount DESC, u.name LIMIT 2") ==
            std::vector<Row>{{s("ama"), Value(5.0)}, {s("esi"), Value(2.0)}});
}

TEST_CASE("join: a table joined to itself") {
    Database db = make_db();
    // pairs where the left person is younger than the right one
    REQUIRE(rows(db, "SELECT a.name, b.name FROM users a JOIN users b ON a.age < b.age ORDER BY a.name, b.name") ==
            std::vector<Row>{{s("ama"), s("esi")}, {s("kofi"), s("ama")}, {s("kofi"), s("esi")}});
    REQUIRE(rows(db, "SELECT a.id FROM users a JOIN users b ON a.id = b.id ORDER BY a.id") ==
            std::vector<Row>{{i(1)}, {i(2)}, {i(3)}});
}

TEST_CASE("join: three tables") {
    Database db = make_db();
    REQUIRE(rows(db, "SELECT u.name, i.sku FROM users u JOIN orders o ON u.id = o.user_id "
                     "JOIN items i ON o.id = i.order_id") ==
            std::vector<Row>{{s("ama"), s("x")}, {s("ama"), s("y")}, {s("esi"), s("z")}});
    REQUIRE(rows(db, "SELECT u.name, i.sku FROM users u, orders o, items i "
                     "WHERE u.id = o.user_id AND o.id = i.order_id AND i.sku <> 'y'") ==
            std::vector<Row>{{s("ama"), s("x")}, {s("esi"), s("z")}});
}

TEST_CASE("join: select star lists the left table's columns then the right's") {
    Database db = make_db();
    QueryResult r = db.execute("SELECT * FROM users u JOIN orders o ON u.id = o.user_id WHERE o.id = 12");
    REQUIRE(r.columns == std::vector<std::string>{"id", "name", "age", "id", "user_id", "amount"});
    REQUIRE(r.rows == std::vector<Row>{{i(3), s("esi"), i(41), i(12), i(3), Value(2.0)}});
}

TEST_CASE("join: an empty side gives nothing") {
    Database db = make_db();
    db.execute("CREATE TABLE nobody (id INT)");
    REQUIRE(rows(db, "SELECT u.id FROM users u JOIN nobody n ON u.id = n.id").empty());
    REQUIRE(rows(db, "SELECT u.id FROM nobody n JOIN users u ON u.id = n.id").empty());
    REQUIRE(rows(db, "SELECT u.id FROM users u, nobody n").empty());
}

TEST_CASE("join: errors in the condition come out of the query") {
    Database db = make_db();
    REQUIRE_THROWS_AS(db.execute("SELECT u.id FROM users u JOIN orders o ON u.id / 0 = 1"), DbError);
}

TEST_CASE("join: runs the same with the optimizer off") {
    Database on = make_db();
    Database off = make_db();
    off.set_optimizer_enabled(false);
    const char* queries[] = {"SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id AND 1 = 1",
                             "SELECT u.id, o.id FROM users u, orders o WHERE TRUE AND o.amount > 1 + 1",
                             "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE 1 > 2"};
    for (const char* sql : queries) {
        INFO(sql);
        REQUIRE(on.execute(sql).rows == off.execute(sql).rows);
    }
}

TEST_CASE("join operator: pairs tested, rows out, and the right side read once") {
    Database db = make_db();
    const Table* users = db.catalog().get_table("users");
    const Table* orders = db.catalog().get_table("orders");

    Scope scope;
    scope.add("u", users->info());
    scope.add("o", orders->info());
    BoundExprPtr condition = bind_expression(*parse_expression("u.id = o.user_id"), scope);

    auto left = std::make_unique<SeqScan>(*users);
    auto right = std::make_unique<SeqScan>(*orders);
    const Operator* left_ptr = left.get();
    const Operator* right_ptr = right.get();
    NestedLoopJoin join(std::move(left), std::move(right), condition);

    std::vector<Row> out;
    while (auto row = join.next()) out.push_back(*row);

    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == Row{i(1), s("ama"), i(30), i(10), i(1), Value(5.0)});
    REQUIRE(join.stats().rows_out == 3);
    REQUIRE(join.stats().rows_scanned == 12);  // 3 left rows x 4 right rows
    REQUIRE(left_ptr->stats().rows_out == 3);
    REQUIRE(right_ptr->stats().rows_out == 4);  // read once, not once per left row
}

TEST_CASE("join operator: a LIMIT above stops the join early") {
    Database db = make_db();
    const Table* users = db.catalog().get_table("users");
    const Table* orders = db.catalog().get_table("orders");
    NestedLoopJoin join(std::make_unique<SeqScan>(*users), std::make_unique<SeqScan>(*orders), nullptr);
    REQUIRE(join.next().has_value());
    REQUIRE(join.next().has_value());
    REQUIRE(join.stats().rows_scanned == 2);
}

TEST_CASE("physical plan: positions run across both sides") {
    Database db = make_db();
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(
        "SELECT u.name, o.amount FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 1").node),
        db.catalog());
    REQUIRE(print(*plan_physical(*plan_select(bound))) ==
            "Project[#1, #5]\n"
            "  Filter[#5 > 1]\n"
            "    NestedLoopJoin[#0 = #4]\n"
            "      SeqScan[users]\n"
            "      SeqScan[orders]");
}

TEST_CASE("physical plan: a cross join, and a self-join") {
    Database db = make_db();
    auto physical = [&](const char* sql) {
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        return print(*plan_physical(*plan_select(bound)));
    };
    REQUIRE(physical("SELECT u.id FROM users u, orders o") ==
            "Project[#0]\n"
            "  NestedLoopJoin[CROSS]\n"
            "    SeqScan[users]\n"
            "    SeqScan[orders]");
    REQUIRE(physical("SELECT b.name FROM users a JOIN users b ON a.id = b.id") ==
            "Project[#4]\n"
            "  NestedLoopJoin[#0 = #3]\n"
            "    SeqScan[users]\n"
            "    SeqScan[users]");
}
