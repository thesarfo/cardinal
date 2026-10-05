#include <catch2/catch_test_macros.hpp>

#include "sql/ast_printer.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

std::vector<ExprPtr> list(ExprPtr a, ExprPtr b) {
    std::vector<ExprPtr> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    return v;
}

}  // namespace

TEST_CASE("print: literals") {
    REQUIRE(print(*lit(i(25))) == "25");
    REQUIRE(print(*lit(Value(1.5))) == "1.5");
    REQUIRE(print(*lit(Value(true))) == "true");
    REQUIRE(print(*lit(Value())) == "null");
    REQUIRE(print(*lit(s("Ghana"))) == "'Ghana'");
    REQUIRE(print(*lit(s("it's"))) == "'it''s'");
}

TEST_CASE("print: columns") {
    REQUIRE(print(*col("age")) == "(col age)");
    REQUIRE(print(*col("u", "age")) == "(col u.age)");
}

TEST_CASE("print: operators") {
    auto e = binary(BinaryOp::Or, binary(BinaryOp::Gt, col("age"), lit(i(25))),
                    unary(UnaryOp::Not, binary(BinaryOp::Eq, col("a"), lit(Value()))));
    REQUIRE(print(*e) == "(or (> (col age) 25) (not (= (col a) null)))");
    REQUIRE(print(*unary(UnaryOp::Neg, col("x"))) == "(neg (col x))");
    REQUIRE(print(*binary(BinaryOp::Ne, col("a"), col("b"))) == "(<> (col a) (col b))");
    REQUIRE(print(*binary(BinaryOp::Le, col("a"), col("b"))) == "(<= (col a) (col b))");
    REQUIRE(print(*binary(BinaryOp::Mod, col("a"), lit(i(2)))) == "(% (col a) 2)");
}

TEST_CASE("print: is null, between, in") {
    REQUIRE(print(*make_expr(IsNull{col("a"), false})) == "(is-null (col a))");
    REQUIRE(print(*make_expr(IsNull{col("a"), true})) == "(is-not-null (col a))");
    REQUIRE(print(*make_expr(Between{col("x"), lit(i(1)), lit(i(5)), false})) ==
            "(between (col x) 1 5)");
    REQUIRE(print(*make_expr(Between{col("x"), lit(i(1)), lit(i(5)), true})) ==
            "(not-between (col x) 1 5)");
    REQUIRE(print(*make_expr(InList{col("x"), list(lit(i(1)), lit(i(2))), false})) ==
            "(in (col x) 1 2)");
    REQUIRE(print(*make_expr(InList{col("x"), list(lit(i(1)), lit(i(2))), true})) ==
            "(not-in (col x) 1 2)");
}

TEST_CASE("print: select") {
    Select sel;
    sel.items.push_back({col("name")});
    sel.from.table = "users";
    sel.where = binary(BinaryOp::Gt, col("age"), lit(i(25)));
    Statement st{std::move(sel)};
    REQUIRE(print(st) == "(select (col name) (from users) (where (> (col age) 25)))");
}

TEST_CASE("print: select with everything") {
    Select sel;
    sel.items.push_back({Star{}});
    sel.from.table = "t";
    sel.order_by.push_back({col("a"), true});
    sel.order_by.push_back({col("b"), false});
    sel.limit = 5;
    REQUIRE(print(Statement{std::move(sel)}) ==
            "(select (star) (from t) (order-by (desc (col a)) (asc (col b))) (limit 5))");
}

TEST_CASE("print: create table") {
    CreateTable ct{"t", {{"a", Type::Int}, {"b", Type::Text}}};
    REQUIRE(print(Statement{std::move(ct)}) == "(create-table t (a int) (b text))");
}

TEST_CASE("print: insert") {
    Insert ins{"t", {}};
    ins.rows.push_back(list(lit(i(1)), lit(s("x"))));
    ins.rows.push_back(list(lit(Value()), lit(Value(false))));
    REQUIRE(print(Statement{std::move(ins)}) == "(insert t (row 1 'x') (row null false))");
}

TEST_CASE("print: explain") {
    Select sel;
    sel.items.push_back({Star{}});
    sel.from.table = "t";
    Explain ex{false, false, std::make_unique<Statement>(Statement{std::move(sel)})};
    REQUIRE(print(Statement{std::move(ex)}) == "(explain (select (star) (from t)))");
}
