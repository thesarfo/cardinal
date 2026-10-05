#include "common/error.h"
#include "optimizer/boolean_cleanup.h"
#include "optimizer/constant_folding.h"
#include "engine/result_format.h"
#include "sql/ast_printer.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

std::string parsed(const char* sql) { return print(parse_statement(sql)); }

void parse_error(const char* sql, const char* message, int column) {
    INFO(sql);
    try {
        parse_statement(sql);
        FAIL("expected a ParseError");
    } catch (const ParseError& e) {
        REQUIRE(e.message() == message);
        REQUIRE(e.column() == column);
    }
}

std::string bind_error(const char* sql) {
    try {
        bind_select(std::get<Select>(parse_statement(sql).node), test_catalog());
    } catch (const DbError& e) {
        return e.what();
    }
    return "no error";
}

}  // namespace

TEST_CASE("parse: aliases") {
    REQUIRE(parsed("SELECT * FROM users u") == "(select (star) (from users u))");
    REQUIRE(parsed("SELECT * FROM users AS u") == "(select (star) (from users u))");
    REQUIRE(parsed("SELECT u.name FROM users u WHERE u.id = 1") ==
            "(select (col u.name) (from users u) (where (= (col u.id) 1)))");
}

TEST_CASE("parse: JOIN ... ON") {
    REQUIRE(parsed("SELECT * FROM a JOIN b ON a.id = b.id") ==
            "(select (star) (from a) (inner-join b (on (= (col a.id) (col b.id)))))");
    REQUIRE(parsed("SELECT * FROM a INNER JOIN b ON a.id = b.id") == parsed("SELECT * FROM a JOIN b ON a.id = b.id"));
    REQUIRE(parsed("SELECT * FROM users u JOIN orders AS o ON u.id = o.user_id WHERE o.amount > 100") ==
            "(select (star) (from users u) (inner-join orders o (on (= (col u.id) (col o.user_id)))) "
            "(where (> (col o.amount) 100)))");
    REQUIRE(parsed("SELECT * FROM a JOIN b ON a.x = b.x AND a.y = b.y OR a.z = 1") ==
            "(select (star) (from a) (inner-join b (on (or (and (= (col a.x) (col b.x)) (= (col a.y) (col b.y))) "
            "(= (col a.z) 1)))))");
}

TEST_CASE("parse: comma joins and chains") {
    REQUIRE(parsed("SELECT * FROM a, b") == "(select (star) (from a) (cross-join b))");
    REQUIRE(parsed("SELECT * FROM a x, b y, c WHERE x.id = y.id") ==
            "(select (star) (from a x) (cross-join b y) (cross-join c) (where (= (col x.id) (col y.id))))");
    REQUIRE(parsed("SELECT * FROM a JOIN b ON a.id = b.id JOIN c ON b.id = c.id") ==
            "(select (star) (from a) (inner-join b (on (= (col a.id) (col b.id)))) "
            "(inner-join c (on (= (col b.id) (col c.id)))))");
    REQUIRE(parsed("SELECT * FROM a, b JOIN c ON b.id = c.id") ==
            "(select (star) (from a) (cross-join b) (inner-join c (on (= (col b.id) (col c.id)))))");
}

TEST_CASE("parse: where, order by and limit still follow the tables") {
    REQUIRE(parsed("SELECT a.x FROM a JOIN b ON a.id = b.id WHERE a.x > 1 ORDER BY a.x LIMIT 2") ==
            "(select (col a.x) (from a) (inner-join b (on (= (col a.id) (col b.id)))) (where (> (col a.x) 1)) "
            "(order-by (asc (col a.x))) (limit 2))");
}

TEST_CASE("parse: join errors") {
    parse_error("SELECT * FROM a JOIN b", "expected 'ON'", 23);
    parse_error("SELECT * FROM a JOIN ON x", "expected a table name", 22);
    parse_error("SELECT * FROM a INNER b", "expected 'JOIN'", 23);
    parse_error("SELECT * FROM a AS", "expected an alias after AS", 19);
    parse_error("SELECT * FROM a,", "expected a table name", 17);
    parse_error("SELECT * FROM a JOIN b ON", "expected an expression", 26);
    parse_error("SELECT * FROM a WHERE JOIN", "expected an expression", 23);
}

TEST_CASE("bind: a three-table join") {
    Planned p = plan_sql(
        "SELECT u.name, o.amount, i.sku FROM users u JOIN orders o ON u.id = o.user_id "
        "JOIN items i ON o.id = i.order_id");
    REQUIRE(p.bound.scope.tables().size() == 3);
    REQUIRE(p.bound.joins.size() == 2);
    REQUIRE(print(*p.plan, p.bound.scope) ==
            "Project[name, amount, sku]\n"
            "  Join[INNER ON o.id = order_id]\n"
            "    Join[INNER ON u.id = user_id]\n"
            "      Scan[users AS u]\n"
            "      Scan[orders AS o]\n"
            "    Scan[items AS i]");
    // users #0-2, orders #3-5, items #6-8
    REQUIRE(print(*p.bound.joins[0].condition) == "(= #0 #4)");
    REQUIRE(print(*p.bound.joins[1].condition) == "(= #3 #7)");
}

TEST_CASE("bind: a self-join gives each side its own ids") {
    Planned p = plan_sql("SELECT a.name, b.name FROM users a JOIN users b ON a.id = b.id");
    REQUIRE(print(*p.bound.joins[0].condition) == "(= #0 #3)");
    REQUIRE(print(*p.bound.items[0].expr) == "#1");
    REQUIRE(print(*p.bound.items[1].expr) == "#4");
    REQUIRE(print(*p.plan, p.bound.scope) ==
            "Project[a.name, b.name]\n"
            "  Join[INNER ON a.id = b.id]\n"
            "    Scan[users AS a]\n"
            "    Scan[users AS b]");
}

