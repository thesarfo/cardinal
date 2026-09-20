#include "optimizer/constant_folding.h"

#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

void folds(const std::string& before, const std::string& after) {
    assert_rewrite(std::make_unique<ConstantFolding>(), before, after);
}

void leaves_alone(const std::string& sql) { assert_unchanged(std::make_unique<ConstantFolding>(), sql); }

}  // namespace

TEST_CASE("folding: arithmetic on literals") {
    folds("SELECT a FROM t WHERE a > 20 + 5", "SELECT a FROM t WHERE a > 25");
    folds("SELECT a FROM t WHERE a = 2 * 3 + 1", "SELECT a FROM t WHERE a = 7");
    folds("SELECT a FROM t WHERE a = (1 + 2) * (3 + 4)", "SELECT a FROM t WHERE a = 21");
    folds("SELECT a FROM t WHERE a = 7 / 2", "SELECT a FROM t WHERE a = 3");  // whole numbers stay whole
    folds("SELECT a FROM t WHERE a = -7 % 3", "SELECT a FROM t WHERE a = -1");
    folds("SELECT a FROM t WHERE a = 10 - 4 - 3", "SELECT a FROM t WHERE a = 3");
    folds("SELECT a FROM t WHERE x > 1.5 + 1.5", "SELECT a FROM t WHERE x > 3.0");
    folds("SELECT a FROM t WHERE x > 1 + 0.5", "SELECT a FROM t WHERE x > 1.5");
    folds("SELECT a FROM t WHERE a = - (2 + 3)", "SELECT a FROM t WHERE a = -5");
}

TEST_CASE("folding: only the constant parts fold") {
    folds("SELECT a FROM t WHERE a + (1 + 2) > b * (3 * 4)", "SELECT a FROM t WHERE a + 3 > b * 12");
    folds("SELECT a FROM t WHERE a = b AND 1 + 1 = 2", "SELECT a FROM t WHERE a = b AND TRUE");
    leaves_alone("SELECT a FROM t WHERE a + 1 > b");
    leaves_alone("SELECT a FROM t WHERE a = 25");
}

