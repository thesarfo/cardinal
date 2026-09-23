#include "optimizer/boolean_cleanup.h"

#include "optimizer/constant_folding.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

void cleans(const std::string& before, const std::string& after) {
    assert_rewrite(std::make_unique<BooleanCleanup>(), before, after);
}

void leaves_alone(const std::string& sql) { assert_unchanged(std::make_unique<BooleanCleanup>(), sql); }

BoundExprPtr column(std::uint32_t id) {
    return std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{ColumnId{id}}, Type::Bool});
}

BoundExprPtr not_of(BoundExprPtr e) {
    return std::make_shared<const BoundExpr>(BoundExpr{BoundUnary{UnaryOp::Not, std::move(e)}, Type::Bool});
}

}  // namespace

TEST_CASE("cleanup: TRUE and FALSE that change nothing") {
    cleans("SELECT a FROM t WHERE flag AND TRUE", "SELECT a FROM t WHERE flag");
    cleans("SELECT a FROM t WHERE TRUE AND flag", "SELECT a FROM t WHERE flag");
    cleans("SELECT a FROM t WHERE flag OR FALSE", "SELECT a FROM t WHERE flag");
    cleans("SELECT a FROM t WHERE FALSE OR flag", "SELECT a FROM t WHERE flag");
    cleans("SELECT a FROM t WHERE a > 1 AND TRUE", "SELECT a FROM t WHERE a > 1");
    cleans("SELECT a FROM t WHERE flag AND TRUE AND a > 1 AND TRUE", "SELECT a FROM t WHERE flag AND a > 1");
}

TEST_CASE("cleanup: TRUE and FALSE that decide everything") {
    cleans("SELECT a FROM t WHERE a > 1 AND FALSE", "SELECT a FROM t WHERE FALSE");
    cleans("SELECT a FROM t WHERE FALSE AND a > 1", "SELECT a FROM t WHERE FALSE");
    cleans("SELECT a FROM t WHERE a > 1 OR TRUE", "SELECT a FROM t WHERE TRUE");
    cleans("SELECT a FROM t WHERE TRUE OR a > 1", "SELECT a FROM t WHERE TRUE");
    cleans("SELECT a FROM t WHERE flag AND a > 1 AND FALSE", "SELECT a FROM t WHERE FALSE");
    cleans("SELECT a FROM t WHERE flag OR b = 2 OR TRUE", "SELECT a FROM t WHERE TRUE");
}

TEST_CASE("cleanup: all TRUE, all FALSE") {
    cleans("SELECT a FROM t WHERE TRUE AND TRUE", "SELECT a FROM t WHERE TRUE");
    cleans("SELECT a FROM t WHERE FALSE OR FALSE", "SELECT a FROM t WHERE FALSE");
}

TEST_CASE("cleanup: repeated conditions collapse") {
    cleans("SELECT a FROM t WHERE a = 5 AND a = 5", "SELECT a FROM t WHERE a = 5");
    cleans("SELECT a FROM t WHERE a = 5 OR a = 5", "SELECT a FROM t WHERE a = 5");
    cleans("SELECT a FROM t WHERE flag AND flag", "SELECT a FROM t WHERE flag");
    cleans("SELECT a FROM t WHERE a = 5 AND b = 1 AND a = 5", "SELECT a FROM t WHERE a = 5 AND b = 1");
    cleans("SELECT a FROM t WHERE a = 5 AND (b = 1 AND a = 5)", "SELECT a FROM t WHERE a = 5 AND b = 1");
    cleans("SELECT a FROM t WHERE a IN (1, 1, 2)", "SELECT a FROM t WHERE a = 1 OR a = 2");
    cleans("SELECT a FROM t WHERE a IN (3, 3)", "SELECT a FROM t WHERE a = 3");
    leaves_alone("SELECT a FROM t WHERE a = 5 AND a = 6");
    leaves_alone("SELECT a FROM t WHERE a = 5 AND b = 5");
    leaves_alone("SELECT a FROM t WHERE a = 1 AND 1 = a");
}

TEST_CASE("cleanup: works at every level") {
    cleans("SELECT a FROM t WHERE (flag AND TRUE) OR (a > 1 OR FALSE)", "SELECT a FROM t WHERE flag OR a > 1");
    cleans("SELECT a FROM t WHERE NOT (flag AND TRUE)", "SELECT a FROM t WHERE NOT flag");
    cleans("SELECT flag AND TRUE FROM t", "SELECT flag FROM t");
    cleans("SELECT a FROM t ORDER BY flag OR FALSE", "SELECT a FROM t ORDER BY flag");
    cleans("SELECT a FROM t WHERE (a > 1 AND TRUE) IS NULL", "SELECT a FROM t WHERE a > 1 IS NULL");
}

TEST_CASE("cleanup: a tree with nothing to tidy keeps its shape") {
    leaves_alone("SELECT a FROM t WHERE flag AND (a > 1 AND a < 9)");
    leaves_alone("SELECT a FROM t WHERE (flag OR a > 1) AND b = 2");
    leaves_alone("SELECT a FROM t WHERE a BETWEEN 1 AND 5");
}

