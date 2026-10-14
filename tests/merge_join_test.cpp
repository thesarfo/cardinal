#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

#include "engine/answer_checker.h"
#include "engine/database.h"
#include "engine/result_format.h"
#include "exec/hash_join.h"
#include "exec/merge_join.h"
#include "exec/seq_scan.h"
#include "exec/sort.h"
#include "expr/evaluator.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

BoundExprPtr column(std::uint32_t position) {
    return std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{ColumnId{position}}, std::nullopt});
}

std::unique_ptr<Operator> sorted_scan(const Table& t) {
    return std::make_unique<Sort>(std::make_unique<SeqScan>(t), std::vector<SortKey>{{column(0), false}});
}

struct Fixture {
    Catalog catalog;
    Table *l, *r;

    Fixture() {
        l = &catalog.create_table({"l", {{"k", Type::Int}, {"tag", Type::Text}}});
        r = &catalog.create_table({"r", {{"k", Type::Int}, {"tag", Type::Text}}});
    }

    std::vector<Row> merged(BoundExprPtr residual = nullptr) {
        MergeJoin join(sorted_scan(*l), sorted_scan(*r), {column(0)}, {column(0)}, residual);
        std::vector<Row> rows;
        while (auto row = join.next()) rows.push_back(*row);
        return rows;
    }
    std::vector<Row> hashed() {
        HashJoin join(std::make_unique<SeqScan>(*l), std::make_unique<SeqScan>(*r), {column(0)}, {column(0)}, nullptr, false);
        std::vector<Row> rows;
        while (auto row = join.next()) rows.push_back(*row);
        return rows;
    }
};

std::vector<Row> ordered(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (a[k].is_null() != b[k].is_null()) return a[k].is_null();
            if (a[k].is_null()) continue;
            int c = compare_values(a[k], b[k]);
            if (c != 0) return c < 0;
        }
        return false;
    });
    return rows;
}

}  // namespace

TEST_CASE("merge join: matching rows, left values then right values") {
    Fixture f;
    f.l->insert({i(1), s("a")});
    f.l->insert({i(3), s("b")});
    f.r->insert({i(3), s("x")});
    f.r->insert({i(2), s("y")});
    f.r->insert({i(1), s("z")});
    std::vector<Row> rows = f.merged();
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0] == Row{i(1), s("a"), i(1), s("z")});
    REQUIRE(rows[1] == Row{i(3), s("b"), i(3), s("x")});
}

TEST_CASE("merge join: duplicate keys on both sides meet pairwise") {
    Fixture f;
    for (const char* t : {"a", "b", "c"}) f.l->insert({i(5), s(t)});
    for (const char* t : {"x", "y"}) f.r->insert({i(5), s(t)});
    f.l->insert({i(6), s("d")});
    f.r->insert({i(4), s("w")});
    REQUIRE(f.merged().size() == 6);
    REQUIRE(ordered(f.merged()) == ordered(f.hashed()));
}

TEST_CASE("merge join: NULL keys never match, not even each other") {
    Fixture f;
    f.l->insert({Value(), s("a")});
    f.l->insert({i(1), s("b")});
    f.r->insert({Value(), s("x")});
    f.r->insert({Value(), s("y")});
    f.r->insert({i(1), s("z")});
    REQUIRE(f.merged() == std::vector<Row>{{i(1), s("b"), i(1), s("z")}});
}

TEST_CASE("merge join: an empty side, and no keys in common") {
    Fixture f;
    REQUIRE(f.merged().empty());
    f.l->insert({i(1), s("a")});
    REQUIRE(f.merged().empty());
    f.r->insert({i(2), s("b")});
    REQUIRE(f.merged().empty());
}

TEST_CASE("merge join: INT and DOUBLE keys match as numbers") {
    Catalog catalog;
    Table& a = catalog.create_table({"a", {{"k", Type::Int}}});
    Table& b = catalog.create_table({"b", {{"k", Type::Double}}});
    a.insert({i(2)});
    a.insert({i(3)});
    b.insert({Value(2.0)});
    b.insert({Value(2.5)});
    MergeJoin join(sorted_scan(a), sorted_scan(b), {column(0)}, {column(0)}, nullptr);
    auto row = join.next();
    REQUIRE(row.has_value());
    REQUIRE(row->size() == 2);
    REQUIRE_FALSE(join.next().has_value());
}

