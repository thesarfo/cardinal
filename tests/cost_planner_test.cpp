#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "cost/cost_planner.h"
#include "logical/plan_util.h"
#include "engine/datagen.h"
#include "logical/planner.h"
#include "physical/physical_printer.h"
#include "sql/parser.h"

using namespace cardinal;
using Catch::Approx;

namespace {

struct Planned {
    BoundSelect bound;
    PlanPtr logical;
};

Planned logical_plan(Database& db, const std::string& sql) {
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
    PlanPtr plan = plan_select(bound);
    return {std::move(bound), std::move(plan)};
}

// users(id, name, age) and orders(id, user_id, amount) with the given numbers of rows, analyzed.
Database sized(int users, int orders) {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    std::vector<Row> u, o;
    for (int n = 1; n <= users; ++n) u.push_back({Value(std::int64_t{n}), Value(std::string("u")), Value(std::int64_t{18 + n % 50})});
    for (int n = 1; n <= orders; ++n)
        o.push_back({Value(std::int64_t{n}), Value(std::int64_t{1 + (n - 1) % std::max(1, users)}), Value(double(n % 100))});
    db.table("users")->insert_rows(std::move(u));
    db.table("orders")->insert_rows(std::move(o));
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    return db;
}

CostBasedPlan plan(Database& db, const std::string& sql, const CostModel& model = DefaultCostModel()) {
    Planned p = logical_plan(db, sql);
    CardinalityEstimator estimator(p.bound.scope, db.catalog());
    return plan_by_cost(p.logical, estimator, model);
}

std::string text(const CostBasedPlan& p) { return print(*p.physical); }

constexpr const char* kJoin = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id";

const StepOptions& join_options(const CostBasedPlan& p) {
    for (const StepOptions& step : p.trace)
        for (const PlanOption& option : step.options)
            if (option.name.find("Join") != std::string::npos) return step;
    FAIL("no join in the trace");
    return p.trace.front();
}

const PlanOption& chosen(const StepOptions& step) {
    return *std::find_if(step.options.begin(), step.options.end(), [](const PlanOption& o) { return o.chosen; });
}

}  // namespace

TEST_CASE("planner: tiny inputs get a nested loop join, big ones a hash join") {
    Database tiny = sized(3, 5);
    REQUIRE(text(plan(tiny, kJoin)).find("NestedLoopJoin") != std::string::npos);

    Database big = sized(1000, 4000);
    REQUIRE(text(plan(big, kJoin)).find("HashJoin") != std::string::npos);
}

TEST_CASE("planner: changing the table sizes switches the chosen join") {
    // grow both tables together and watch the choice flip, once
    std::vector<std::string> methods;
    for (int n : {1, 2, 4, 8, 16, 32, 64, 128, 256}) {
        Database db = sized(n, 3 * n);
        methods.push_back(text(plan(db, kJoin)).find("HashJoin") != std::string::npos ? "hash" : "nested");
    }
    REQUIRE(methods.front() == "nested");
    REQUIRE(methods.back() == "hash");
    int flips = 0;
    for (std::size_t i = 1; i < methods.size(); ++i) flips += methods[i] != methods[i - 1];
    REQUIRE(flips == 1);
}

TEST_CASE("planner: the table that goes into the hash table is the smaller one") {
    Database db = sized(1000, 4000);
    REQUIRE(text(plan(db, kJoin)).find("build left") != std::string::npos);  // users, 1000 rows
    REQUIRE(text(plan(db, "SELECT u.name FROM orders o JOIN users u ON u.id = o.user_id")).find("build right") != std::string::npos);
}

TEST_CASE("planner: a condition that is not an equality stays a nested loop, however big") {
    Database db = sized(1000, 4000);
    CostBasedPlan p = plan(db, "SELECT u.name FROM users u JOIN orders o ON u.id < o.user_id");
    REQUIRE(text(p).find("NestedLoopJoin") != std::string::npos);
    const StepOptions& step = join_options(p);
    REQUIRE(step.options.size() == 1);
    REQUIRE(step.options[0].note.find("no equality") != std::string::npos);

    REQUIRE(text(plan(db, "SELECT u.name FROM users u, orders o")).find("NestedLoopJoin[CROSS]") != std::string::npos);
}

TEST_CASE("planner: an equality with other conditions hashes on the equality and checks the rest") {
    Database db = sized(1000, 4000);
    std::string t = text(plan(db, "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id AND o.amount > u.age"));
    REQUIRE(t.find("HashJoin") != std::string::npos);
    REQUIRE(t.find(", then ") != std::string::npos);
}

TEST_CASE("planner: every option considered is in the trace, with exactly one winner per step") {
    Database db = sized(1000, 4000);
    CostBasedPlan p = plan(db, "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 50 ORDER BY u.name LIMIT 5");
    REQUIRE(p.trace.size() >= 7);
    for (const StepOptions& step : p.trace) {
        REQUIRE_FALSE(step.options.empty());
        REQUIRE(std::count_if(step.options.begin(), step.options.end(), [](const PlanOption& o) { return o.chosen; }) == 1);
    }

    const StepOptions& join = join_options(p);
    REQUIRE(join.options.size() == 3);
    REQUIRE(join.options[0].name == "NestedLoopJoin");
    REQUIRE(join.options[1].name == "HashJoin (build right)");
    REQUIRE(join.options[2].name == "HashJoin (build left)");
    // the winner is the cheapest of them
    for (const PlanOption& o : join.options) REQUIRE(chosen(join).cost <= o.cost);
}