TEST_CASE("folding: NULL") {
    folds("SELECT a FROM t WHERE a > NULL + 1", "SELECT a FROM t WHERE a > NULL");
    folds("SELECT a FROM t WHERE NULL = NULL", "SELECT a FROM t WHERE NULL");
    folds("SELECT a FROM t WHERE 1 + NULL * 2 > 0", "SELECT a FROM t WHERE NULL");
    // Not always NULL: AND and OR can be decided by one side.
    folds("SELECT a FROM t WHERE NULL AND FALSE", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE FALSE AND NULL", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE NULL OR TRUE", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE NULL AND TRUE", "SELECT a FROM t WHERE NULL");
    folds("SELECT a FROM t WHERE NULL OR FALSE", "SELECT a FROM t WHERE NULL");
    folds("SELECT a FROM t WHERE NULL IS NULL", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE NULL IS NOT NULL", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE 1 IS NULL", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE 1 + NULL IS NULL", "SELECT a FROM t WHERE TRUE");
}

TEST_CASE("folding: comparisons and logic on literals") {
    folds("SELECT a FROM t WHERE 1 < 2", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE 1 >= 2", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE 1 = 1.0", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE 'a' < 'b'", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE TRUE AND FALSE", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE NOT TRUE", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE 3 BETWEEN 1 AND 5", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE 9 BETWEEN 1 AND 5", "SELECT a FROM t WHERE FALSE");
    folds("SELECT a FROM t WHERE 2 IN (1, 2, 3)", "SELECT a FROM t WHERE TRUE");
    folds("SELECT a FROM t WHERE 5 NOT IN (1, 2, 3)", "SELECT a FROM t WHERE TRUE");
}

TEST_CASE("folding: errors are left for the executor") {
    leaves_alone("SELECT a FROM t WHERE 1 / 0 = 1");
    leaves_alone("SELECT a FROM t WHERE 5 > 1 / 0");
    leaves_alone("SELECT a FROM t WHERE 1 % 0 = 1");
    leaves_alone("SELECT a FROM t WHERE 1.5 / 0 > 1");
    leaves_alone("SELECT a FROM t WHERE 9223372036854775807 + 1 > 0");
    leaves_alone("SELECT 1 / 0 FROM t");
    // FALSE AND (1/0 = 1) would be FALSE if only one side were looked at, but the
    // evaluator works out both sides, and folding must agree with it.
    leaves_alone("SELECT a FROM t WHERE FALSE AND 1 / 0 = 1");
    // The parts that are fine still fold.
    folds("SELECT a FROM t WHERE 1 / 0 + (2 + 3) = 1", "SELECT a FROM t WHERE 1 / 0 + 5 = 1");
    folds("SELECT a FROM t WHERE a > 1 / 0 AND 1 + 1 = 2", "SELECT a FROM t WHERE a > 1 / 0 AND TRUE");
}

TEST_CASE("folding: x = x is not touched") {
    leaves_alone("SELECT a FROM t WHERE a = a");
    leaves_alone("SELECT a FROM t WHERE a <> a");
    leaves_alone("SELECT a FROM t WHERE flag OR NOT flag");
}

TEST_CASE("folding: select list and sort keys fold too") {
    folds("SELECT 1 + 2 FROM t", "SELECT 3 FROM t");
    folds("SELECT a, 2 * 3 FROM t", "SELECT a, 6 FROM t");
    folds("SELECT a FROM t ORDER BY a, 1 + 1", "SELECT a FROM t ORDER BY a, 2");
    folds("SELECT a + (1 + 1) FROM t ORDER BY a DESC", "SELECT a + 2 FROM t ORDER BY a DESC");
}

TEST_CASE("folding: result column names don't change") {
    Planned p = plan_sql("SELECT 1 + 2 FROM t");
    OptimizeResult result = optimizer_of(std::make_unique<ConstantFolding>()).optimize(p.plan);
    const auto& project = std::get<LogicalProject>(result.plan->node);
    REQUIRE(project.items[0].name == "?column?");
    REQUIRE(project.items[0].expr->type == Type::Int);
}

TEST_CASE("folding: the trace names the rule") {
    Planned p = plan_sql("SELECT a FROM t WHERE a > 20 + 5");
    OptimizeResult result = optimizer_of(std::make_unique<ConstantFolding>()).optimize(p.plan);
    REQUIRE(format_trace(result.trace, p.bound.scope) ==
            "1. constant-folding (pass 1)\n"
            "   before:\n"
            "     Project[a]\n"
            "       Filter[a > 20 + 5]\n"
            "         Scan[t]\n"
            "   after:\n"
            "     Project[a]\n"
            "       Filter[a > 25]\n"
            "         Scan[t]");
}

TEST_CASE("folding: folded results agree with running the query") {
    // Whatever the folder decides, the executor must say the same.
    Database db;
    db.execute("CREATE TABLE n (v INT)");
    db.execute("INSERT INTO n VALUES (1), (2), (3), (NULL)");
    const char* conditions[] = {"v > 1 + 1", "v = 7 / 2", "NULL AND v = 1", "v = 1 OR NULL", "NOT (v < 2 + 0)",
                                "v BETWEEN 1 + 0 AND 3 - 1", "v IN (1 + 1, 3)", "1 = 1 AND v IS NOT NULL"};
    for (const char* cond : conditions) {
        INFO(cond);
        std::string sql = std::string("SELECT v FROM n WHERE ") + cond;
        BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), db.catalog());
        PlanPtr plan = plan_select(bound);
        PlanPtr folded = optimizer_of(std::make_unique<ConstantFolding>()).optimize(plan).plan;

        auto run = [&](const PlanPtr& logical) {
            std::unique_ptr<Operator> root = build_operator(*plan_physical(*logical), db.catalog());
            std::vector<Row> rows;
            while (auto row = root->next()) rows.push_back(*row);
            return rows;
        };
        REQUIRE(run(folded) == run(plan));
    }
}