TEST_CASE("bind: comma joins have no condition") {
    Planned p = plan_sql("SELECT u.name FROM users u, orders o WHERE u.id = o.user_id");
    REQUIRE(p.bound.joins[0].condition == nullptr);
    REQUIRE(print(*p.plan, p.bound.scope) ==
            "Project[name]\n"
            "  Filter[u.id = user_id]\n"
            "    Join[CROSS]\n"
            "      Scan[users AS u]\n"
            "      Scan[orders AS o]");
}

TEST_CASE("bind: star covers every table, in order") {
    Planned p = plan_sql("SELECT * FROM users u, orders o");
    REQUIRE(p.bound.items.size() == 6);
    REQUIRE(p.bound.items[3].name == "id");
    REQUIRE(print(*p.bound.items[3].expr) == "#3");
}

TEST_CASE("bind: names") {
    REQUIRE(bind_error("SELECT id FROM users u JOIN orders o ON u.id = o.user_id") ==
            "column id is ambiguous: it could be u.id or o.id");
    REQUIRE(bind_error("SELECT u.id FROM users u WHERE users.id = 1") == "unknown table or alias users");
    REQUIRE(bind_error("SELECT * FROM users JOIN users ON users.id = users.id") ==
            "table name users is used twice; give one an alias");
    REQUIRE(bind_error("SELECT * FROM users u JOIN orders o ON u.id = i.order_id JOIN items i ON o.id = i.order_id") ==
            "unknown table or alias i");
    REQUIRE(bind_error("SELECT * FROM users u JOIN nope n ON u.id = n.id") == "unknown table nope");
    REQUIRE(bind_error("SELECT * FROM users u JOIN orders o ON u.id") == "ON needs a true/false value, got INT");
    REQUIRE(bind_error("SELECT * FROM users u JOIN orders o ON u.name = o.id") == "cannot compare TEXT with INT");
    REQUIRE(bind_error("SELECT * FROM users u JOIN orders o ON u.id = o.user_id WHERE nope = 1") == "unknown column nope");
    REQUIRE(bind_error("SELECT * FROM users u JOIN orders o ON u.id = o.user_id") == "no error");
}

TEST_CASE("bind: an unqualified name that only one table has is fine") {
    Planned p = plan_sql("SELECT name, amount FROM users u JOIN orders o ON u.id = user_id");
    REQUIRE(print(*p.bound.items[0].expr) == "#1");
    REQUIRE(print(*p.bound.items[1].expr) == "#5");
}

TEST_CASE("plan: the rest of the query sits above the joins") {
    REQUIRE(plan_text("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 100 "
                      "ORDER BY u.name LIMIT 5") ==
            "Limit[5]\n"
            "  Project[name]\n"
            "    Sort[name ASC]\n"
            "      Filter[amount > 100]\n"
            "        Join[INNER ON u.id = user_id]\n"
            "          Scan[users AS u]\n"
            "          Scan[orders AS o]");
}

TEST_CASE("plan: a single table is planned as before") {
    REQUIRE(plan_text("SELECT name FROM users") == "Project[name]\n  Scan[users]");
}

TEST_CASE("rules reach into both sides of a join and its condition") {
    assert_rewrite(std::make_unique<ConstantFolding>(),
                   "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id AND 1 + 1 = 2",
                   "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id AND TRUE");
    assert_rewrite(std::make_unique<BooleanCleanup>(),
                   "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id AND TRUE",
                   "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    assert_unchanged(std::make_unique<ConstantFolding>(),
                     "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 100");
}

TEST_CASE("rules see every step of a three-table join") {
    int scans_seen = 0;
    struct Counter : Rule {
        explicit Counter(int& n) : n_(n) {}
        std::string name() const override { return "counter"; }
        std::optional<PlanPtr> apply(const PlanPtr& node) const override {
            if (std::holds_alternative<LogicalScan>(node->node)) ++n_;
            return std::nullopt;
        }
        int& n_;
    };
    Planned p = plan_sql("SELECT u.name FROM users u, orders o, items i");
    optimizer_of(std::make_unique<Counter>(scans_seen)).optimize(p.plan);
    REQUIRE(scans_seen == 3);
}

TEST_CASE("explain: a join") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT)");
    db.execute("INSERT INTO users VALUES (1, 'ama'), (2, 'kofi'), (3, 'esi')");
    db.execute("INSERT INTO orders VALUES (10, 1), (11, 1), (12, 3), (13, 3), (14, 2), (15, 1)");
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    REQUIRE(format_result(db.execute("EXPLAIN SELECT name FROM users u JOIN orders o ON u.id = o.user_id AND 1 = 1")) ==
            "Original plan\n"
            "  Project[name]  est_rows=1\n"
            "    Join[INNER ON u.id = user_id AND 1 = 1]  est_rows=1\n"
            "      Scan[users AS u]  est_rows=3\n"
            "      Scan[orders AS o]  est_rows=6\n"
            "\n"
            "Rules fired\n"
            "  1. constant-folding (pass 1)\n"
            "  2. boolean-cleanup (pass 1)\n"
            "  3. column-pruning (pass 2)\n"
            "\n"
            "Final plan\n"
            "  Project[name]  est_rows=6\n"
            "    Join[INNER ON u.id = user_id]  est_rows=6\n"
            "      Scan[users AS u]  est_rows=3\n"
            "      Prune[user_id]  est_rows=6\n"
            "        Scan[orders AS o]  est_rows=6\n"
            "\n"
            "Statistics\n"
            "  users: analyzed when it had 3 rows\n"
            "  orders: analyzed when it had 6 rows");
}
