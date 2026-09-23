#include <catch2/catch_test_macros.hpp>

#include "catalog/catalog.h"
#include "common/error.h"

using namespace cardinal;

namespace {

TableInfo users() { return {"users", {{"id", Type::Int}, {"name", Type::Text}, {"score", Type::Double}}}; }

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

}  // namespace

TEST_CASE("catalog: create and look up a table") {
    Catalog catalog;
    catalog.create_table(users());

    Table* t = catalog.get_table("users");
    REQUIRE(t != nullptr);
    REQUIRE(t->info().name == "users");
    REQUIRE(t->info().columns.size() == 3);
    REQUIRE(t->info().columns[1].name == "name");
    REQUIRE(t->info().columns[1].type == Type::Text);
    REQUIRE(t->info().find_column("score") == 2);
    REQUIRE_FALSE(t->info().find_column("nope").has_value());
}

TEST_CASE("catalog: unknown table is null, and names are case-sensitive") {
    Catalog catalog;
    catalog.create_table(users());
    REQUIRE(catalog.get_table("orders") == nullptr);
    REQUIRE(catalog.get_table("Users") == nullptr);
}

TEST_CASE("catalog: a table name can't be reused") {
    Catalog catalog;
    catalog.create_table(users());
    REQUIRE_THROWS_AS(catalog.create_table(users()), DbError);
    catalog.get_table("users")->insert({i(1), s("a"), Value(1.0)});
    REQUIRE_THROWS_AS(catalog.create_table({"users", {{"x", Type::Int}}}), DbError);
    REQUIRE(catalog.get_table("users")->rows().size() == 1);
}

TEST_CASE("catalog: a column name can't repeat") {
    Catalog catalog;
    REQUIRE_THROWS_AS(catalog.create_table({"t", {{"a", Type::Int}, {"a", Type::Text}}}), DbError);
    REQUIRE(catalog.get_table("t") == nullptr);
}

TEST_CASE("table: insert keeps rows in order") {
    Catalog catalog;
    Table& t = catalog.create_table(users());
    t.insert({i(1), s("ama"), Value(9.5)});
    t.insert({i(2), Value(), Value()});
    REQUIRE(t.rows().size() == 2);
    REQUIRE(t.rows()[0] == Row{i(1), s("ama"), Value(9.5)});
    REQUIRE(t.rows()[1][1].is_null());
}

TEST_CASE("table: wrong number of values is an error") {
    Catalog catalog;
    Table& t = catalog.create_table(users());
    REQUIRE_THROWS_AS(t.insert({i(1)}), DbError);
    REQUIRE_THROWS_AS(t.insert({i(1), s("a"), Value(1.0), i(4)}), DbError);
    REQUIRE_THROWS_AS(t.insert({}), DbError);
    REQUIRE(t.rows().empty());
}

TEST_CASE("table: values must fit their column") {
    Catalog catalog;
    Table& t = catalog.create_table(users());
    REQUIRE_THROWS_AS(t.insert({s("x"), s("a"), Value(1.0)}), DbError);
    REQUIRE_THROWS_AS(t.insert({i(1), i(2), Value(1.0)}), DbError);
    REQUIRE_THROWS_AS(t.insert({i(1), s("a"), s("high")}), DbError);
    REQUIRE(t.rows().empty());
    t.insert({i(1), s("a"), i(3)});
    REQUIRE(t.rows()[0][2] == Value(3.0));
    REQUIRE_THROWS_AS(t.insert({Value(1.5), s("a"), Value(1.0)}), DbError);
}