TEST_CASE("planner: the trace lists the inputs of a step before the step") {
    Database db = sized(100, 400);
    CostBasedPlan p = plan(db, kJoin);
    const StepOptions& join = join_options(p);
    std::size_t join_at = static_cast<std::size_t>(&join - p.trace.data());
    // two scans and a project... the scans come first, then the join, then the project on top
    REQUIRE(join_at == 2);
    REQUIRE(p.trace.back().options[0].name == "Project");
    REQUIRE(p.trace.front().options[0].name.rfind("SeqScan", 0) == 0);
}

TEST_CASE("planner: costs are the model's formulas applied to the estimator's row counts") {
    Database db = sized(1000, 4000);
    DefaultCostModel model;

    // no join: scan, then project
    CostBasedPlan simple = plan(db, "SELECT id FROM users", model);
    REQUIRE(simple.cost.total == Approx(model.scan(1000).total + model.project(1000).total));  // 20 + 10

    // with a filter
    Planned f = logical_plan(db, "SELECT id FROM users WHERE age > 50");
    CardinalityEstimator est(f.bound.scope, db.catalog());
    CostBasedPlan filtered = plan_by_cost(f.logical, est, model);
    double kept = est.rows(input_of(*f.logical));  // rows out of the filter
    REQUIRE(filtered.cost.total == Approx(model.scan(1000).total + model.filter(1000, 1).total + model.project(kept).total));

    // the join: both scans, the winner's own cost, and the project on top
    CostBasedPlan join = plan(db, kJoin, model);
    double out = 4000;  // 1000 x 4000 / 1000
    Cost scans = model.scan(1000) + model.scan(4000);
    Cost hash = model.hash_join(1000, 4000, out);  // build the 1000 users
    REQUIRE(join.cost.total == Approx((scans + hash + model.project(out)).total));
}

TEST_CASE("planner: a limit is not discounted, and a sort is priced on its input") {
    Database db = sized(1000, 4000);
    DefaultCostModel model;
    CostBasedPlan p = plan(db, "SELECT id FROM users ORDER BY age LIMIT 3", model);
    REQUIRE(p.cost.total == Approx(model.scan(1000).total + model.sort(1000).total + model.project(1000).total));
}

TEST_CASE("planner: it follows whatever cost model it is given") {
    struct AlwaysNestedLoop : DefaultCostModel {
        Cost hash_join(double, double, double) const override { return {1e18}; }
    };
    struct AlwaysHash : DefaultCostModel {
        Cost nested_loop_join(double, double, double) const override { return {1e18}; }
    };
    Database tiny = sized(3, 5);
    Database big = sized(1000, 4000);
    REQUIRE(text(plan(big, kJoin, AlwaysNestedLoop())).find("NestedLoopJoin") != std::string::npos);
    REQUIRE(text(plan(tiny, kJoin, AlwaysHash())).find("HashJoin") != std::string::npos);

    // different settings move the crossover: make checks very cheap and building very dear
    CostParams dear_build;
    dear_build.per_row_cost = 5.0;
    Database mid = sized(40, 120);
    REQUIRE(text(plan(mid, kJoin, DefaultCostModel())).find("HashJoin") != std::string::npos);
    REQUIRE(text(plan(mid, kJoin, DefaultCostModel(dear_build))).find("NestedLoopJoin") != std::string::npos);
}

TEST_CASE("planner: a tie goes to the nested loop, then hash building from the right") {
    struct Flat : CostModel {
        Cost scan(double) const override { return {1}; }
        Cost filter(double, int) const override { return {1}; }
        Cost project(double) const override { return {1}; }
        Cost sort(double) const override { return {1}; }
        Cost nested_loop_join(double, double, double) const override { return {7}; }
        Cost hash_join(double, double, double) const override { return {7}; }
    };
    Database db = sized(10, 10);
    REQUIRE(text(plan(db, kJoin, Flat())).find("NestedLoopJoin") != std::string::npos);
}

TEST_CASE("planner: works on tables that were never analyzed") {
    Database db;
    db.execute("CREATE TABLE a (x INT)");
    db.execute("CREATE TABLE b (y INT)");
    std::vector<Row> rows;
    for (int n = 0; n < 1000; ++n) rows.push_back({Value(std::int64_t{n})});
    db.table("a")->insert_rows(rows);
    db.table("b")->insert_rows(rows);
    REQUIRE(text(plan(db, "SELECT a.x FROM a JOIN b ON a.x = b.y")).find("HashJoin") != std::string::npos);
}

TEST_CASE("engine: joins are chosen by cost by default, and it shows in the work done") {
    Database db;
    generate_users_orders(db, {.users = 400});
    db.execute("ANALYZE users");
    db.execute("ANALYZE orders");
    const char* sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 500";

    QueryResult chosen_by_cost = db.execute(sql);
    db.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    QueryResult nested = db.execute(sql);
    db.use_cost_based_planner();

    REQUIRE(chosen_by_cost.rows.size() == nested.rows.size());
    REQUIRE(chosen_by_cost.stats.plan_hash != nested.stats.plan_hash);
    // the filter keeps about half the 1600 orders: 400 users x about 800 orders is over 300,000 pairs
    REQUIRE(nested.stats.rows_processed > 300000);
    REQUIRE(chosen_by_cost.stats.rows_processed < nested.stats.rows_processed / 20);
}

TEST_CASE("engine: the settings can force a join method and go back") {
    Database db = sized(300, 900);
    REQUIRE_FALSE(db.forced_planner().has_value());
    db.set_planner_options(PlannerOptions{JoinMethod::Hash, true});
    REQUIRE(db.forced_planner().has_value());
    db.use_cost_based_planner();
    REQUIRE_FALSE(db.forced_planner().has_value());
}