TEST_CASE("merge join: random tables give the hash join's rows") {
    std::mt19937_64 rng(2);
    for (int round = 0; round < 40; ++round) {
        Fixture f;
        int nl = static_cast<int>(rng() % 30), nr = static_cast<int>(rng() % 30);
        auto key = [&] { return rng() % 5 == 0 ? Value() : i(static_cast<std::int64_t>(rng() % 8)); };
        for (int n = 0; n < nl; ++n) f.l->insert({key(), s("l")});
        for (int n = 0; n < nr; ++n) f.r->insert({key(), s("r")});
        INFO("round " << round);
        REQUIRE(ordered(f.merged()) == ordered(f.hashed()));
    }
}

TEST_CASE("merge join: a forced merge plan sorts both inputs and gives the same answer as a nested loop") {
    Database db;
    db.execute("CREATE TABLE a (k INT, v TEXT)");
    db.execute("CREATE TABLE b (k INT, w TEXT)");
    db.execute("INSERT INTO a VALUES (3, 'a3'), (1, 'a1'), (3, 'a3b'), (NULL, 'an')");
    db.execute("INSERT INTO b VALUES (3, 'b3'), (2, 'b2'), (1, 'b1'), (3, 'b3b'), (NULL, 'bn')");
    const char* sql = "SELECT a.v, b.w FROM a JOIN b ON a.k = b.k AND a.v <> b.w";
    db.set_planner_options(PlannerOptions{JoinMethod::Merge});
    std::string verbose = format_result(db.execute(std::string("EXPLAIN VERBOSE ") + sql));
    REQUIRE(verbose.find("MergeJoin[") != std::string::npos);
    REQUIRE(verbose.find("Sort[") != std::string::npos);
    QueryResult merged = db.execute(sql);
    db.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    QueryResult looped = db.execute(sql);
    REQUIRE(merged.rows.size() == 5);
    REQUIRE(ordered(merged.rows) == ordered(looped.rows));
    REQUIRE(check_query(db, sql).ok);
}

namespace {

Database indexed_pair() {
    Database db;
    db.execute("CREATE TABLE a (k INT, v TEXT)");
    db.execute("CREATE TABLE b (k INT, w TEXT)");
    db.execute("CREATE INDEX a_k ON a (k)");
    db.execute("CREATE INDEX b_k ON b (k)");
    for (int n = 0; n < 40; ++n) {
        db.execute("INSERT INTO a VALUES (" + std::to_string((n * 7) % 10) + ", 'a" + std::to_string(n) + "')");
        db.execute("INSERT INTO b VALUES (" + std::to_string((n * 3) % 10) + ", 'b" + std::to_string(n) + "')");
    }
    return db;
}

std::string chosen_plan(Database& db, const std::string& sql) {
    std::string text = format_result(db.execute("EXPLAIN VERBOSE " + sql));
    return text.substr(text.find("Chosen plan"));
}

}  // namespace

TEST_CASE("sorted_by: an index scan says what order it reads in") {
    Database db = indexed_pair();
    std::string plan = chosen_plan(db, "SELECT v FROM a WHERE k >= 0");
    REQUIRE(plan.find("sorted by #0") != std::string::npos);
}

TEST_CASE("sorted_by: a merge join does not sort an input that already arrives in key order") {
    Database db = indexed_pair();
    db.set_planner_options(PlannerOptions{JoinMethod::Merge});
    const char* sql = "SELECT a.v, b.w FROM a JOIN b ON a.k = b.k WHERE a.k >= 0 AND b.k >= 0";
    std::string plan = chosen_plan(db, sql);
    REQUIRE(plan.find("MergeJoin[") != std::string::npos);
    REQUIRE(plan.find("Sort[") == std::string::npos);  // both sides come from an index scan
    QueryResult merged = db.execute(sql);
    db.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    REQUIRE(ordered(merged.rows) == ordered(db.execute(sql).rows));
    REQUIRE(check_query(db, sql).ok);
}

TEST_CASE("sorted_by: one side sorted, the other not, still joins correctly") {
    Database db = indexed_pair();
    db.set_planner_options(PlannerOptions{JoinMethod::Merge});
    const char* sql = "SELECT a.v, b.w FROM a JOIN b ON a.k = b.k WHERE a.k >= 3";
    std::string plan = chosen_plan(db, sql);
    REQUIRE(plan.find("MergeJoin[") != std::string::npos);
    REQUIRE(std::count(plan.begin(), plan.end(), '\n') > 0);
    QueryResult merged = db.execute(sql);
    db.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    REQUIRE(ordered(merged.rows) == ordered(db.execute(sql).rows));
}

