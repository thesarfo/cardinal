#include "optimizer/column_pruning.h"
#include "optimizer/default_rules.h"

#include "exec/nested_loop_join.h"
#include "optimizer/cross_to_inner.h"
#include "optimizer/filter_pushdown.h"
#include "physical/physical_printer.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

void prunes_to(const std::string& sql, const std::string& expected_plan) {
    assert_rewrite_to_text(std::make_unique<ColumnPruning>(), sql, expected_plan);
}

constexpr const char* kJoin = "FROM users u JOIN orders o ON u.id = o.user_id";

// Everything that runs before pruning, then pruning, as the engine does.
std::string optimized(const std::string& sql) {
    Planned p = plan_sql(sql);
    return print(*RuleOptimizer::staged(default_stages()).optimize(p.plan).plan, p.bound.scope);
}

}  // namespace

TEST_CASE("pruning: only the columns the select list and the join condition need") {
    prunes_to(std::string("SELECT u.name ") + kJoin,
              "Project[name]\n"
              "  Join[INNER ON u.id = user_id]\n"
              "    Prune[u.id, name]\n"
              "      Scan[users AS u]\n"
              "    Prune[user_id]\n"
              "      Scan[orders AS o]");
}

TEST_CASE("pruning: a column used only in the join condition survives until the join") {
    prunes_to(std::string("SELECT o.amount ") + kJoin,
              "Project[amount]\n"
              "  Join[INNER ON u.id = user_id]\n"
              "    Prune[u.id]\n"
              "      Scan[users AS u]\n"
              "    Prune[user_id, amount]\n"
              "      Scan[orders AS o]");
}

TEST_CASE("pruning: a column used only in ORDER BY survives until the sort") {
    prunes_to(std::string("SELECT u.name ") + kJoin + " ORDER BY o.amount",
              "Project[name]\n"
              "  Sort[amount ASC]\n"
              "    Join[INNER ON u.id = user_id]\n"
              "      Prune[u.id, name]\n"
              "        Scan[users AS u]\n"
              "      Prune[user_id, amount]\n"
              "        Scan[orders AS o]");
}

TEST_CASE("pruning: a column used only in a filter above the join survives until the filter") {
    prunes_to(std::string("SELECT u.name ") + kJoin + " WHERE u.age > o.amount",
              "Project[name]\n"
              "  Filter[age > amount]\n"
              "    Join[INNER ON u.id = user_id]\n"
              "      Scan[users AS u]\n"
              "      Prune[user_id, amount]\n"
              "        Scan[orders AS o]");
}

TEST_CASE("pruning: a filter below the join drops its column once it has run") {
    Planned p = plan_sql(std::string("SELECT u.name ") + kJoin + " WHERE o.amount > 100");
    RuleOptimizer optimizer = optimizer_of_all<FilterPushdown, ColumnPruning>();
    REQUIRE(print(*optimizer.optimize(p.plan).plan, p.bound.scope) ==
            "Project[name]\n"
            "  Join[INNER ON u.id = user_id]\n"
            "    Prune[u.id, name]\n"
            "      Scan[users AS u]\n"
            "    Prune[user_id]\n"
            "      Filter[amount > 100]\n"
            "        Prune[user_id, amount]\n"
            "          Scan[orders AS o]");
}

TEST_CASE("pruning: nothing to drop") {
    assert_unchanged(std::make_unique<ColumnPruning>(), std::string("SELECT * ") + kJoin);
    assert_unchanged(std::make_unique<ColumnPruning>(),
                     "SELECT u.id, u.name, u.age, o.id, o.user_id, o.amount FROM users u, orders o");
}

TEST_CASE("pruning: plans without a join are left alone") {
    assert_unchanged(std::make_unique<ColumnPruning>(), "SELECT name FROM users");
    assert_unchanged(std::make_unique<ColumnPruning>(), "SELECT name FROM users WHERE age > 30 ORDER BY id LIMIT 2");
}

TEST_CASE("pruning: a select list with no columns still needs the join's columns") {
    prunes_to(std::string("SELECT 1 ") + kJoin,
              "Project[1]\n"
              "  Join[INNER ON u.id = user_id]\n"
              "    Prune[u.id]\n"
              "      Scan[users AS u]\n"
              "    Prune[user_id]\n"
              "      Scan[orders AS o]");
    prunes_to("SELECT 1 FROM users u, orders o",
              "Project[1]\n"
              "  Join[CROSS]\n"
              "    Prune[]\n"
              "      Scan[users AS u]\n"
              "    Prune[]\n"
              "      Scan[orders AS o]");
}

TEST_CASE("pruning: a self-join keeps the two sides' columns apart") {
    prunes_to("SELECT a.name FROM users a JOIN users b ON a.id = b.id",
              "Project[a.name]\n"
              "  Join[INNER ON a.id = b.id]\n"
              "    Prune[a.id, a.name]\n"
              "      Scan[users AS a]\n"
              "    Prune[b.id]\n"
              "      Scan[users AS b]");
}

