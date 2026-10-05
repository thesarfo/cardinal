#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>

#include "engine/datagen.h"
#include "logical/plan_util.h"
#include "logical/planner.h"
#include "optimizer/default_rules.h"
#include "sql/parser.h"
#include "stats/cardinality.h"

using namespace cardinal;
using Catch::Approx;

namespace {

// 300 users, 1200 orders, every order has a user, spread evenly. (Small, because the checks
// below run the joins for real, and the nested loop join tests every pair.)
constexpr int kUsers = 300;
constexpr int kOrders = 4 * kUsers;

struct World {
    Database db;
    explicit World(bool analyze = true) {
        generate_users_orders(db, {.users = kUsers});
        if (analyze) {
            db.execute("ANALYZE users");
            db.execute("ANALYZE orders");
        }
    }

    struct Query {
        BoundSelect bound;
        PlanPtr plan;
    };

    Query plan(const std::string& sql, bool optimized = false) const {
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        PlanPtr logical = plan_select(bound);
        if (optimized) logical = RuleOptimizer::staged(default_stages()).optimize(logical).plan;
        return {std::move(bound), std::move(logical)};
    }

    double estimate(const std::string& sql, bool optimized = false) const {
        Query q = plan(sql, optimized);
        return CardinalityEstimator(q.bound.scope, db.catalog()).rows(q.plan);
    }

    // The query's real result size, with the optimizer off so nothing is rewritten.
    double actual(const std::string& sql) {
        db.set_optimizer_enabled(false);
        double n = static_cast<double>(db.execute(sql).rows.size());
        db.set_optimizer_enabled(true);
        return n;
    }
};

double q_error(double estimate, double actual) {
    estimate = std::max(1.0, estimate);
    actual = std::max(1.0, actual);
    return std::max(estimate / actual, actual / estimate);
}

void for_each_node(const PlanPtr& plan, const std::function<void(const PlanPtr&)>& visit) {
    visit(plan);
    for (const PlanPtr& child : children_of(*plan)) for_each_node(child, visit);
}

}  // namespace

TEST_CASE("cardinality: a scan is the table's row count") {
    World w;
    REQUIRE(w.estimate("SELECT id FROM users") == kUsers);
    REQUIRE(w.estimate("SELECT id FROM orders") == kOrders);
}

TEST_CASE("cardinality: a scan uses the rows now, even if the table has grown since ANALYZE") {
    World w;
    w.db.execute("INSERT INTO users VALUES (5000, 'new', 'Ghana', 30)");
    REQUIRE(w.estimate("SELECT id FROM users") == kUsers + 1);
}

TEST_CASE("cardinality: a filter keeps its selectivity of the rows coming in") {
    World w;
    double guess = w.estimate("SELECT id FROM users WHERE age > 50");
    double truth = w.actual("SELECT id FROM users WHERE age > 50");
    REQUIRE(std::fabs(guess - truth) / kUsers < 0.05);
    REQUIRE(w.estimate("SELECT id FROM users WHERE id = 7") == Approx(1));
    REQUIRE(w.estimate("SELECT id FROM users WHERE FALSE") == 0);
    REQUIRE(w.estimate("SELECT id FROM users WHERE TRUE") == kUsers);
}

