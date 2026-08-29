#include <catch2/catch_test_macros.hpp>

#include "binder/format.h"
#include "logical/plan_printer.h"
#include "logical/planner.h"
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

std::string plan_of(const char* sql) {
    static const Catalog catalog = make_catalog();
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), catalog);
    return print(*plan_select(bound), bound.scope);
}

}  // namespace

TEST_CASE("plan: select star") {
    REQUIRE(plan_of("SELECT * FROM users") ==
            "Project[id, name, score, active]\n"
            "  Scan[users]");
}

TEST_CASE("plan: filter") {
    REQUIRE(plan_of("SELECT name FROM users WHERE score > 2.5") ==
            "Project[name]\n"
            "  Filter[score > 2.5]\n"
            "    Scan[users]");
}

TEST_CASE("plan: every step, in order") {
    REQUIRE(plan_of("SELECT name, score * 2 FROM users WHERE active AND id BETWEEN 1 AND 10 "
                    "ORDER BY score DESC, name LIMIT 3") ==
            "Limit[3]\n"
            "  Project[name, score * 2]\n"
            "    Sort[score DESC, name ASC]\n"
            "      Filter[active AND (id >= 1 AND id <= 10)]\n"
            "        Scan[users]");
}

TEST_CASE("plan: ORDER BY a column that isn't selected sorts before projecting") {
    REQUIRE(plan_of("SELECT name FROM users ORDER BY id") ==
            "Project[name]\n"
            "  Sort[id ASC]\n"
            "    Scan[users]");
}

TEST_CASE("plan: rewritten predicates and NULL tests") {
    REQUIRE(plan_of("SELECT id FROM users WHERE NOT (id = 1 OR active) AND name IS NOT NULL "
                    "LIMIT 0") ==
            "Limit[0]\n"
            "  Project[id]\n"
            "    Filter[id <> 1 AND NOT active AND name IS NOT NULL]\n"
            "      Scan[users]");
}

TEST_CASE("plan: IN and strings") {
    REQUIRE(plan_of("SELECT id FROM users WHERE name IN ('ama', 'it''s')") ==
            "Project[id]\n"
            "  Filter[name = 'ama' OR name = 'it''s']\n"
            "    Scan[users]");
}

TEST_CASE("plan: the same query always prints the same plan") {
    const char* sql = "SELECT name FROM users WHERE id > 1 ORDER BY name LIMIT 2";
    REQUIRE(plan_of(sql) == plan_of(sql));
}

TEST_CASE("plan: nodes are shared, not copied") {
    Catalog catalog = make_catalog();
    BoundSelect bound = bind_select(std::get<Select>(parse_statement("SELECT * FROM users WHERE id = 1").node), catalog);
    PlanPtr plan = plan_select(bound);
    const auto& project = std::get<LogicalProject>(plan->node);
    const auto& filter = std::get<LogicalFilter>(project.input->node);
    REQUIRE(filter.predicate == bound.where);
}

TEST_CASE("format: columns that exist in two tables get their alias") {
    TableInfo users{"users", {{"id", Type::Int}, {"name", Type::Text}}};
    TableInfo orders{"orders", {{"id", Type::Int}, {"user_id", Type::Int}}};
    Scope scope;
    scope.add("u", users);
    scope.add("o", orders);
    auto e = bind_expression(*parse_expression("u.id = o.user_id AND name = 'a' AND o.id > 1"), scope);
    REQUIRE(format(*e, scope) == "u.id = user_id AND name = 'a' AND o.id > 1");
}

TEST_CASE("format: brackets only where the tree needs them") {
    TableInfo t{"t", {{"a", Type::Int}, {"b", Type::Int}, {"f", Type::Bool}, {"g", Type::Bool}}};
    Scope scope;
    scope.add("t", t);
    auto show = [&](const char* sql) { return format(*bind_expression(*parse_expression(sql), scope), scope); };
    REQUIRE(show("a + b * 2") == "a + b * 2");
    REQUIRE(show("(a + b) * 2") == "(a + b) * 2");
    REQUIRE(show("a - (b - 1)") == "a - (b - 1)");
    REQUIRE(show("a - b - 1") == "a - b - 1");
    REQUIRE(show("-(a + 1) > 0") == "-(a + 1) > 0");
    REQUIRE(show("-a > 0") == "-a > 0");
    REQUIRE(show("f OR g AND f") == "f OR g AND f");
    REQUIRE(show("(f OR g) AND f") == "(f OR g) AND f");
    REQUIRE(show("f AND (g AND f)") == "f AND (g AND f)");
    REQUIRE(show("f AND g AND f") == "f AND g AND f");
    REQUIRE(show("NOT f") == "NOT f");
    REQUIRE(show("a + 1 IS NULL") == "a + 1 IS NULL");
    REQUIRE(show("(a = 1) = f") == "(a = 1) = f");
    REQUIRE(show("a = NULL") == "a = NULL");
    REQUIRE(show("f = TRUE") == "f = TRUE");
}
