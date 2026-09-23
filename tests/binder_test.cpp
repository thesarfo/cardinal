#include <catch2/catch_test_macros.hpp>

#include "binder/binder.h"
#include "common/error.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

TableInfo users_info() {
    return {"users",
            {{"id", Type::Int}, {"name", Type::Text}, {"score", Type::Double}, {"active", Type::Bool}}};
}
TableInfo orders_info() {
    return {"orders", {{"id", Type::Int}, {"user_id", Type::Int}, {"amount", Type::Double}}};
}

// users: id=#0 name=#1 score=#2 active=#3
struct UsersScope {
    TableInfo users = users_info();
    Scope scope;
    UsersScope() { scope.add("users", users); }
};

BoundExprPtr bind_in(const Scope& scope, const char* sql) {
    return bind_expression(*parse_expression(sql), scope);
}

std::string bound(const char* sql) {
    UsersScope s;
    return print(*bind_in(s.scope, sql));
}

void require_error(const Scope& scope, const char* sql, const std::string& message) {
    INFO(sql);
    try {
        bind_in(scope, sql);
        FAIL("expected a DbError");
    } catch (const DbError& e) {
        REQUIRE(std::string(e.what()) == message);
    }
}

void require_error(const char* sql, const std::string& message) {
    UsersScope s;
    require_error(s.scope, sql, message);
}

BoundSelect bind_sql(const Catalog& catalog, const char* sql) {
    return bind_select(std::get<Select>(parse_statement(sql).node), catalog);
}

Catalog make_catalog() {
    Catalog catalog;
    catalog.create_table(users_info());
    catalog.create_table(orders_info());
    return catalog;
}

}  // namespace

TEST_CASE("bind: columns become ids") {
    REQUIRE(bound("id") == "#0");
    REQUIRE(bound("score") == "#2");
    REQUIRE(bound("users.active") == "#3");
    REQUIRE(bound("id + 1 > score") == "(> (+ #0 1) #2)");
}

TEST_CASE("bind: BETWEEN becomes AND of comparisons") {
    REQUIRE(bound("score BETWEEN 1 AND 5") == "(and (>= #2 1) (<= #2 5))");
    REQUIRE(bound("score NOT BETWEEN 1 AND 5") == "(or (< #2 1) (> #2 5))");
}

TEST_CASE("bind: IN becomes OR of equalities") {
    REQUIRE(bound("id IN (1, 2, 3)") == "(or (or (= #0 1) (= #0 2)) (= #0 3))");
    REQUIRE(bound("id IN (1)") == "(= #0 1)");
    REQUIRE(bound("id NOT IN (1, 2)") == "(and (<> #0 1) (<> #0 2))");
}

TEST_CASE("bind: NOT is pushed inward") {
    REQUIRE(bound("NOT id = 1") == "(<> #0 1)");
    REQUIRE(bound("NOT id < 1") == "(>= #0 1)");
    REQUIRE(bound("NOT id <= 1") == "(> #0 1)");
    REQUIRE(bound("NOT (id < 1 AND score >= 2)") == "(or (>= #0 1) (< #2 2))");
    REQUIRE(bound("NOT (id = 1 OR active)") == "(and (<> #0 1) (not #3))");
    REQUIRE(bound("NOT NOT active") == "#3");
    REQUIRE(bound("NOT name IS NULL") == "(is-not-null #1)");
    REQUIRE(bound("NOT name IS NOT NULL") == "(is-null #1)");
    REQUIRE(bound("NOT TRUE") == "false");
    REQUIRE(bound("NOT NULL") == "null");
    // Nothing to push into: the NOT stays.
    REQUIRE(bound("NOT active") == "(not #3)");
}

TEST_CASE("bind: other forms pass through") {
    REQUIRE(bound("-id + 1") == "(+ (neg #0) 1)");
    REQUIRE(bound("name IS NULL") == "(is-null #1)");
    REQUIRE(bound("name = 'a' OR id = 1 AND active") == "(or (= #1 'a') (and (= #0 1) #3))");
}

TEST_CASE("bind: INT and DOUBLE mix as numbers") {
    UsersScope s;
    REQUIRE(*bind_in(s.scope, "id + 1")->type == Type::Int);
    REQUIRE(*bind_in(s.scope, "id / 2")->type == Type::Int);
    REQUIRE(*bind_in(s.scope, "id + score")->type == Type::Double);
    REQUIRE(*bind_in(s.scope, "id * 1.5")->type == Type::Double);
    REQUIRE(*bind_in(s.scope, "id % 2")->type == Type::Int);
    REQUIRE(*bind_in(s.scope, "id < score")->type == Type::Bool);
    REQUIRE(*bind_in(s.scope, "-score")->type == Type::Double);
    REQUIRE(*bind_in(s.scope, "NULL + 1")->type == Type::Int);
    REQUIRE_FALSE(bind_in(s.scope, "NULL + NULL")->type.has_value());
    REQUIRE_FALSE(bind_in(s.scope, "NULL")->type.has_value());
}

TEST_CASE("bind: NULL fits any type") {
    REQUIRE(bound("name = NULL") == "(= #1 null)");
    REQUIRE(bound("active AND NULL") == "(and #3 null)");
    REQUIRE(bound("id + NULL") == "(+ #0 null)");
}

