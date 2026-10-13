#include "cost/join_order.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <numeric>
#include <random>

#include "cost/cost_planner.h"
#include "engine/answer_checker.h"
#include "engine/database.h"
#include "engine/fuzz.h"
#include "logical/plan_util.h"
#include "physical/join_keys.h"
#include "logical/planner.h"
#include "sql/parser.h"

using namespace cardinal;
using Catch::Approx;

namespace {

// Tables t0, t1, ... each (id INT, k INT), analyzed. Column k takes `distinct` values.
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
    JoinOrder left_deep;
    JoinOrder bushy;
    Cost every_left_deep;  // the cheapest, trying every order
    Cost every_bushy;      // the cheapest, trying every tree
};

// Every tree the bushy search could build for `group`: each split with no cross join that
// could have waited.
std::vector<PlanPtr> every_tree(const JoinGraph& graph, std::uint32_t group) {
    if (std::popcount(group) == 1) return {relation_with_filters(graph, static_cast<std::size_t>(std::countr_zero(group)))};
    std::vector<PlanPtr> out;
    for (std::uint32_t left = (group - 1) & group; left != 0; left = (left - 1) & group) {
        std::uint32_t right = group ^ left;
        if (cross_join_can_wait(graph, left, right)) continue;
        for (const PlanPtr& l : every_tree(graph, left))
            for (const PlanPtr& r : every_tree(graph, right)) out.push_back(join_onto(graph, l, left, r, right));
    }
    return out;
}

Searched search(Database& db, const std::string& sql) {
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
    std::optional<JoinGraph> graph = extract_join_graph(input_of(*plan_select(bound)));
    REQUIRE(graph);
    CardinalityEstimator estimator(bound.scope, db.catalog());
    DefaultCostModel model;
    Searched out{*graph, choose_join_order(*graph, estimator, model), choose_join_order(*graph, estimator, model, true), {}, {}};

    std::vector<std::size_t> order(graph->relations.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    bool any = false;
    do {
        bool allowed = true;
        std::uint32_t joined = std::uint32_t{1} << order[0];
        for (std::size_t step = 1; step < order.size() && allowed; ++step) {
            allowed = !cross_join_can_wait(*graph, joined, std::uint32_t{1} << order[step]);
            joined |= std::uint32_t{1} << order[step];
        }
        if (!allowed) continue;
        Cost cost = plan_by_cost(build_join_tree(*graph, order), estimator, model).cost;
        if (!any || cost < out.every_left_deep) out.every_left_deep = cost;
        any = true;
    } while (std::next_permutation(order.begin(), order.end()));
    REQUIRE(any);

    any = false;
    for (const PlanPtr& tree : every_tree(*graph, (1u << graph->relations.size()) - 1)) {
        Cost cost = plan_by_cost(with_constant_filters(*graph, tree), estimator, model).cost;
        if (!any || cost < out.every_bushy) out.every_bushy = cost;
        any = true;
    }
    REQUIRE(any);
    return out;
}

// t0 and t3 are tiny and each picks out a few rows of its neighbour by id. t1 and t2 join on
// k, which has two values, so any join of t1 or t2 alone onto the other side is big.
Database bushy_wins(int middle = 2000) { return tables_of({5, middle, middle, 5}, {5, 2, 2, 5}); }
constexpr const char* kBushyWins = "SELECT t0.id, t3.id FROM t0, t1, t2, t3 WHERE t0.id = t1.id AND t1.k = t2.k AND t2.id = t3.id";

}  // namespace

TEST_CASE("join order: three tables in a line, by hand") {
    Database db = tables_of({10, 5000, 5000}, {10, 1000, 1000});
    Searched s = search(db, "SELECT * FROM t2 JOIN t1 ON t2.k = t1.k JOIN t0 ON t1.k = t0.k");

    // Four pairs, then one table onto each connected pair. t2 + t0 is never tried.
    REQUIRE(s.left_deep.joins_priced == 6);
    REQUIRE(s.left_deep.search == JoinSearch::LeftDeep);
    REQUIRE(s.left_deep.order.back() == 0);
    REQUIRE(s.left_deep.cost.total == Approx(s.every_left_deep.total));
}

