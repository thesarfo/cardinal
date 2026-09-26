#include "optimizer/filter_cleanup.h"

#include "exec/empty.h"
#include "optimizer/boolean_cleanup.h"
#include "optimizer/constant_folding.h"
#include "physical/physical_printer.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

const LogicalFilter& filter_of(const PlanPtr& project) {
    return std::get<LogicalFilter>(std::get<LogicalProject>(project->node).input->node);
}

// Puts another Filter, with the condition from `where`, directly above the plan's Filter.
// This is the shape pushdown will make in M4; the planner never builds it alone.
PlanPtr stack_filter(const Planned& base, const std::string& where) {
    Planned other = plan_sql("SELECT a FROM t WHERE " + where);
    const auto& project = std::get<LogicalProject>(base.plan->node);
    PlanPtr stacked = std::make_shared<const LogicalPlan>(
        LogicalPlan{LogicalFilter{project.input, filter_of(other.plan).predicate}});
    return std::make_shared<const LogicalPlan>(LogicalPlan{LogicalProject{stacked, project.items}});
}

std::string run_text(const PlanPtr& logical, const Catalog& catalog) {
    std::unique_ptr<Operator> root = build_operator(*plan_physical(*logical), catalog);
    std::string out;
    while (auto row = root->next()) {
        for (const Value& v : *row) out += v.to_string() + " ";
        out += "\n";
    }
    return out;
}

}  // namespace

TEST_CASE("merge filters: two become one") {
    Planned p = plan_sql("SELECT a FROM t WHERE a > 1");
    OptimizeResult result = optimizer_of(std::make_unique<MergeFilters>()).optimize(stack_filter(p, "b < 5"));
    REQUIRE(print(*result.plan, p.bound.scope) ==
            "Project[a]\n"
            "  Filter[a > 1 AND b < 5]\n"
            "    Scan[t]");
    REQUIRE(result.trace.size() == 1);
}

TEST_CASE("merge filters: a tall stack folds all the way down") {
    Planned p = plan_sql("SELECT a FROM t WHERE a > 1");
    PlanPtr stacked = stack_filter(Planned{p.bound, stack_filter(p, "b < 5")}, "flag");
    OptimizeResult result = optimizer_of(std::make_unique<MergeFilters>()).optimize(stacked);
    REQUIRE(print(*result.plan, p.bound.scope) ==
            "Project[a]\n"
            "  Filter[a > 1 AND b < 5 AND flag]\n"
            "    Scan[t]");
    REQUIRE_FALSE(result.hit_pass_limit);
}

TEST_CASE("merge filters: a single filter, or filters with something between, are left alone") {
    assert_unchanged(std::make_unique<MergeFilters>(), "SELECT a FROM t WHERE a > 1");
    assert_unchanged(std::make_unique<MergeFilters>(), "SELECT a FROM t WHERE a > 1 ORDER BY a LIMIT 2");
    assert_unchanged(std::make_unique<MergeFilters>(), "SELECT a FROM t");
}

TEST_CASE("remove true filter") {
    assert_rewrite_to_text(std::make_unique<RemoveTrueFilter>(), "SELECT a FROM t WHERE TRUE",
                           "Project[a]\n  Scan[t]");
    assert_rewrite_to_text(std::make_unique<RemoveTrueFilter>(),
                           "SELECT a FROM t WHERE TRUE ORDER BY a LIMIT 2",
                           "Limit[2]\n  Project[a]\n    Sort[a ASC]\n      Scan[t]");
    assert_unchanged(std::make_unique<RemoveTrueFilter>(), "SELECT a FROM t WHERE a > 1");
    assert_unchanged(std::make_unique<RemoveTrueFilter>(), "SELECT a FROM t WHERE FALSE");
    assert_unchanged(std::make_unique<RemoveTrueFilter>(), "SELECT a FROM t WHERE NULL");
    assert_unchanged(std::make_unique<RemoveTrueFilter>(), "SELECT a FROM t WHERE flag");
}

TEST_CASE("empty for false filters") {
    assert_rewrite_to_text(std::make_unique<EmptyFalseFilter>(), "SELECT a FROM t WHERE FALSE",
                           "Project[a]\n  Empty");
    assert_rewrite_to_text(std::make_unique<EmptyFalseFilter>(),
                           "SELECT a, b FROM t WHERE FALSE ORDER BY a LIMIT 3",
                           "Limit[3]\n  Project[a, b]\n    Sort[a ASC]\n      Empty");
    // A NULL condition keeps no row either.
    assert_rewrite_to_text(std::make_unique<EmptyFalseFilter>(), "SELECT a FROM t WHERE NULL",
                           "Project[a]\n  Empty");
    assert_unchanged(std::make_unique<EmptyFalseFilter>(), "SELECT a FROM t WHERE TRUE");
    assert_unchanged(std::make_unique<EmptyFalseFilter>(), "SELECT a FROM t WHERE a > 1");
    assert_unchanged(std::make_unique<EmptyFalseFilter>(), "SELECT a FROM t");
}

TEST_CASE("empty keeps the columns of the step it replaced") {
    Planned p = plan_sql("SELECT b, a FROM t WHERE FALSE ORDER BY x");
    OptimizeResult result = optimizer_of(std::make_unique<EmptyFalseFilter>()).optimize(p.plan);

    PlanPtr empty = input_of(*input_of(*result.plan));
    REQUIRE(std::holds_alternative<LogicalEmpty>(empty->node));
    REQUIRE(std::get<LogicalEmpty>(empty->node).columns.size() == 5);

    // So the physical planner can still turn column ids into positions above it.
    REQUIRE(print(*plan_physical(*result.plan)) ==
            "Project[#1, #0]\n"
            "  Sort[#2 ASC]\n"
            "    Empty");
}

