#include <catch2/catch_test_macros.hpp>

#include "logical/plan_printer.h"
#include "logical/planner.h"
#include "physical/physical_planner.h"
#include "physical/physical_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Catalog make_catalog() {
    Catalog catalog;
    catalog.create_table({"users",
                          {{"id", Type::Int},
                           {"name", Type::Text},
                           {"score", Type::Double},
                           {"active", Type::Bool}}});
    return catalog;
}

std::string physical_of(const char* sql) {
    static const Catalog catalog = make_catalog();
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), catalog);
    return print(*plan_physical(*plan_select(bound)));
}

PlanPtr logical(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

BoundExprPtr column(std::uint32_t id) {
    return std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{ColumnId{id}}, Type::Int});
}

}  // namespace

TEST_CASE("physical: one step for each logical step") {
    REQUIRE(physical_of("SELECT * FROM users") ==
            "Project[#0, #1, #2, #3]\n"
            "  SeqScan[users]");
    REQUIRE(physical_of("SELECT name FROM users WHERE score > 2.5") ==
            "Project[#1]\n"
            "  Filter[#2 > 2.5]\n"
            "    SeqScan[users]");
    REQUIRE(physical_of("SELECT name, score * 2 FROM users WHERE active AND id BETWEEN 1 AND 10 "
                        "ORDER BY score DESC, name LIMIT 3") ==
            "Limit[3]\n"
            "  Project[#1, #2 * 2]\n"
            "    Sort[#2 DESC, #1 ASC]\n"
            "      Filter[#3 AND (#0 >= 1 AND #0 <= 10)]\n"
            "        SeqScan[users]");
    REQUIRE(physical_of("SELECT name FROM users ORDER BY id") ==
            "Project[#1]\n"
            "  Sort[#0 ASC]\n"
            "    SeqScan[users]");
}

TEST_CASE("physical: ids become positions") {
    // A scan whose columns have ids 7, 8, 9: nothing like their positions.
    auto scan = logical(LogicalScan{"t", "t", {ColumnId{7}, ColumnId{8}, ColumnId{9}}});
    auto filter = logical(LogicalFilter{
        scan, std::make_shared<const BoundExpr>(BoundExpr{
                  BoundBinary{BinaryOp::Gt, column(9),
                              std::make_shared<const BoundExpr>(
                                  BoundExpr{BoundLiteral{Value(std::int64_t{1})}, Type::Int})},
                  Type::Bool})});
    auto project = logical(LogicalProject{filter, {{column(8), "b"}, {column(7), "a"}}});

    REQUIRE(print(*plan_physical(*project)) ==
            "Project[#1, #0]\n"
            "  Filter[#2 > 1]\n"
            "    SeqScan[t]");
}

TEST_CASE("physical: a column the input doesn't produce is a bug") {
    auto scan = logical(LogicalScan{"t", "t", {ColumnId{0}}});
    auto project = logical(LogicalProject{scan, {{column(5), "x"}}});
    REQUIRE_THROWS_AS(plan_physical(*project), std::logic_error);
}

TEST_CASE("physical: the logical plan is left alone") {
    Catalog catalog = make_catalog();
    BoundSelect bound = bind_select(std::get<Select>(parse_statement("SELECT name FROM users WHERE id = 1").node), catalog);
    PlanPtr plan = plan_select(bound);
    std::string before = print(*plan, bound.scope);
    plan_physical(*plan);
    REQUIRE(print(*plan, bound.scope) == before);
}