TEST_CASE("join order: the chosen plan is the one build_join_tree gives for the order") {
    Database db = tables_of({10, 5000, 5000}, {10, 1000, 1000});
    Searched s = search(db, "SELECT * FROM t2 JOIN t1 ON t2.k = t1.k JOIN t0 ON t1.k = t0.k WHERE t2.id > 3 AND 1 = 1");
    REQUIRE(plan_equal(s.left_deep.plan, build_join_tree(s.graph, s.left_deep.order)));
}

TEST_CASE("join order: a table nothing joins to may come in at any point") {
    Database db = tables_of({100, 100, 3}, {50, 50, 3});
    Searched s = search(db, "SELECT * FROM t0, t1, t2 WHERE t0.k = t1.k");
    REQUIRE(s.left_deep.cost.total == Approx(s.every_left_deep.total));
    REQUIRE(s.bushy.cost.total == Approx(s.every_bushy.total));
}

TEST_CASE("join order: matches trying everything, five tables") {
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
            REQUIRE(s.left_deep.cost.total == Approx(s.every_left_deep.total));
            REQUIRE(s.bushy.cost.total == Approx(s.every_bushy.total));
            // Every left-deep tree is also a bushy one.
            REQUIRE(s.bushy.cost.total <= Approx(s.left_deep.cost.total));
        }
    }
}

TEST_CASE("join order: bushy beats left-deep when both sides shrink before they meet") {
    Database db = bushy_wins();
    Searched s = search(db, kBushyWins);
    REQUIRE(s.bushy.search == JoinSearch::Bushy);
    REQUIRE(s.bushy.order.empty());
    REQUIRE(s.bushy.cost.total < s.left_deep.cost.total);

    // The top join has a join on each side.
    const auto& top = std::get<LogicalJoin>(s.bushy.plan->node);
    REQUIRE(std::holds_alternative<LogicalJoin>(top.left->node));
    REQUIRE(std::holds_alternative<LogicalJoin>(top.right->node));
}

TEST_CASE("join order: bushy plans give the same answers") {
    Database db = bushy_wins(40);
    db.set_bushy_joins(true);
    REQUIRE(db.execute(std::string("EXPLAIN VERBOSE ") + kBushyWins).message.find("* bushy") != std::string::npos);
    CheckOutcome check = check_query(db, kBushyWins);
    INFO(check.detail);
    REQUIRE(check.ok);

    Database fuzzed;
    create_fuzz_tables(fuzzed, 3);
    fuzzed.set_bushy_joins(true);
    std::optional<FuzzFailure> failure = fuzz(fuzzed, 3, 300).failure;
    if (failure) INFO(to_lines(failure->shrunk));
    REQUIRE_FALSE(failure);
}

TEST_CASE("join order: EXPLAIN VERBOSE shows both costs") {
    Database db = bushy_wins();
    std::string left_deep = db.execute(std::string("EXPLAIN VERBOSE ") + kBushyWins).message;
    INFO(left_deep);
    REQUIRE(left_deep.find("Join order\n  4 tables, searched left-deep") != std::string::npos);
    REQUIRE(left_deep.find("* left-deep  cost") != std::string::npos);
    REQUIRE(left_deep.find("  bushy      cost") != std::string::npos);

    db.set_bushy_joins(true);
    std::string bushy = db.execute(std::string("EXPLAIN VERBOSE ") + kBushyWins).message;
    INFO(bushy);
    REQUIRE(bushy.find("searched bushy") != std::string::npos);
    REQUIRE(bushy.find("* bushy      cost") != std::string::npos);
}

