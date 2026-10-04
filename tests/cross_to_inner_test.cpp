#include "optimizer/cross_to_inner.h"

#include "engine/result_format.h"
#include "optimizer/filter_pushdown.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

void becomes(const std::string& before, const std::string& after) {
    assert_rewrite(std::make_unique<CrossToInnerJoin>(), before, after);
}

void unchanged(const std::string& sql) { assert_unchanged(std::make_unique<CrossToInnerJoin>(), sql); }

}  // namespace

TEST_CASE("cross to inner: a comma join with an equality is the same plan as JOIN ... ON") {
    becomes("SELECT u.name FROM users u, orders o WHERE u.id = o.user_id",
            "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    becomes("SELECT u.name FROM users u, orders o WHERE o.user_id = u.id",
            "SELECT u.name FROM users u JOIN orders o ON o.user_id = u.id");
    becomes("SELECT * FROM users a, users b WHERE a.id = b.id",
            "SELECT * FROM users a JOIN users b ON a.id = b.id");
}

TEST_CASE("cross to inner: the plan text") {
    assert_rewrite_to_text(std::make_unique<CrossToInnerJoin>(),
                           "SELECT u.name FROM users u, orders o WHERE u.id = o.user_id",
                           "Project[name]\n"
                           "  Join[INNER ON u.id = user_id]\n"
                           "    Scan[users AS u]\n"
                           "    Scan[orders AS o]");
}

TEST_CASE("cross to inner: each side may be an expression") {
    becomes("SELECT u.name FROM users u, orders o WHERE u.id + 1 = o.user_id * 2",
            "SELECT u.name FROM users u JOIN orders o ON u.id + 1 = o.user_id * 2");
}

TEST_CASE("cross to inner: every part that reads both sides joins the condition") {
    becomes("SELECT u.name FROM users u, orders o WHERE u.id = o.user_id AND u.age > o.amount",
            "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id AND u.age > o.amount");
    becomes("SELECT u.name FROM users u, orders o WHERE u.age > o.amount AND u.id = o.user_id",
            "SELECT u.name FROM users u JOIN orders o ON u.age > o.amount AND u.id = o.user_id");
}

TEST_CASE("cross to inner: parts that read one side stay in the filter") {
    assert_rewrite_to_text(std::make_unique<CrossToInnerJoin>(),
                           "SELECT u.name FROM users u, orders o WHERE u.age > 30 AND u.id = o.user_id AND o.amount > 5",
                           "Project[name]\n"
                           "  Filter[age > 30 AND amount > 5]\n"
                           "    Join[INNER ON u.id = user_id]\n"
                           "      Scan[users AS u]\n"
                           "      Scan[orders AS o]");
}

TEST_CASE("cross to inner: needs an equality that really joins the two sides") {
    unchanged("SELECT u.name FROM users u, orders o WHERE u.age > o.amount");
    unchanged("SELECT u.name FROM users u, orders o WHERE u.id + o.id = 5");
    unchanged("SELECT u.name FROM users u, orders o WHERE u.id = u.age");
    unchanged("SELECT u.name FROM users u, orders o WHERE u.id = 5");
    unchanged("SELECT u.name FROM users u, orders o WHERE u.id = o.user_id OR u.age = 1");
    unchanged("SELECT u.name FROM users u, orders o");
    unchanged("SELECT name FROM users WHERE id = age");
}

TEST_CASE("cross to inner: an inner join is left alone") {
    unchanged("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.id = o.id");
}