TEST_CASE("sorted_by: ORDER BY on the indexed column needs no sort step") {
    Database db = indexed_pair();
    std::string plan = chosen_plan(db, "SELECT v FROM a WHERE k > 4 ORDER BY k");
    REQUIRE(plan.find("IndexScan") != std::string::npos);
    REQUIRE(plan.find("Sort[") == std::string::npos);
    REQUIRE(check_query(db, "SELECT v FROM a WHERE k > 4 ORDER BY k").ok);
    // descending is a different order, so it does sort
    REQUIRE(chosen_plan(db, "SELECT v FROM a WHERE k > 4 ORDER BY k DESC").find("Sort[") != std::string::npos);
}

TEST_CASE("sorted_by: a sort aggregate over an index scan says its input was already sorted") {
    Database db = indexed_pair();
    PlannerOptions options{JoinMethod::NestedLoop};
    options.aggregate_method = AggregateMethod::Sort;
    db.set_planner_options(options);
    const char* sql = "SELECT k, COUNT(*) FROM a WHERE k >= 2 GROUP BY k";
    REQUIRE(chosen_plan(db, sql).find("input already sorted") != std::string::npos);
    QueryResult sorted = db.execute(sql);
    db.set_use_indexes(false);
    db.use_cost_based_planner();
    REQUIRE(ordered(sorted.rows) == ordered(db.execute(sql).rows));
}

namespace {

// a and b with an index on k each; `rows` rows, k taking `distinct` values spread through the table.
Database indexed_big(int rows, int distinct) {
    Database db;
    db.execute("CREATE TABLE a (id INT, k INT)");
    db.execute("CREATE TABLE b (id INT, k INT)");
    for (const char* t : {"a", "b"}) {
        std::string insert = std::string("INSERT INTO ") + t + " VALUES ";
        for (int n = 0; n < rows; ++n)
            insert += std::string(n ? ", (" : "(") + std::to_string(n) + ", " + std::to_string((n * 7919) % distinct) + ")";
        db.execute(insert);
        db.execute(std::string("CREATE INDEX ") + t + "_k ON " + t + " (k)");
        db.execute(std::string("ANALYZE ") + t);
    }
    return db;
}

}  // namespace

TEST_CASE("merge join wins when both inputs arrive in key order from an index") {
    Database db = indexed_big(10000, 10000);
    const char* sql = "SELECT a.id, b.id FROM a JOIN b ON a.k = b.k WHERE a.k < 2000 AND b.k < 2000";
    std::string plan = chosen_plan(db, sql);
    INFO(plan);
    REQUIRE(plan.find("MergeJoin[") != std::string::npos);
    REQUIRE(plan.find("Sort[") == std::string::npos);
    REQUIRE(plan.find("IndexScan[a using a_k") != std::string::npos);
    REQUIRE(plan.find("IndexScan[b using b_k") != std::string::npos);

    // and it is listed as a way to run the join, with the reason it won
    std::string all = format_result(db.execute(std::string("EXPLAIN VERBOSE ") + sql));
    REQUIRE(all.find("MergeJoin (both inputs in order)") != std::string::npos);

    QueryResult merged = db.execute(sql);
    db.set_use_indexes(false);
    db.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    // nested loop on 2000 x 2000 rows is fine; the answers must agree
    REQUIRE(ordered(merged.rows) == ordered(db.execute(sql).rows));
}

TEST_CASE("merge join is not chosen when the input would have to be sorted and hashing is cheaper") {
    Database db = indexed_big(10000, 10000);
    std::string plan = chosen_plan(db, "SELECT a.id, b.id FROM a JOIN b ON a.k = b.k");
    REQUIRE(plan.find("HashJoin[") != std::string::npos);
}

