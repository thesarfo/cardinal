#include "optimizer/filter_pushdown.h"

#include "engine/datagen.h"
#include "engine/result_format.h"
#include "optimizer/filter_cleanup.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

void pushes(const std::string& sql, const std::string& expected_plan) {
    assert_rewrite_to_text(std::make_unique<FilterPushdown>(), sql, expected_plan);
}

constexpr const char* kJoin = "FROM users u JOIN orders o ON u.id = o.user_id ";

}  // namespace

TEST_CASE("pushdown: a condition on the left side goes to the left input") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30",
           "Project[name]\n"
           "  Join[INNER ON u.id = user_id]\n"
           "    Filter[age > 30]\n"
           "      Scan[users AS u]\n"
           "    Scan[orders AS o]");
}

TEST_CASE("pushdown: a condition on the right side goes to the right input") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE o.amount > 100",
           "Project[name]\n"
           "  Join[INNER ON u.id = user_id]\n"
           "    Scan[users AS u]\n"
           "    Filter[amount > 100]\n"
           "      Scan[orders AS o]");
}

TEST_CASE("pushdown: each part of an AND goes its own way") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30 AND o.amount > 100",
           "Project[name]\n"
           "  Join[INNER ON u.id = user_id]\n"
           "    Filter[age > 30]\n"
           "      Scan[users AS u]\n"
           "    Filter[amount > 100]\n"
           "      Scan[orders AS o]");
}

TEST_CASE("pushdown: a condition that reads both sides stays above the join") {
    assert_unchanged(std::make_unique<FilterPushdown>(),
                     std::string("SELECT u.name ") + kJoin + "WHERE u.age > o.amount");
    assert_unchanged(std::make_unique<FilterPushdown>(),
                     "SELECT u.name FROM users u, orders o WHERE u.id = o.user_id");
}

TEST_CASE("pushdown: a mix of one side, both sides and the other side") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30 AND u.id < o.user_id AND o.amount > 100",
           "Project[name]\n"
           "  Filter[u.id < user_id]\n"
           "    Join[INNER ON u.id = user_id]\n"
           "      Filter[age > 30]\n"
           "        Scan[users AS u]\n"
           "      Filter[amount > 100]\n"
           "        Scan[orders AS o]");
}

TEST_CASE("pushdown: several parts for one side are joined with AND, in order") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30 AND o.amount > 1 AND u.id <> 7 AND o.id > 2",
           "Project[name]\n"
           "  Join[INNER ON u.id = user_id]\n"
           "    Filter[age > 30 AND u.id <> 7]\n"
           "      Scan[users AS u]\n"
           "    Filter[amount > 1 AND o.id > 2]\n"
           "      Scan[orders AS o]");
}

TEST_CASE("pushdown: a cross join takes pushed filters too") {
    pushes("SELECT u.name FROM users u, orders o WHERE u.age > 30 AND o.amount > 100",
           "Project[name]\n"
           "  Join[CROSS]\n"
           "    Filter[age > 30]\n"
           "      Scan[users AS u]\n"
           "    Filter[amount > 100]\n"
           "      Scan[orders AS o]");
}

TEST_CASE("pushdown: constants stay where they are") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE 1 = 1 AND u.age > 30",
           "Project[name]\n"
           "  Filter[1 = 1]\n"
           "    Join[INNER ON u.id = user_id]\n"
           "      Filter[age > 30]\n"
           "        Scan[users AS u]\n"
           "      Scan[orders AS o]");
    assert_unchanged(std::make_unique<FilterPushdown>(), std::string("SELECT u.name ") + kJoin + "WHERE FALSE");
}

TEST_CASE("pushdown: a three-table join pushes each part all the way down") {
    pushes("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id "
           "WHERE u.age > 30 AND i.sku = 'x' AND o.amount > 5",
           "Project[name]\n"
           "  Join[INNER ON o.id = order_id]\n"
           "    Join[INNER ON u.id = user_id]\n"
           "      Filter[age > 30]\n"
           "        Scan[users AS u]\n"
           "      Filter[amount > 5]\n"
           "        Scan[orders AS o]\n"
           "    Filter[sku = 'x']\n"
           "      Scan[items AS i]");
}

TEST_CASE("pushdown: a self-join keeps the two sides apart") {
    pushes("SELECT a.name FROM users a JOIN users b ON a.id = b.id WHERE a.age > 30 AND b.age < 50",
           "Project[a.name]\n"
           "  Join[INNER ON a.id = b.id]\n"
           "    Filter[a.age > 30]\n"
           "      Scan[users AS a]\n"
           "    Filter[b.age < 50]\n"
           "      Scan[users AS b]");
}

TEST_CASE("pushdown: single-table queries and filters not over a join are left alone") {
    assert_unchanged(std::make_unique<FilterPushdown>(), "SELECT name FROM users WHERE age > 30");
    assert_unchanged(std::make_unique<FilterPushdown>(), "SELECT name FROM users");
    assert_unchanged(std::make_unique<FilterPushdown>(),
                     std::string("SELECT u.name ") + kJoin);
}