TEST_CASE("cardinality: an equality join is left x right / the larger distinct count") {
    World w;
    // users.id has 300 distinct values; orders.user_id has no more, since some users may have no orders
    const ColumnStats& user_id = w.db.table("orders")->stats()->columns[1];
    REQUIRE(user_id.distinct <= kUsers);
    double guess = w.estimate("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE(guess == Approx(1.0 * kOrders));  // users x orders / the larger distinct count
    REQUIRE(guess == Approx(w.actual("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id")));
}

TEST_CASE("cardinality: the same join written with a comma is estimated the same") {
    World w;
    double with_on = w.estimate("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    double with_comma = w.estimate("SELECT u.name FROM users u, orders o WHERE u.id = o.user_id");
    REQUIRE(with_comma == Approx(with_on));
    // ...and after the optimizer has turned one into the other
    REQUIRE(w.estimate("SELECT u.name FROM users u, orders o WHERE u.id = o.user_id", true) == Approx(with_on));
}

TEST_CASE("cardinality: a cross join is left x right") {
    World w;
    REQUIRE(w.estimate("SELECT u.id FROM users u, orders o") == static_cast<double>(kUsers) * kOrders);
}

TEST_CASE("cardinality: a table joined to itself on its key") {
    World w;
    double guess = w.estimate("SELECT a.id FROM users a JOIN users b ON a.id = b.id");
    REQUIRE(guess == Approx(kUsers));
    REQUIRE(guess == Approx(w.actual("SELECT a.id FROM users a JOIN users b ON a.id = b.id")));
}

TEST_CASE("cardinality: filters on either side of a join, with and without pushdown") {
    World w;
    const std::string sql =
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.country = 'Ghana' AND o.amount > 900";
    double truth = w.actual(sql);
    double before = w.estimate(sql);
    double after = w.estimate(sql, true);
    INFO("actual " << truth << ", estimate " << before << " before, " << after << " after");
    REQUIRE(q_error(before, truth) < 2.0);
    // The rows coming out should not depend on where the filters sit.
    REQUIRE(after == Approx(before).epsilon(0.1));
}

TEST_CASE("cardinality: limit caps the rows, but never raises them") {
    World w;
    REQUIRE(w.estimate("SELECT id FROM users LIMIT 10") == 10);
    REQUIRE(w.estimate("SELECT id FROM users LIMIT 5000") == kUsers);
    REQUIRE(w.estimate("SELECT id FROM users WHERE id = 7 LIMIT 10") == Approx(1));
    REQUIRE(w.estimate("SELECT id FROM users LIMIT 0") == 0);
}

TEST_CASE("cardinality: project, sort and prune pass the count through") {
    World w;
    REQUIRE(w.estimate("SELECT id FROM users ORDER BY age") == kUsers);
    REQUIRE(w.estimate("SELECT name FROM users u JOIN orders o ON u.id = o.user_id", true) == Approx(kOrders));
}

TEST_CASE("cardinality: a step the optimizer emptied has no rows") {
    World w;
    REQUIRE(w.estimate("SELECT id FROM users WHERE 1 > 2", true) == 0);
    REQUIRE(w.estimate("SELECT u.id FROM users u JOIN orders o ON u.id = o.user_id WHERE 1 > 2", true) == 0);
}

TEST_CASE("cardinality: every step of the plan gets an estimate") {
    World w;
    for (bool optimized : {false, true}) {
        World::Query q = w.plan(
            "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id "
            "WHERE u.country = 'Ghana' AND o.amount > 900 ORDER BY u.name LIMIT 5",
            optimized);
        CardinalityEstimator estimator(q.bound.scope, w.db.catalog());
        estimator.rows(q.plan);
        int nodes = 0;
        for_each_node(q.plan, [&](const PlanPtr& node) {
            ++nodes;
            REQUIRE(estimator.has(node));
            REQUIRE(estimator.rows(node) >= 0);
        });
        REQUIRE(nodes >= 6);
    }
}

TEST_CASE("cardinality: answers are saved, so asking again gives the same number") {
    World w;
    World::Query q = w.plan("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 500");
    CardinalityEstimator estimator(q.bound.scope, w.db.catalog());
    PlanPtr filter = input_of(*q.plan);
    double first = estimator.rows(q.plan);
    REQUIRE(estimator.has(filter));
    REQUIRE(estimator.rows(filter) == estimator.rows(filter));
    REQUIRE(estimator.rows(q.plan) == first);
}

TEST_CASE("cardinality: without ANALYZE the table sizes are exact and the rest is guessed") {
    World w(/*analyze=*/false);
    REQUIRE(w.estimate("SELECT id FROM users") == kUsers);
    REQUIRE(w.estimate("SELECT id FROM users WHERE age > 50") == Approx(kUsers * kDefaultRange));
    REQUIRE(w.estimate("SELECT id FROM users WHERE id = 7") == Approx(kUsers * kDefaultEquality));
    // 200 distinct values assumed on both sides
    REQUIRE(w.estimate("SELECT u.id FROM users u JOIN orders o ON u.id = o.user_id") ==
            Approx(1.0 * kUsers * kOrders / kDefaultDistinct));
}

TEST_CASE("cardinality: a column with statistics against one without") {
    World w(/*analyze=*/false);
    w.db.execute("ANALYZE users");  // orders left unanalyzed
    // users.id: 300 distinct; orders.user_id: assumed 200; the larger is 300
    REQUIRE(w.estimate("SELECT u.id FROM users u JOIN orders o ON u.id = o.user_id") == Approx(kOrders));
}