TEST_CASE("bind: type errors") {
    require_error("name = 1", "cannot compare TEXT with INT");
    require_error("active < 1", "cannot compare BOOL with INT");
    require_error("name + 1", "operator + needs numbers, got TEXT");
    require_error("1 * active", "operator * needs numbers, got BOOL");
    require_error("score % 2", "operator % needs whole numbers, got DOUBLE");
    require_error("id AND active", "AND needs true/false values, got INT");
    require_error("active OR 'x'", "OR needs true/false values, got TEXT");
    require_error("NOT id", "NOT needs a true/false value, got INT");
    require_error("-name", "unary - needs a number, got TEXT");
    require_error("name BETWEEN 1 AND 5", "cannot compare TEXT with INT");
    require_error("id IN (1, 'a')", "cannot compare INT with TEXT");
}

TEST_CASE("bind: unknown names") {
    require_error("nope", "unknown column nope");
    require_error("users.nope", "unknown column users.nope");
    require_error("x.id", "unknown table or alias x");
}

TEST_CASE("bind: a self-join gets different ids for each side") {
    TableInfo users = users_info();
    Scope scope;
    scope.add("a", users);
    scope.add("b", users);

    REQUIRE(print(*bind_in(scope, "a.id")) == "#0");
    REQUIRE(print(*bind_in(scope, "b.id")) == "#4");
    REQUIRE(print(*bind_in(scope, "a.id = b.id")) == "(= #0 #4)");
    REQUIRE(print(*bind_in(scope, "a.name = b.name AND a.score > b.score")) ==
            "(and (= #1 #5) (> #2 #6))");
    REQUIRE(scope.meta(ColumnId{4}).table == "b");
    REQUIRE(scope.meta(ColumnId{4}).name == "id");
    REQUIRE(scope.columns().size() == 8);

    // The table's own name isn't visible once it has an alias.
    require_error(scope, "users.id", "unknown table or alias users");
}

TEST_CASE("bind: ambiguous column") {
    TableInfo users = users_info(), orders = orders_info();
    Scope scope;
    scope.add("users", users);
    scope.add("orders", orders);

    require_error(scope, "id", "column id is ambiguous: it could be users.id or orders.id");
    REQUIRE(print(*bind_in(scope, "user_id")) == "#5");
    REQUIRE(print(*bind_in(scope, "orders.id")) == "#4");
    REQUIRE(print(*bind_in(scope, "users.id = orders.user_id")) == "(= #0 #5)");

    Scope twice;
    twice.add("a", users);
    twice.add("b", users);
    require_error(twice, "id", "column id is ambiguous: it could be a.id or b.id");
}

TEST_CASE("bind: a table name can't be used twice in a scope") {
    TableInfo users = users_info();
    Scope scope;
    scope.add("users", users);
    REQUIRE_THROWS_AS(scope.add("users", users), DbError);
}

TEST_CASE("bind select: star expands to every column") {
    Catalog catalog = make_catalog();
    BoundSelect s = bind_sql(catalog, "SELECT * FROM users");
    REQUIRE(s.items.size() == 4);
    REQUIRE(s.items[0].name == "id");
    REQUIRE(s.items[3].name == "active");
    REQUIRE(print(*s.items[2].expr) == "#2");
    REQUIRE(s.where == nullptr);
    REQUIRE(s.order_by.empty());
    REQUIRE_FALSE(s.limit.has_value());
}

TEST_CASE("bind select: every clause") {
    Catalog catalog = make_catalog();
    BoundSelect s = bind_sql(catalog,
                             "SELECT name, id + 1 FROM users WHERE score BETWEEN 1 AND 5 "
                             "ORDER BY id DESC, name LIMIT 3");
    REQUIRE(s.items.size() == 2);
    REQUIRE(s.items[0].name == "name");
    REQUIRE(print(*s.items[0].expr) == "#1");
    REQUIRE(s.items[1].name == "?column?");
    REQUIRE(print(*s.items[1].expr) == "(+ #0 1)");
    REQUIRE(print(*s.where) == "(and (>= #2 1) (<= #2 5))");
    REQUIRE(s.order_by.size() == 2);
    REQUIRE(print(*s.order_by[0].expr) == "#0");
    REQUIRE(s.order_by[0].descending);
    REQUIRE_FALSE(s.order_by[1].descending);
    REQUIRE(*s.limit == 3);
}

TEST_CASE("bind select: uses the right table's ids") {
    Catalog catalog = make_catalog();
    BoundSelect s = bind_sql(catalog, "SELECT amount FROM orders WHERE user_id = 7");
    REQUIRE(print(*s.items[0].expr) == "#2");
    REQUIRE(print(*s.where) == "(= #1 7)");
}

TEST_CASE("bind select: errors") {
    Catalog catalog = make_catalog();
    auto message = [&](const char* sql) {
        try {
            bind_sql(catalog, sql);
        } catch (const DbError& e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    REQUIRE(message("SELECT * FROM nope") == "unknown table nope");
    REQUIRE(message("SELECT nope FROM users") == "unknown column nope");
    REQUIRE(message("SELECT * FROM users WHERE id") == "WHERE needs a true/false value, got INT");
    REQUIRE(message("SELECT * FROM users WHERE name = 1") == "cannot compare TEXT with INT");
    REQUIRE(message("SELECT * FROM users ORDER BY nope") == "unknown column nope");
    REQUIRE(message("SELECT * FROM users WHERE NULL") == "no error");
    REQUIRE(message("SELECT orders.id FROM users") == "unknown table or alias orders");
}