TEST_CASE("cross to inner: three tables, with pushdown, match the written-out joins") {
    RuleOptimizer optimizer = optimizer_of_all<FilterPushdown, CrossToInnerJoin>();
    Planned p = plan_sql("SELECT u.name FROM users u, orders o, items i WHERE u.id = o.user_id AND o.id = i.order_id");
    OptimizeResult result = optimizer.optimize(p.plan);
    REQUIRE(print(*result.plan, p.bound.scope) ==
            plan_text("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id"));

    // The order of the parts doesn't matter.
    Planned q = plan_sql("SELECT u.name FROM users u, orders o, items i WHERE o.id = i.order_id AND u.id = o.user_id");
    REQUIRE(print(*optimizer.optimize(q.plan).plan, q.bound.scope) ==
            "Project[name]\n"
            "  Join[INNER ON o.id = order_id]\n"
            "    Join[INNER ON u.id = user_id]\n"
            "      Scan[users AS u]\n"
            "      Scan[orders AS o]\n"
            "    Scan[items AS i]");
}

TEST_CASE("cross to inner: in the demo query") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, country TEXT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    REQUIRE(format_result(db.execute("EXPLAIN SELECT u.name FROM users u, orders o "
                                     "WHERE u.id = o.user_id AND u.country = 'Ghana' AND o.amount > 100")) ==
            "Original plan\n"
            "  Project[name]\n"
            "    Filter[u.id = user_id AND country = 'Ghana' AND amount > 100]\n"
            "      Join[CROSS]\n"
            "        Scan[users AS u]\n"
            "        Scan[orders AS o]\n"
            "\n"
            "Rules fired\n"
            "  1. filter-pushdown (pass 1)\n"
            "  2. cross-to-inner-join (pass 1)\n"
            "  3. column-pruning (pass 2)\n"
            "\n"
            "Final plan\n"
            "  Project[name]\n"
            "    Join[INNER ON u.id = user_id]\n"
            "      Prune[u.id, name]\n"
            "        Filter[country = 'Ghana']\n"
            "          Scan[users AS u]\n"
            "      Prune[user_id]\n"
            "        Filter[amount > 100]\n"
            "          Prune[user_id, amount]\n"
            "            Scan[orders AS o]");
}

TEST_CASE("cross to inner: same rows as the comma join, NULLs included") {
    Database on;
    Database off;
    off.set_optimizer_enabled(false);
    for (Database* db : {&on, &off}) {
        db->execute("CREATE TABLE users (id INT, name TEXT, age INT)");
        db->execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
        db->execute("CREATE TABLE items (id INT, order_id INT, sku TEXT)");
        db->execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 41), (NULL, 'ghost', 5)");
        db->execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 70.5), (12, 3, 2.0), (13, NULL, 1.0)");
        db->execute("INSERT INTO items VALUES (100, 10, 'x'), (101, 10, 'y'), (102, 12, 'z'), (103, NULL, 'w')");
    }
    const char* queries[] = {
        "SELECT u.name, o.id FROM users u, orders o WHERE u.id = o.user_id",
        "SELECT u.name, o.id FROM users u, orders o WHERE o.user_id = u.id AND u.age > 25",
        "SELECT u.name, o.id FROM users u, orders o WHERE u.id = o.user_id AND u.age > o.amount",
        "SELECT u.name, i.sku FROM users u, orders o, items i WHERE u.id = o.user_id AND o.id = i.order_id",
        "SELECT u.name, i.sku FROM users u, orders o, items i WHERE o.id = i.order_id AND u.id = o.user_id AND i.sku <> 'y'",
        "SELECT a.name, b.name FROM users a, users b WHERE a.id = b.id",
        "SELECT a.name, b.name FROM users a, users b WHERE a.id + 1 = b.id",
        "SELECT u.name FROM users u, orders o WHERE u.id = o.user_id OR u.age = 20",
    };
    for (const char* sql : queries) {
        INFO(sql);
        std::vector<Row> a = on.execute(sql).rows;
        std::vector<Row> b = off.execute(sql).rows;
        auto key = [](const Row& r) { std::string s; for (const Value& v : r) s += v.to_string() + "|"; return s; };
        auto by_key = [&](const Row& x, const Row& y) { return key(x) < key(y); };
        std::sort(a.begin(), a.end(), by_key);
        std::sort(b.begin(), b.end(), by_key);
        REQUIRE(a == b);
    }
}