TEST_CASE("join order: too many tables, greedy") {
    const std::size_t n = kMaxSearchedRelations + 1;
    Database db = tables_of(std::vector<int>(n, 2), std::vector<int>(n, 2));
    std::string sql = "SELECT t0.id FROM t0", where;
    for (std::size_t t = 1; t < n; ++t) {
        sql += ", t" + std::to_string(t);
        where += (t == 1 ? " WHERE " : " AND ") + ("t" + std::to_string(t - 1)) + ".k = t" + std::to_string(t) + ".k";
    }
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql + where).node), db.catalog());
    JoinGraph graph = *extract_join_graph(input_of(*plan_select(bound)));
    CardinalityEstimator estimator(bound.scope, db.catalog());
    JoinOrder greedy = choose_join_order(graph, estimator, DefaultCostModel());
    REQUIRE(greedy.search == JoinSearch::Greedy);
    std::vector<std::size_t> sorted = greedy.order;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t t = 0; t < n; ++t) REQUIRE(sorted[t] == t);
    REQUIRE(plan_equal(greedy.plan, build_join_tree(graph, greedy.order)));

    CheckOutcome check = check_query(db, sql + where);
    INFO(check.detail);
    REQUIRE(check.ok);
}

TEST_CASE("join order: a query written in a bad order is reordered, with the same answer") {
    Database db = tables_of({5, 300, 300}, {5, 60, 60});
    const std::string sql = "SELECT t0.id, t2.id FROM t2 JOIN t1 ON t2.k = t1.k JOIN t0 ON t1.k = t0.k";
    std::string explain = db.execute("EXPLAIN " + sql).message;
    INFO(explain);
    REQUIRE(explain.find("join-order") != std::string::npos);

    CheckOutcome check = check_query(db, sql);
    INFO(check.detail);
    REQUIRE(check.ok);

    QueryResult reordered = db.execute(sql);
    db.set_optimizer_enabled(false);
    REQUIRE(reordered.stats.rows_processed < db.execute(sql).stats.rows_processed);
}

TEST_CASE("join order: hash_joinable agrees with split_join_condition") {
    Database db = tables_of({4, 4, 4, 4}, {2, 2, 2, 2});
    for (const char* sql : {
             "SELECT * FROM t0, t1, t2, t3 WHERE t0.k = t1.k AND t1.id = t2.id AND t2.k = t3.k AND t3.id = t0.id",
             "SELECT * FROM t0, t1, t2, t3 WHERE t0.id + t1.id = t2.k AND t3.k = 1 + t2.id AND t0.k = t0.id",
             "SELECT * FROM t0, t1, t2, t3 WHERE t0.k = t1.k AND t2.k < t3.k AND t1.id = t3.id + t2.id",
         }) {
        INFO(sql);
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        JoinGraph graph = *extract_join_graph(input_of(*plan_select(bound)));
        auto any_tree = [&](std::uint32_t group) {
            PlanPtr tree;
            std::uint32_t joined = 0;
            for (std::size_t t = 0; t < graph.relations.size(); ++t) {
                if (!(group >> t & 1)) continue;
                PlanPtr table = relation_with_filters(graph, t);
                tree = tree ? join_onto(graph, tree, joined, table, 1u << t) : table;
                joined |= 1u << t;
            }
            return tree;
        };
        const std::uint32_t all = (1u << graph.relations.size()) - 1;
        for (std::uint32_t group = 1; group <= all; ++group) {
            for (std::uint32_t left = (group - 1) & group; left != 0; left = (left - 1) & group) {
                std::uint32_t right = group ^ left;
                PlanPtr joined = join_onto(graph, any_tree(left), left, any_tree(right), right);
                const auto& join = std::get<LogicalJoin>(joined->node);
                bool split = !split_join_condition(join.condition, output_columns(*join.left), output_columns(*join.right)).keys.empty();
                INFO("left " << left << ", right " << right);
                REQUIRE(hash_joinable(graph, left, right) == split);
            }
        }
    }
}
