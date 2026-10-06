#include "optimizer/join_graph.h"

#include <algorithm>
#include <numeric>

#include "broken_rules.h"
#include "optimizer/default_rules.h"
#include "engine/fuzz.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

JoinGraph graph_of(const std::string& sql) {
    Planned p = plan_sql(sql);
    // The join sits under the Project.
    std::optional<JoinGraph> graph = extract_join_graph(input_of(*p.plan));
    REQUIRE(graph);
    return *graph;
}

std::vector<PredicateKind> kinds(const JoinGraph& g) {
    std::vector<PredicateKind> out;
    for (const GraphPredicate& p : g.predicates) out.push_back(p.kind);
    return out;
}

// The alias of the leftmost scan: a name for a table that survives a rebuild.
std::string name_of(const PlanPtr& plan) {
    if (const auto* scan = std::get_if<LogicalScan>(&plan->node)) return scan->alias + "/" + scan->table;
    return name_of(input_of(*plan));
}

// Joins the tables in a fixed order that depends only on which tables there are, so the
// rewrite settles instead of reshuffling forever.
class ReordersJoins : public Rule {
public:
    explicit ReordersJoins(bool backwards) : backwards_(backwards) {}
    std::string name() const override { return "reorders-joins"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        std::optional<JoinGraph> graph = extract_join_graph(node);
        if (!graph) return std::nullopt;
        std::vector<std::size_t> order(graph->relations.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            std::string x = name_of(graph->relations[a].plan), y = name_of(graph->relations[b].plan);
            return backwards_ ? x > y : x < y;
        });
        PlanPtr rebuilt = build_join_tree(*graph, order);
        if (plan_equal(rebuilt, node)) return std::nullopt;
        return rebuilt;
    }

private:
    bool backwards_;
};

// Takes the joins apart and puts them back in the order they came.
class RoundTripsJoins : public Rule {
public:
    std::string name() const override { return "round-trips-joins"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        std::optional<JoinGraph> graph = extract_join_graph(node);
        if (!graph) return std::nullopt;
        PlanPtr rebuilt = build_join_tree(*graph);
        if (plan_equal(rebuilt, node)) return std::nullopt;
        return rebuilt;
    }
};

std::optional<FuzzFailure> run_with(std::function<std::unique_ptr<Rule>()> rule, uint64_t seed, int count) {
    Database db;
    create_fuzz_tables(db, seed);
    with_extra_rule(db, std::move(rule));
    return fuzz(db, seed, count).failure;
}

}  // namespace

TEST_CASE("plan_equal: same shape is equal, any difference is not") {
    PlanPtr a = plan_sql("SELECT u.name FROM users u WHERE u.age > 3 ORDER BY u.id LIMIT 2").plan;
    PlanPtr b = plan_sql("SELECT u.name FROM users u WHERE u.age > 3 ORDER BY u.id LIMIT 2").plan;
    REQUIRE(a != b);
    REQUIRE(plan_equal(a, b));
    REQUIRE(plan_equal(a, a));
    REQUIRE_FALSE(plan_equal(a, plan_sql("SELECT u.name FROM users u WHERE u.age > 4 ORDER BY u.id LIMIT 2").plan));
    REQUIRE_FALSE(plan_equal(a, plan_sql("SELECT u.name FROM users u WHERE u.age > 3 ORDER BY u.id LIMIT 3").plan));
    REQUIRE_FALSE(plan_equal(a, plan_sql("SELECT u.name FROM users u WHERE u.age > 3 ORDER BY u.id DESC LIMIT 2").plan));
    REQUIRE_FALSE(plan_equal(a, plan_sql("SELECT u.name FROM users u WHERE u.age > 3").plan));
    REQUIRE_FALSE(plan_equal(a, nullptr));
}

TEST_CASE("join graph: tables, equalities and single-table filters are separated") {
    JoinGraph g = graph_of(
        "SELECT * FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id "
        "WHERE u.age > 30 AND o.amount > i.id");
    REQUIRE(g.relations.size() == 3);
    REQUIRE(kinds(g) == std::vector<PredicateKind>{PredicateKind::Edge, PredicateKind::Edge, PredicateKind::SingleTable,
                                                   PredicateKind::Other});
    // bit i is table i: users, orders, items
    REQUIRE(g.predicates[0].tables == 0b011);
    REQUIRE(g.predicates[1].tables == 0b110);
    REQUIRE(g.predicates[2].tables == 0b001);
    REQUIRE(g.predicates[3].tables == 0b110);
}

