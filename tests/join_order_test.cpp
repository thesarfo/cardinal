#include "cost/join_order.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <random>

#include "cost/cost_planner.h"
#include "engine/database.h"
#include "logical/plan_util.h"
#include "logical/planner.h"
#include "sql/parser.h"

using namespace cardinal;
using Catch::Approx;

namespace {

// Tables t0, t1, ... each (id INT, k INT), with the given numbers of rows, analyzed. Column
// k takes `distinct` values, so joins on k grow or shrink depending on the sizes.
Database tables_of(const std::vector<int>& sizes, const std::vector<int>& distinct) {
    Database db;
    for (std::size_t t = 0; t < sizes.size(); ++t) {
        std::string name = "t" + std::to_string(t);
        db.execute("CREATE TABLE " + name + " (id INT, k INT)");
        std::vector<Row> rows;
        for (int n = 0; n < sizes[t]; ++n) rows.push_back({Value(std::int64_t{n}), Value(std::int64_t{n % distinct[t]})});
        db.table(name)->insert_rows(std::move(rows));
        db.execute("ANALYZE " + name);
    }
    return db;
}

struct Searched {
    JoinGraph graph;
    std::optional<JoinOrder> chosen;
    Cost brute_force;  // the cheapest order, trying every one
};

// Runs the search on the joins of `sql`, and also prices every order the search is allowed
// to pick: every permutation with no cross join that could have waited.
Searched search(Database& db, const std::string& sql) {
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
    PlanPtr plan = plan_select(bound);
    std::optional<JoinGraph> graph = extract_join_graph(input_of(*plan));
    REQUIRE(graph);
    CardinalityEstimator estimator(bound.scope, db.catalog());
    DefaultCostModel model;

    Searched out{*graph, choose_join_order(*graph, estimator, model), {}};
    std::vector<std::size_t> order(graph->relations.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    bool any = false;
    do {
        bool allowed = true;
        std::uint32_t joined = std::uint32_t{1} << order[0];
        for (std::size_t step = 1; step < order.size() && allowed; ++step) {
            allowed = !cross_join_can_wait(*graph, joined, order[step]);
            joined |= std::uint32_t{1} << order[step];
        }
        if (!allowed) continue;
        Cost cost = plan_by_cost(build_join_tree(*graph, order), estimator, model).cost;
        if (!any || cost < out.brute_force) out.brute_force = cost;
        any = true;
    } while (std::next_permutation(order.begin(), order.end()));
    REQUIRE(any);
    return out;
}

}  // namespace

TEST_CASE("join order: three tables in a line, by hand") {
    // t0 is tiny, t1 and t2 are big. t0 - t1 - t2: t0 and t2 share nothing.
    Database db = tables_of({10, 5000, 5000}, {10, 1000, 1000});
    Searched s = search(db, "SELECT * FROM t2 JOIN t1 ON t2.k = t1.k JOIN t0 ON t1.k = t0.k");
    REQUIRE(s.chosen);

    // Written as t2, t1, t0, which are tables 0, 1 and 2 of the graph. Pairs priced: t2 + t1,
    // t1 + t2, t1 + t0, t0 + t1, then the third table onto each of the two connected pairs.
    // t2 + t0 is never tried, because t1 joins to either of them.
    REQUIRE(s.chosen->joins_priced == 6);

    // Joining the tiny table first keeps the middle result small, so t2 comes last.
    REQUIRE(s.chosen->order.back() == 0);
    REQUIRE(s.chosen->cost.total == Approx(s.brute_force.total));
}

TEST_CASE("join order: the chosen plan is the one build_join_tree gives for the order") {
    Database db = tables_of({10, 5000, 5000}, {10, 1000, 1000});
    Searched s = search(db, "SELECT * FROM t2 JOIN t1 ON t2.k = t1.k JOIN t0 ON t1.k = t0.k WHERE t2.id > 3 AND 1 = 1");
    REQUIRE(s.chosen);
    REQUIRE(plan_equal(s.chosen->plan, build_join_tree(s.graph, s.chosen->order)));
}

TEST_CASE("join order: two groups with nothing between them are joined by a cross join, last") {
    Database db = tables_of({100, 100, 3}, {50, 50, 3});
    Searched s = search(db, "SELECT * FROM t0, t1, t2 WHERE t0.k = t1.k");
    REQUIRE(s.chosen);
    REQUIRE(s.chosen->cost.total == Approx(s.brute_force.total));

    // t0 and t1 are joined before t2 comes in, whichever side t2 lands on.
    const std::vector<std::size_t>& order = s.chosen->order;
    REQUIRE((order.front() == 2 || order.back() == 2));
}

TEST_CASE("join order: matches trying every order, five tables") {
    std::mt19937 random(7);
    const std::vector<std::string> queries = {
        // a line
        "SELECT * FROM t0 JOIN t1 ON t0.k = t1.k JOIN t2 ON t1.k = t2.k JOIN t3 ON t2.k = t3.k JOIN t4 ON t3.k = t4.k",
        // a star around t2
        "SELECT * FROM t0, t1, t2, t3, t4 WHERE t2.k = t0.k AND t2.k = t1.k AND t2.id = t3.id AND t2.id = t4.k",
        // a cycle, a condition on three tables, and a filter
        "SELECT * FROM t0, t1, t2, t3, t4 WHERE t0.k = t1.k AND t1.id = t2.id AND t2.k = t3.k AND t3.id = t4.id "
        "AND t4.k = t0.k AND t0.id + t1.id > t2.id AND t3.id < 50",
        // two groups with nothing between them
        "SELECT * FROM t0, t1, t2, t3, t4 WHERE t0.k = t1.k AND t1.k = t2.k AND t3.k = t4.k",
    };
    for (int trial = 0; trial < 6; ++trial) {
        std::vector<int> sizes, distinct;
        for (int t = 0; t < 5; ++t) {
            sizes.push_back(std::uniform_int_distribution<int>(1, 400)(random));
            distinct.push_back(std::uniform_int_distribution<int>(1, sizes.back())(random));
        }
        Database db = tables_of(sizes, distinct);
        for (const std::string& sql : queries) {
            INFO("trial " << trial << ": " << sql);
            Searched s = search(db, sql);
            REQUIRE(s.chosen);
            REQUIRE(s.chosen->cost.total == Approx(s.brute_force.total));
        }
    }
}

TEST_CASE("join order: too many tables, no search") {
    Database db = tables_of(std::vector<int>(kMaxSearchedRelations + 1, 2), std::vector<int>(kMaxSearchedRelations + 1, 2));
    std::string sql = "SELECT * FROM t0";
    for (std::size_t t = 1; t <= kMaxSearchedRelations; ++t) sql += ", t" + std::to_string(t);
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
    std::optional<JoinGraph> graph = extract_join_graph(input_of(*plan_select(bound)));
    REQUIRE(graph);
    CardinalityEstimator estimator(bound.scope, db.catalog());
    REQUIRE_FALSE(choose_join_order(*graph, estimator, DefaultCostModel()));
}