TEST_CASE("cleanup: NULL breaks these, so they must not happen") {
    leaves_alone("SELECT a FROM t WHERE a = a");
    leaves_alone("SELECT a FROM t WHERE a <> a");
    leaves_alone("SELECT a FROM t WHERE flag AND NOT flag");
    leaves_alone("SELECT a FROM t WHERE flag OR NOT flag");
    leaves_alone("SELECT a FROM t WHERE flag AND NULL");
    leaves_alone("SELECT a FROM t WHERE flag OR NULL");
}

TEST_CASE("cleanup: a lone NULL keeps a boolean type") {
    Planned p = plan_sql("SELECT NULL AND TRUE FROM t");
    OptimizeResult result = optimizer_of(std::make_unique<BooleanCleanup>()).optimize(p.plan);
    const auto& project = std::get<LogicalProject>(result.plan->node);
    REQUIRE(print(*project.items[0].expr) == "null");
    REQUIRE(project.items[0].expr->type == Type::Bool);
}

TEST_CASE("cleanup: NOT NOT x") {
    BoundExprPtr x = column(0);
    BoundExprPtr cleaned = clean_booleans(not_of(not_of(x)));
    REQUIRE(cleaned == x);
    REQUIRE(clean_booleans(not_of(x)) != x);
    REQUIRE(print(*clean_booleans(not_of(not_of(not_of(x))))) == "(not #0)");
}

TEST_CASE("cleanup: expr_equal") {
    Planned a = plan_sql("SELECT a FROM t WHERE a + 1 > b AND flag");
    Planned b = plan_sql("SELECT a FROM t WHERE a + 1 > b AND flag");
    const auto& fa = std::get<LogicalFilter>(std::get<LogicalProject>(a.plan->node).input->node);
    const auto& fb = std::get<LogicalFilter>(std::get<LogicalProject>(b.plan->node).input->node);
    REQUIRE(fa.predicate != fb.predicate);
    REQUIRE(expr_equal(*fa.predicate, *fb.predicate));

    Planned c = plan_sql("SELECT a FROM t WHERE a + 2 > b AND flag");
    const auto& fc = std::get<LogicalFilter>(std::get<LogicalProject>(c.plan->node).input->node);
    REQUIRE_FALSE(expr_equal(*fa.predicate, *fc.predicate));

    Planned i = plan_sql("SELECT a FROM t WHERE a = 1");
    Planned d = plan_sql("SELECT a FROM t WHERE x = 1.0");
    const auto& fi = std::get<LogicalFilter>(std::get<LogicalProject>(i.plan->node).input->node);
    const auto& fd = std::get<LogicalFilter>(std::get<LogicalProject>(d.plan->node).input->node);
    REQUIRE_FALSE(expr_equal(*fi.predicate, *fd.predicate));
}

TEST_CASE("cleanup after folding: the two rules work together") {
    RuleOptimizer optimizer = optimizer_of_all<ConstantFolding, BooleanCleanup>();
    Planned p = plan_sql("SELECT a FROM t WHERE 1 = 1 AND flag AND 2 > 3 OR a > 4 + 1");
    OptimizeResult result = optimizer.optimize(p.plan);
    REQUIRE(print(*result.plan, p.bound.scope) == plan_text("SELECT a FROM t WHERE a > 5"));
    REQUIRE_FALSE(result.hit_pass_limit);

    Planned q = plan_sql("SELECT a FROM t WHERE a > 1 + 1 AND NULL OR TRUE");
    OptimizeResult folded = optimizer.optimize(q.plan);
    REQUIRE(print(*folded.plan, q.bound.scope) == plan_text("SELECT a FROM t WHERE TRUE"));
}

TEST_CASE("cleanup: agrees with running the query, NULLs included") {
    Database db;
    db.execute("CREATE TABLE n (v INT, f BOOL)");
    db.execute("INSERT INTO n VALUES (1, TRUE), (2, FALSE), (3, NULL), (NULL, TRUE), (NULL, NULL), (5, FALSE)");
    const char* conditions[] = {"f AND TRUE",
                                "f OR FALSE",
                                "f AND FALSE",
                                "f OR TRUE",
                                "f AND f",
                                "f OR f",
                                "v > 1 AND v > 1",
                                "v > 1 AND f AND v > 1",
                                "NOT f AND TRUE",
                                "f AND NOT f",
                                "f OR NOT f",
                                "v = v",
                                "(v > 1 AND TRUE) OR (f AND FALSE)",
                                "v IN (1, 1, 5)",
                                "NOT (v > 1 AND TRUE)",
                                "f AND NULL",
                                "f OR NULL"};
    for (const char* cond : conditions) {
        INFO(cond);
        std::string sql = std::string("SELECT v FROM n WHERE ") + cond;
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        PlanPtr plan = plan_select(bound);
        PlanPtr cleaned = optimizer_of(std::make_unique<BooleanCleanup>()).optimize(plan).plan;

        auto run = [&](const PlanPtr& logical) {
            std::unique_ptr<Operator> root = build_operator(*plan_physical(*logical), db.catalog());
            std::vector<Row> rows;
            while (auto row = root->next()) rows.push_back(*row);
            return rows;
        };
        REQUIRE(run(cleaned) == run(plan));
    }
}