TEST_CASE("pushdown: ORDER BY and LIMIT above the filter stay above") {
    pushes(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30 ORDER BY o.amount LIMIT 3",
           "Limit[3]\n"
           "  Project[name]\n"
           "    Sort[amount ASC]\n"
           "      Join[INNER ON u.id = user_id]\n"
           "        Filter[age > 30]\n"
           "          Scan[users AS u]\n"
           "        Scan[orders AS o]");
}

TEST_CASE("pushdown: refuses a join type it doesn't know") {
    // Stands in for a LEFT JOIN, which doesn't exist yet: the rule must not touch it.
    Planned p = plan_sql(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30");
    const auto& project = std::get<LogicalProject>(p.plan->node);
    const auto& filter = std::get<LogicalFilter>(project.input->node);
    LogicalJoin odd = std::get<LogicalJoin>(filter.input->node);
    odd.type = static_cast<JoinType>(99);
    PlanPtr bad_join = std::make_shared<const LogicalPlan>(LogicalPlan{odd});
    PlanPtr bad_filter = std::make_shared<const LogicalPlan>(LogicalPlan{LogicalFilter{bad_join, filter.predicate}});

    OptimizeResult result = optimizer_of(std::make_unique<FilterPushdown>()).optimize(bad_filter);
    REQUIRE(result.trace.empty());
    REQUIRE(result.plan == bad_filter);
}

TEST_CASE("pushdown: the plan from the project's demo query") {
    Database db;
    generate_users_orders(db, {.users = 1000});
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    REQUIRE(format_result(db.execute("EXPLAIN SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id "
                                     "WHERE u.country = 'Ghana' AND o.amount > 100")) ==
            "Original plan\n"
            "  Project[name]  est_rows=234\n"
            "    Filter[country = 'Ghana' AND amount > 100]  est_rows=234\n"
            "      Join[INNER ON u.id = user_id]  est_rows=4000\n"
            "        Scan[users AS u]  est_rows=1000\n"
            "        Scan[orders AS o]  est_rows=4000\n"
            "\n"
            "Rules fired\n"
            "  1. filter-pushdown (pass 1)\n"
            "  2. column-pruning (pass 2)\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=234\n"
            "    Join[INNER ON u.id = user_id]  est_rows=234\n"
            "      Prune[u.id, name]  est_rows=65\n"
            "        Filter[country = 'Ghana']  est_rows=65\n"
            "          Prune[u.id, name, country]  est_rows=1000\n"
            "            Scan[users AS u]  est_rows=1000\n"
            "      Prune[user_id]  est_rows=3602\n"
            "        Filter[amount > 100]  est_rows=3602\n"
            "          Prune[user_id, amount]  est_rows=4000\n"
            "            Scan[orders AS o]  est_rows=4000\n"
            "\n"
            "Statistics\n"
            "  users: analyzed when it had 1000 rows\n"
            "  orders: analyzed when it had 4000 rows");
}

TEST_CASE("pushdown: pushed filters merge with filters already on the side") {
    Planned p = plan_sql(std::string("SELECT u.name ") + kJoin + "WHERE u.age > 30");
    const auto& project = std::get<LogicalProject>(p.plan->node);
    const auto& filter = std::get<LogicalFilter>(project.input->node);
    const auto& join = std::get<LogicalJoin>(filter.input->node);

    // users already has a filter of its own; a second one arrives from above.
    Planned other = plan_sql("SELECT name FROM users WHERE id > 1");
    PlanPtr existing = std::make_shared<const LogicalPlan>(LogicalPlan{LogicalFilter{
        join.left, std::get<LogicalFilter>(std::get<LogicalProject>(other.plan->node).input->node).predicate}});
    PlanPtr join_with_filter = std::make_shared<const LogicalPlan>(
        LogicalPlan{LogicalJoin{existing, join.right, join.type, join.condition}});
    PlanPtr top = std::make_shared<const LogicalPlan>(LogicalPlan{LogicalFilter{join_with_filter, filter.predicate}});
    PlanPtr stacked = std::make_shared<const LogicalPlan>(LogicalPlan{LogicalProject{top, project.items}});

    OptimizeResult merged = optimizer_of_all<FilterPushdown, MergeFilters>().optimize(stacked);
    REQUIRE(print(*merged.plan, p.bound.scope) ==
            "Project[name]\n"
            "  Join[INNER ON u.id = user_id]\n"
            "    Filter[u.id > 1 AND age > 30]\n"
            "      Scan[users AS u]\n"
            "    Scan[orders AS o]");
}

TEST_CASE("pushdown: gives the same rows as not pushing, NULLs included") {
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
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age > 25",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 3",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age > 25 AND o.amount > 3",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age > o.amount",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age IS NULL OR o.amount IS NULL",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE NOT (u.age > 25 AND o.amount > 3)",
        "SELECT u.name, o.id FROM users u, orders o WHERE u.age > 25 AND o.amount < 10",
        "SELECT u.name, o.id FROM users u, orders o WHERE u.id = o.user_id AND o.amount < 10",
        "SELECT u.name, i.sku FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id "
        "WHERE u.age > 25 AND i.sku <> 'y' AND o.amount > 1",
        "SELECT a.name, b.name FROM users a JOIN users b ON a.id <> b.id WHERE a.age > 25 AND b.age < 45",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age BETWEEN 20 AND 41 AND o.amount IN (5.0, 2.0)",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE FALSE",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age > 25 ORDER BY o.amount DESC LIMIT 2",
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