TEST_CASE("the empty step runs and gives nothing") {
    Empty empty;
    REQUIRE_FALSE(empty.next().has_value());
    REQUIRE(empty.stats().rows_out == 0);
}

TEST_CASE("each rule fires in the trace, on a hand-made query") {
    Planned p = plan_sql("SELECT a FROM t WHERE a > 1");
    PlanPtr plan = stack_filter(Planned{p.bound, stack_filter(p, "TRUE")}, "b < 5");
    // Project / Filter[b < 5] / Filter[TRUE] / Filter[a > 1] / Scan

    RuleOptimizer optimizer = optimizer_of_all<RemoveTrueFilter, MergeFilters, EmptyFalseFilter>();
    OptimizeResult result = optimizer.optimize(plan);
    REQUIRE(print(*result.plan, p.bound.scope) ==
            "Project[a]\n"
            "  Filter[a > 1 AND b < 5]\n"
            "    Scan[t]");
    REQUIRE(result.trace.size() == 2);
    REQUIRE(result.trace[0].rule == "remove-true-filter");
    REQUIRE(result.trace[1].rule == "merge-filters");

    Planned q = plan_sql("SELECT a FROM t WHERE a > 1 AND FALSE");
    OptimizeResult emptied = optimizer_of_all<BooleanCleanup, EmptyFalseFilter>().optimize(q.plan);
    REQUIRE(emptied.trace.size() == 2);
    REQUIRE(emptied.trace[0].rule == "boolean-cleanup");
    REQUIRE(emptied.trace[1].rule == "empty-false-filter");
    REQUIRE(format_trace(emptied.trace, q.bound.scope) ==
            "1. boolean-cleanup (pass 1)\n"
            "   before:\n"
            "     Project[a]\n"
            "       Filter[a > 1 AND FALSE]\n"
            "         Scan[t]\n"
            "   after:\n"
            "     Project[a]\n"
            "       Filter[FALSE]\n"
            "         Scan[t]\n"
            "\n"
            "2. empty-false-filter (pass 1)\n"
            "   before:\n"
            "     Project[a]\n"
            "       Filter[FALSE]\n"
            "         Scan[t]\n"
            "   after:\n"
            "     Project[a]\n"
            "       Empty");
}

TEST_CASE("all the rules together: WHERE 1 + 1 = 2 collapses to a plain scan") {
    RuleOptimizer optimizer =
        optimizer_of_all<ConstantFolding, BooleanCleanup, RemoveTrueFilter, MergeFilters, EmptyFalseFilter>();
    Planned p = plan_sql("SELECT a FROM t WHERE 1 + 1 = 2 AND TRUE");
    REQUIRE(print(*optimizer.optimize(p.plan).plan, p.bound.scope) == "Project[a]\n  Scan[t]");

    Planned q = plan_sql("SELECT a FROM t WHERE a > 1 AND 2 < 1");
    REQUIRE(print(*optimizer.optimize(q.plan).plan, q.bound.scope) == "Project[a]\n  Empty");
}

TEST_CASE("filter cleanup agrees with running the query") {
    Database db;
    db.execute("CREATE TABLE t (a INT, b INT, x DOUBLE, name TEXT, flag BOOL)");
    db.execute("INSERT INTO t VALUES (1, 5, 1.0, 'a', TRUE), (2, 3, 2.0, 'b', FALSE), (3, 9, 3.0, 'c', NULL), "
               "(NULL, 1, 4.0, 'd', TRUE)");
    const char* queries[] = {"SELECT a FROM t WHERE TRUE",
                             "SELECT a FROM t WHERE FALSE",
                             "SELECT a FROM t WHERE NULL",
                             "SELECT b, a FROM t WHERE FALSE ORDER BY x",
                             "SELECT a FROM t WHERE 1 = 1 AND flag",
                             "SELECT a FROM t WHERE a > 1 AND 1 > 2",
                             "SELECT a FROM t WHERE a > 1 OR TRUE ORDER BY b DESC LIMIT 2",
                             "SELECT a FROM t WHERE flag AND NULL"};
    RuleOptimizer optimizer =
        optimizer_of_all<ConstantFolding, BooleanCleanup, RemoveTrueFilter, MergeFilters, EmptyFalseFilter>();
    for (const char* sql : queries) {
        INFO(sql);
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        PlanPtr plan = plan_select(bound);
        REQUIRE(run_text(optimizer.optimize(plan).plan, db.catalog()) == run_text(plan, db.catalog()));
    }
}

TEST_CASE("stacked filters give the same rows once merged") {
    Database db;
    db.execute("CREATE TABLE t (a INT, b INT, x DOUBLE, name TEXT, flag BOOL)");
    db.execute("INSERT INTO t VALUES (1, 5, 1.0, 'a', TRUE), (2, 3, 2.0, 'b', FALSE), (3, 9, 3.0, 'c', NULL), "
               "(4, 1, 4.0, 'd', TRUE)");
    BoundSelect bound = bind_select(std::get<Select>(parse_statement("SELECT a FROM t WHERE a > 1").node), db.catalog());
    Planned base{bound, plan_select(bound)};
    PlanPtr stacked = stack_filter(base, "b < 9");
    PlanPtr merged = optimizer_of(std::make_unique<MergeFilters>()).optimize(stacked).plan;
    REQUIRE(run_text(merged, db.catalog()) == run_text(stacked, db.catalog()));
    REQUIRE(run_text(merged, db.catalog()) == "2 \n4 \n");
}