TEST_CASE("join graph: a comma join and its WHERE give the same graph as JOIN ... ON") {
    JoinGraph a = graph_of("SELECT * FROM users u, orders o WHERE u.id = o.user_id");
    JoinGraph b = graph_of("SELECT * FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE(kinds(a) == kinds(b));
    REQUIRE(a.relations.size() == 2);
}

TEST_CASE("join graph: an equality needs one table on each side to be an edge") {
    REQUIRE(kinds(graph_of("SELECT * FROM users u, orders o WHERE u.id + o.id = 5")) == std::vector{PredicateKind::Other});
    REQUIRE(kinds(graph_of("SELECT * FROM users u, orders o WHERE u.id = u.age")) == std::vector{PredicateKind::SingleTable});
    REQUIRE(kinds(graph_of("SELECT * FROM users u, orders o WHERE u.id + 1 = o.user_id * 2")) == std::vector{PredicateKind::Edge});
    REQUIRE(kinds(graph_of("SELECT * FROM users u, orders o WHERE 1 = 1")) == std::vector{PredicateKind::Other});
}

TEST_CASE("join graph: a table that was filtered or pruned is still one table") {
    Planned p = plan_sql("SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.age > 3 AND o.amount > 5");
    PlanPtr optimized = p.plan;
    for (std::vector<std::unique_ptr<Rule>>& stage : default_stages())
        optimized = RuleOptimizer(std::move(stage)).optimize(optimized).plan;
    std::optional<JoinGraph> g = extract_join_graph(input_of(*optimized));
    REQUIRE(g);
    REQUIRE(g->relations.size() == 2);
    REQUIRE(kinds(*g) == std::vector{PredicateKind::Edge});
}

TEST_CASE("join graph: not a join, no graph") {
    REQUIRE_FALSE(extract_join_graph(plan_sql("SELECT * FROM users u").plan));
    REQUIRE_FALSE(extract_join_graph(plan_sql("SELECT * FROM users u WHERE u.age > 3").plan));
}

TEST_CASE("join graph: rebuilding in the original order gives the original plan") {
    for (const char* sql : {
             "SELECT * FROM users u JOIN orders o ON u.id = o.user_id",
             "SELECT * FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id",
             "SELECT * FROM users a JOIN users b ON a.id = b.id AND a.age > b.age JOIN users c ON c.id = b.id",
         }) {
        INFO(sql);
        Planned p = plan_sql(sql);
        std::optional<JoinGraph> g = extract_join_graph(input_of(*p.plan));
        REQUIRE(g);
        REQUIRE(plan_equal(build_join_tree(*g), input_of(*p.plan)));
    }
}

TEST_CASE("join graph: another order moves each condition to the first join that has its tables") {
    Planned p = plan_sql("SELECT * FROM users u JOIN orders o ON u.id = o.user_id JOIN items i ON o.id = i.order_id WHERE u.age > 30");
    JoinGraph g = *extract_join_graph(input_of(*p.plan));

    // items, orders, users
    PlanPtr tree = build_join_tree(g, {2, 1, 0});
    REQUIRE(print(*tree, p.bound.scope) ==
            "Join[INNER ON u.id = user_id]\n"
            "  Join[INNER ON o.id = order_id]\n"
            "    Scan[items AS i]\n"
            "    Scan[orders AS o]\n"
            "  Filter[age > 30]\n"
            "    Scan[users AS u]");

    // users, items: nothing joins them yet, so it is a cross join, and both conditions wait for orders
    PlanPtr crossed = build_join_tree(g, {0, 2, 1});
    REQUIRE(print(*crossed, p.bound.scope) ==
            "Join[INNER ON u.id = user_id AND o.id = order_id]\n"
            "  Join[CROSS]\n"
            "    Filter[age > 30]\n"
            "      Scan[users AS u]\n"
            "    Scan[items AS i]\n"
            "  Scan[orders AS o]");
}

TEST_CASE("join graph: an order that is not a permutation is refused") {
    JoinGraph g = graph_of("SELECT * FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE_THROWS_AS(build_join_tree(g, {0}), std::invalid_argument);
    REQUIRE_THROWS_AS(build_join_tree(g, {0, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(build_join_tree(g, {0, 2}), std::invalid_argument);
}

TEST_CASE("join graph: taking joins apart and putting them back never changes an answer") {
    for (uint64_t seed : {1}) {
        std::optional<FuzzFailure> failure = run_with([] { return std::make_unique<RoundTripsJoins>(); }, seed, 600);
        if (failure) INFO(to_lines(failure->shrunk));
        REQUIRE_FALSE(failure);
    }
}

TEST_CASE("join graph: any order of the tables gives the same answer") {
    for (bool backwards : {false, true}) {
        for (uint64_t seed : {2}) {
            std::optional<FuzzFailure> failure =
                run_with([=] { return std::make_unique<ReordersJoins>(backwards); }, seed, 600);
            if (failure) INFO(to_lines(failure->shrunk));
            REQUIRE_FALSE(failure);
        }
    }
}