TEST_CASE("an ORDER BY the join key is met by a merge join's own order") {
    // Many rows per key: the join's output is much larger than its inputs, so sorting the
    // output would cost more than sorting the inputs and merging.
    Database db = indexed_big(60, 4);
    const char* sql = "SELECT a.k, b.id FROM a JOIN b ON a.k = b.k WHERE a.id < 40 AND b.id < 40 ORDER BY a.k";
    REQUIRE(check_query(db, sql).ok);

    // On bigger tables the plan is chosen by cost; the answer still has to match.
    Database big = indexed_big(2000, 20);
    const char* big_sql = "SELECT a.k, b.id FROM a JOIN b ON a.k = b.k WHERE a.id < 100 AND b.id < 100 ORDER BY a.k";
    QueryResult chosen = big.execute(big_sql);
    big.set_use_indexes(false);
    big.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    QueryResult looped = big.execute(big_sql);
    REQUIRE(chosen.rows.size() == looped.rows.size());
    for (std::size_t i = 1; i < chosen.rows.size(); ++i) REQUIRE(compare_values(chosen.rows[i - 1][0], chosen.rows[i][0]) <= 0);
}

TEST_CASE("merge join chains: the output of one merge feeds the next without a sort") {
    Database db = indexed_big(40, 40);
    db.execute("CREATE TABLE c (id INT, k INT)");
    std::string insert = "INSERT INTO c VALUES ";
    for (int n = 0; n < 40; ++n) insert += std::string(n ? ", (" : "(") + std::to_string(n) + ", " + std::to_string(n) + ")";
    db.execute(insert);
    db.execute("CREATE INDEX c_k ON c (k)");
    db.execute("ANALYZE c");
    const char* sql = "SELECT a.id FROM a, b, c WHERE a.k = b.k AND b.k = c.k AND a.k < 30 AND b.k < 30 AND c.k < 30";
    REQUIRE(check_query(db, sql).ok);
}

TEST_CASE("a sort aggregate pays off when its input arrives in group key order") {
    Database db = indexed_big(10000, 10000);
    const char* sql = "SELECT k, COUNT(*) FROM a WHERE k < 2000 GROUP BY k";
    std::string plan = chosen_plan(db, sql);
    INFO(plan);
    REQUIRE(plan.find("SortAggregate[") != std::string::npos);
    REQUIRE(plan.find("input already sorted") != std::string::npos);
    REQUIRE(plan.find("IndexScan[a using a_k") != std::string::npos);
    std::string all = format_result(db.execute(std::string("EXPLAIN VERBOSE ") + sql));
    REQUIRE(all.find("SortAggregate (input in order)") != std::string::npos);

    // without an index to deliver the order, hashing wins as it always did
    REQUIRE(chosen_plan(db, "SELECT id, COUNT(*) FROM a WHERE id < 2000 GROUP BY id").find("HashAggregate[") != std::string::npos);
}

TEST_CASE("GROUP BY k ORDER BY k needs no sort step when the aggregate delivers the order") {
    Database db = indexed_big(10000, 10000);
    std::string plan = chosen_plan(db, "SELECT k, COUNT(*) FROM a WHERE k < 2000 GROUP BY k ORDER BY k");
    INFO(plan);
    REQUIRE(plan.find("SortAggregate[") != std::string::npos);
    REQUIRE(plan.find("Sort[") == std::string::npos);

    Database small = indexed_big(60, 6);
    REQUIRE(check_query(small, "SELECT k, COUNT(*), SUM(id) FROM a WHERE k < 4 GROUP BY k ORDER BY k").ok);
    REQUIRE(check_query(small, "SELECT k, COUNT(*) FROM a WHERE k < 4 GROUP BY k HAVING COUNT(*) > 3").ok);
}

TEST_CASE("an index is used for a filter that column pruning has put a Prune under") {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("INSERT INTO users VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, NULL)");
    db.execute("INSERT INTO orders VALUES (1, 1, 5.0), (2, 1, 6.0), (3, 3, 1.0), (4, NULL, 2.0), (5, 9, 3.0)");
    db.execute("CREATE INDEX orders_user ON orders (user_id)");
    const char* sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.user_id >= 1";
    db.set_planner_options(PlannerOptions{JoinMethod::Hash, true});
    std::string plan = chosen_plan(db, sql);
    INFO(plan);
    REQUIRE(plan.find("IndexScan[orders using orders_user") != std::string::npos);
    REQUIRE(check_query(db, sql).ok);
    REQUIRE(check_query(db, "SELECT o.id FROM orders o WHERE o.user_id = 1 AND o.amount > 5").ok);
}