TEST_CASE("pruning: a three-table join narrows the middle join's output too") {
    prunes_to("SELECT i.sku FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id",
              "Project[sku]\n"
              "  Join[INNER ON o.id = order_id]\n"
              "    Prune[o.id]\n"
              "      Join[INNER ON u.id = user_id]\n"
              "        Prune[u.id]\n"
              "          Scan[users AS u]\n"
              "        Prune[o.id, user_id]\n"
              "          Scan[orders AS o]\n"
              "    Prune[order_id, sku]\n"
              "      Scan[items AS i]");
}

TEST_CASE("pruning: the default pipeline pushes filters first, then prunes") {
    REQUIRE(optimized("SELECT u.name FROM users u, orders o, items i "
                      "WHERE u.id = o.user_id AND o.id = i.order_id AND o.amount > 5 AND i.sku = 'x'") ==
            "Project[name]\n"
            "  Join[INNER ON o.id = order_id]\n"
            "    Prune[name, o.id]\n"
            "      Join[INNER ON u.id = user_id]\n"
            "        Prune[u.id, name]\n"
            "          Scan[users AS u]\n"
            "        Prune[o.id, user_id]\n"
            "          Filter[amount > 5]\n"
            "            Scan[orders AS o]\n"
            "    Prune[order_id]\n"
            "      Filter[sku = 'x']\n"
            "        Prune[order_id, sku]\n"
            "          Scan[items AS i]");
}

TEST_CASE("pruning: positions in the physical plan are for the narrow rows") {
    Planned p = plan_sql(std::string("SELECT u.name ") + kJoin + " WHERE u.age > 30");
    PlanPtr plan = optimizer_of_all<FilterPushdown, ColumnPruning>().optimize(p.plan).plan;
    REQUIRE(print(*plan_physical(*plan)) ==
            "Project[#1]\n"
            "  NestedLoopJoin[#0 = #2]\n"
            "    Project[#0, #1]\n"
            "      Filter[#2 > 30]\n"
            "        SeqScan[users]\n"
            "    Project[#1]\n"
            "      SeqScan[orders]");
}

TEST_CASE("pruning: joined rows are narrower") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20)");
    db.execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 2, 7.5)");

    Planned p = plan_sql(std::string("SELECT u.name ") + kJoin);
    PlanPtr pruned = optimizer_of(std::make_unique<ColumnPruning>()).optimize(p.plan).plan;

    // Take the join out of the plan and look at the rows it produces.
    auto join_width = [&](const PlanPtr& plan) {
        PlanPtr join = input_of(*plan);
        std::unique_ptr<Operator> root = build_operator(*plan_physical(*join), db.catalog());
        return root->next()->size();
    };
    REQUIRE(join_width(p.plan) == 6);
    REQUIRE(join_width(pruned) == 3);  // name, and the two join keys
}

TEST_CASE("pruning: same rows as without it, NULLs included") {
    Database on;
    Database off;
    off.set_optimizer_enabled(false);
    for (Database* db : {&on, &off}) {
        db->execute("CREATE TABLE users (id INT, name TEXT, age INT)");
        db->execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
        db->execute("CREATE TABLE items (id INT, order_id INT, sku TEXT)");
        db->execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41), (4, 'yaw', NULL)");
        db->execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 70.5), (12, 3, 2.0), (13, NULL, 1.0), (14, 4, NULL)");
        db->execute("INSERT INTO items VALUES (100, 10, 'x'), (101, 10, 'y'), (102, 12, 'z'), (103, 99, 'w'), (104, 11, NULL)");
    }
    const char* queries[] = {
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id",
        "SELECT o.amount FROM users u JOIN orders o ON u.id = o.user_id",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id ORDER BY o.amount, u.name",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age > o.amount",
        "SELECT 1 FROM users u, orders o",
        "SELECT i.sku FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id",
        "SELECT u.name, i.sku FROM users u, orders o, items i WHERE u.id = o.user_id AND o.id = i.order_id AND o.amount > 1",
        "SELECT a.name FROM users a JOIN users b ON a.id = b.id WHERE b.age IS NULL",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id ORDER BY o.id DESC LIMIT 2",
        "SELECT o.id, u.id FROM users u JOIN orders o ON u.id = o.user_id",
    };
    for (const char* sql : queries) {
        INFO(sql);
        std::vector<Row> a = on.execute(sql).rows;
        std::vector<Row> b = off.execute(sql).rows;
        if (std::string(sql).find("ORDER BY") == std::string::npos) {
            auto key = [](const Row& r) { std::string s; for (const Value& v : r) s += v.to_string() + "|"; return s; };
            auto by_key = [&](const Row& x, const Row& y) { return key(x) < key(y); };
            std::sort(a.begin(), a.end(), by_key);
            std::sort(b.begin(), b.end(), by_key);
        }
        REQUIRE(a == b);
    }
}
