#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "cost/cost_model.h"

using namespace cardinal;
using Catch::Approx;

// Every number below was worked out by hand from the formulas in docs/cost-model.md, with the
// default settings: page_cost 1, per_row_cost 0.01, per_check_cost 0.01, 100 rows a page.

TEST_CASE("cost: adding and comparing") {
    REQUIRE((Cost{2} + Cost{3}).total == 5);
    Cost c{1};
    c += Cost{4};
    REQUIRE(c.total == 5);
    REQUIRE(Cost{1} < Cost{2});
    REQUIRE(Cost{2} == Cost{2});
    REQUIRE(Cost{}.total == 0);
}

TEST_CASE("cost: the default settings") {
    CostParams p;
    REQUIRE(p.page_cost == 1.0);      // PostgreSQL's
    REQUIRE(p.per_row_cost == 0.01);  // PostgreSQL's
    // PostgreSQL uses 0.0025. Measured on this engine a pair check costs about what a row does,
    // so this was fitted to 0.01 (bench/analyze_calibration.py, docs/cost-model.md).
    REQUIRE(p.per_check_cost == 0.01);
    REQUIRE(p.rows_per_page == 100);
}

TEST_CASE("scan = pages x page_cost + rows x per_row_cost") {
    DefaultCostModel m;
    REQUIRE(m.scan(1000).total == Approx(10 * 1.0 + 1000 * 0.01));  // 10 pages: 10 + 10 = 20
    REQUIRE(m.scan(1000).total == Approx(20));
    REQUIRE(m.scan(250).total == Approx(3 + 2.5));    // 2.5 pages round up to 3
    REQUIRE(m.scan(1).total == Approx(1 + 0.01));     // one row still reads a page
    REQUIRE(m.scan(100).total == Approx(1 + 1));      // exactly one full page
    REQUIRE(m.scan(0).total == 0);
    REQUIRE(m.scan(-5).total == 0);
}

TEST_CASE("filter = input rows x per_check_cost x number of conditions") {
    DefaultCostModel m;
    REQUIRE(m.filter(1000, 1).total == Approx(10));
    REQUIRE(m.filter(1000, 2).total == Approx(20));
    REQUIRE(m.filter(1000, 4).total == Approx(40));
    REQUIRE(m.filter(1000, 0).total == Approx(10));  // a filter always checks at least once
    REQUIRE(m.filter(0, 3).total == 0);
}

TEST_CASE("project = rows x per_row_cost") {
    DefaultCostModel m;
    REQUIRE(m.project(500).total == Approx(5));
    REQUIRE(m.project(0).total == 0);
}

TEST_CASE("sort = rows x log2(rows) x per_check_cost") {
    DefaultCostModel m;
    REQUIRE(m.sort(1024).total == Approx(1024 * 10 * 0.01));  // 102.4
    REQUIRE(m.sort(1024).total == Approx(102.4));
    REQUIRE(m.sort(2).total == Approx(2 * 1 * 0.01));
    REQUIRE(m.sort(8).total == Approx(8 * 3 * 0.01));
    REQUIRE(m.sort(1).total == 0);  // nothing to put in order
    REQUIRE(m.sort(0).total == 0);
    REQUIRE(m.sort(0.5).total == 0);
}

TEST_CASE("nested loop join = every pair checked + output rows x per_row_cost") {
    DefaultCostModel m;
    // 10 x 20 = 200 pairs x 0.01 = 2, plus 5 output rows x 0.01 = 0.05
    REQUIRE(m.nested_loop_join(10, 20, 5).total == Approx(2.05));
    REQUIRE(m.nested_loop_join(1000, 4000, 4000).total == Approx(4000000 * 0.01 + 4000 * 0.01));  // 40040
    REQUIRE(m.nested_loop_join(0, 20, 0).total == 0);
    REQUIRE(m.nested_loop_join(10, 0, 0).total == 0);
}

TEST_CASE("hash join = build rows x (row + check) + probe rows x check + output rows x per_row_cost") {
    DefaultCostModel m;
    // build 10 x 0.02 = 0.2, probe 20 x 0.01 = 0.2, output 5 x 0.01 = 0.05
    REQUIRE(m.hash_join(10, 20, 5).total == Approx(0.45));
    REQUIRE(m.hash_join(1000, 4000, 4000).total == Approx(1000 * 0.02 + 4000 * 0.01 + 4000 * 0.01));  // 100
    REQUIRE(m.hash_join(0, 0, 0).total == 0);
}

TEST_CASE("the join methods cross over: tiny inputs favour the nested loop, big ones the hash") {
    DefaultCostModel m;
    // one row against three: 3 pairs checked, against building a table first
    REQUIRE(m.nested_loop_join(1, 3, 1).total < m.hash_join(1, 3, 1).total);
    // the demo query: 1000 users against 4000 orders
    REQUIRE(m.hash_join(1000, 4000, 4000).total < m.nested_loop_join(1000, 4000, 4000).total / 100);
    // somewhere between, the cheaper one changes
    int flips = 0;
    bool nested_cheaper = true;
    for (int n = 1; n <= 200; ++n) {
        bool now = m.nested_loop_join(n, 3 * n, n).total < m.hash_join(n, 3 * n, n).total;
        if (now != nested_cheaper) ++flips;
        nested_cheaper = now;
    }
    REQUIRE(flips == 1);
}

TEST_CASE("cost grows with rows, never shrinks") {
    DefaultCostModel m;
    for (double rows = 1; rows < 100000; rows *= 3) {
        REQUIRE(m.scan(rows).total <= m.scan(rows * 3).total);
        REQUIRE(m.sort(rows).total <= m.sort(rows * 3).total);
        REQUIRE(m.filter(rows, 2).total <= m.filter(rows * 3, 2).total);
        REQUIRE(m.nested_loop_join(rows, 10, 1).total <= m.nested_loop_join(rows * 3, 10, 1).total);
        REQUIRE(m.hash_join(rows, 10, 1).total <= m.hash_join(rows * 3, 10, 1).total);
    }
}

TEST_CASE("the settings can be changed") {
    CostParams p;
    p.page_cost = 4.0;
    p.per_row_cost = 0.02;
    p.per_check_cost = 0.005;
    p.rows_per_page = 50;
    DefaultCostModel m(p);
    REQUIRE(m.scan(100).total == Approx(2 * 4.0 + 100 * 0.02));
    REQUIRE(m.filter(100, 2).total == Approx(100 * 0.005 * 2));
    REQUIRE(m.sort(8).total == Approx(8 * 3 * 0.005));
    REQUIRE(m.params().page_cost == 4.0);
    // and a different model still is the same default shape
    REQUIRE(m.scan(100).total > DefaultCostModel().scan(100).total);
}

TEST_CASE("a model can be swapped through the interface") {
    struct Flat : CostModel {
        Cost scan(double) const override { return {1}; }
        Cost filter(double, int) const override { return {1}; }
        Cost project(double) const override { return {1}; }
        Cost sort(double) const override { return {1}; }
        Cost nested_loop_join(double, double, double) const override { return {5}; }
        Cost hash_join(double, double, double) const override { return {3}; }
    };
    std::unique_ptr<CostModel> model = std::make_unique<Flat>();
    REQUIRE(model->hash_join(1e9, 1e9, 1e9) < model->nested_loop_join(1, 1, 1));
    model = std::make_unique<DefaultCostModel>();
    REQUIRE(model->nested_loop_join(1, 1, 1) < model->hash_join(1, 1, 1));
}
