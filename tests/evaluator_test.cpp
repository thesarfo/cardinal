#include <catch2/catch_test_macros.hpp>

#include "binder/binder.h"
#include "common/error.h"
#include "expr/evaluator.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }
const Value T(true), F(false), N;

// t(a INT, b INT, x DOUBLE, name TEXT, flag BOOL): positions 0..4
TableInfo t_info() {
    return {"t",
            {{"a", Type::Int},
             {"b", Type::Int},
             {"x", Type::Double},
             {"name", Type::Text},
             {"flag", Type::Bool}}};
}

Value eval(const char* sql, const Row& row = {}) {
    static const TableInfo info = t_info();
    Scope scope;
    scope.add("t", info);
    return evaluate(*bind_expression(*parse_expression(sql), scope), row);
}

BoundExprPtr node(auto n, std::optional<Type> type = Type::Bool) {
    return std::make_shared<const BoundExpr>(BoundExpr{std::move(n), type});
}

}  // namespace

// The three-valued truth tables come first: everything else leans on them.

TEST_CASE("truth table: AND") {
    REQUIRE(eval("TRUE AND TRUE") == T);
    REQUIRE(eval("TRUE AND FALSE") == F);
    REQUIRE(eval("TRUE AND NULL") == N);
    REQUIRE(eval("FALSE AND TRUE") == F);
    REQUIRE(eval("FALSE AND FALSE") == F);
    REQUIRE(eval("FALSE AND NULL") == F);
    REQUIRE(eval("NULL AND TRUE") == N);
    REQUIRE(eval("NULL AND FALSE") == F);
    REQUIRE(eval("NULL AND NULL") == N);
}

TEST_CASE("truth table: OR") {
    REQUIRE(eval("TRUE OR TRUE") == T);
    REQUIRE(eval("TRUE OR FALSE") == T);
    REQUIRE(eval("TRUE OR NULL") == T);
    REQUIRE(eval("FALSE OR TRUE") == T);
    REQUIRE(eval("FALSE OR FALSE") == F);
    REQUIRE(eval("FALSE OR NULL") == N);
    REQUIRE(eval("NULL OR TRUE") == T);
    REQUIRE(eval("NULL OR FALSE") == N);
    REQUIRE(eval("NULL OR NULL") == N);
}

TEST_CASE("truth table: NOT") {
    // The binder rewrites NOT away from constants, so build the node by hand.
    auto not_of = [](Value v) {
        return evaluate(*node(BoundUnary{UnaryOp::Not, node(BoundLiteral{v})}), {});
    };
    REQUIRE(not_of(T) == F);
    REQUIRE(not_of(F) == T);
    REQUIRE(not_of(N) == N);
    // Through a column the NOT survives binding.
    REQUIRE(eval("NOT flag", {i(0), i(0), Value(0.0), s(""), T}) == F);
    REQUIRE(eval("NOT flag", {i(0), i(0), Value(0.0), s(""), N}) == N);
}

TEST_CASE("truth table: IS NULL") {
    REQUIRE(eval("NULL IS NULL") == T);
    REQUIRE(eval("1 IS NULL") == F);
    REQUIRE(eval("NULL IS NOT NULL") == F);
    REQUIRE(eval("1 IS NOT NULL") == T);
}

TEST_CASE("NULL carries through comparisons and arithmetic") {
    Row row{N, i(2), N, N, N};
    REQUIRE(eval("a = 1", row) == N);
    REQUIRE(eval("a = a", row) == N);  // x = x is not always true
    REQUIRE(eval("a <> a", row) == N);
    REQUIRE(eval("a < b", row) == N);
    REQUIRE(eval("a + b", row) == N);
    REQUIRE(eval("-a", row) == N);
    REQUIRE(eval("a * 0", row) == N);
    REQUIRE(eval("x > 1.5", row) == N);
    REQUIRE(eval("name = 'a'", row) == N);
    REQUIRE(eval("a IS NULL", row) == T);
    REQUIRE(eval("a = 1 OR b = 2", row) == T);
    REQUIRE(eval("a = 1 AND b = 3", row) == F);
    REQUIRE(eval("a = 1 AND b = 2", row) == N);
    REQUIRE(eval("a = 1 OR b = 3", row) == N);
    REQUIRE(eval("a BETWEEN 1 AND 5", row) == N);
    REQUIRE(eval("a IN (1, 2)", row) == N);
    REQUIRE(eval("b IN (a, 2)", row) == T);
    REQUIRE(eval("b IN (a, 3)", row) == N);
    REQUIRE(eval("NOT a = 1", row) == N);
    REQUIRE(eval("NULL = NULL") == N);
}

TEST_CASE("is_true keeps only true") {
    REQUIRE(is_true(T));
    REQUIRE_FALSE(is_true(F));
    REQUIRE_FALSE(is_true(N));
}

TEST_CASE("columns are read by position") {
    Row row{i(10), i(3), Value(2.5), s("ama"), T};
    REQUIRE(eval("a", row) == i(10));
    REQUIRE(eval("name", row) == s("ama"));
    REQUIRE(eval("x", row) == Value(2.5));
    REQUIRE(eval("flag", row) == T);
}

TEST_CASE("integer arithmetic stays integer") {
    Row row{i(10), i(3), N, N, N};
    REQUIRE(eval("a + b", row) == i(13));
    REQUIRE(eval("a - b", row) == i(7));
    REQUIRE(eval("a * b", row) == i(30));
    REQUIRE(eval("a / b", row) == i(3));
    REQUIRE(eval("a % b", row) == i(1));
    REQUIRE(eval("-a", row) == i(-10));
    REQUIRE(eval("-7 / 2") == i(-3));  // truncates toward zero
    REQUIRE(eval("-7 % 3") == i(-1));
    REQUIRE(eval("1 + 2 * 3") == i(7));
}

TEST_CASE("mixing INT and DOUBLE gives DOUBLE") {
    Row row{i(10), i(4), Value(2.5), N, N};
    REQUIRE(eval("a + x", row) == Value(12.5));
    REQUIRE(eval("a / 4.0", row) == Value(2.5));
    REQUIRE(eval("x * b", row) == Value(10.0));
    REQUIRE(eval("-x", row) == Value(-2.5));
    REQUIRE(eval("1.5 + 1.5") == Value(3.0));
}

TEST_CASE("INT and DOUBLE compare as numbers") {
    Row row{i(3), i(0), Value(3.0), N, N};
    REQUIRE(eval("a = x", row) == T);
    REQUIRE(eval("a < 3.5", row) == T);
    REQUIRE(eval("x >= 3", row) == T);
    REQUIRE(eval("x > 3", row) == F);
    REQUIRE(eval("a <> 3.5", row) == T);
    REQUIRE(eval("a BETWEEN 2.5 AND 3.5", row) == T);
}

TEST_CASE("comparisons") {
    Row row{i(1), i(2), N, s("ama"), F};
    REQUIRE(eval("a < b", row) == T);
    REQUIRE(eval("a <= b", row) == T);
    REQUIRE(eval("a > b", row) == F);
    REQUIRE(eval("a >= b", row) == F);
    REQUIRE(eval("a = b", row) == F);
    REQUIRE(eval("a <> b", row) == T);
    REQUIRE(eval("name = 'ama'", row) == T);
    REQUIRE(eval("name < 'kofi'", row) == T);
    REQUIRE(eval("name > 'Ama'", row) == T);  // by bytes, so lower case sorts after upper
    REQUIRE(eval("'' < 'a'") == T);
    REQUIRE(eval("flag = FALSE", row) == T);
    REQUIRE(eval("flag < TRUE", row) == T);
}

TEST_CASE("BETWEEN and IN") {
    Row row{i(5), i(0), N, s("b"), N};
    REQUIRE(eval("a BETWEEN 5 AND 5", row) == T);
    REQUIRE(eval("a BETWEEN 1 AND 4", row) == F);
    REQUIRE(eval("a NOT BETWEEN 1 AND 4", row) == T);
    REQUIRE(eval("a IN (1, 5, 9)", row) == T);
    REQUIRE(eval("a IN (1, 2)", row) == F);
    REQUIRE(eval("a NOT IN (1, 2)", row) == T);
    REQUIRE(eval("name IN ('a', 'b')", row) == T);
}

TEST_CASE("errors") {
    Row row{i(1), i(0), Value(0.0), N, N};
    REQUIRE_THROWS_AS(eval("a / b", row), DbError);
    REQUIRE_THROWS_AS(eval("a % b", row), DbError);
    REQUIRE_THROWS_AS(eval("a / x", row), DbError);
    REQUIRE_THROWS_AS(eval("1.5 / 0"), DbError);
    // Both sides are evaluated, so the error shows even when the answer is decided.
    REQUIRE_THROWS_AS(eval("FALSE AND 1 / 0 = 1"), DbError);
    REQUIRE_THROWS_AS(eval("NULL + 1 / 0"), DbError);

    constexpr std::int64_t big = INT64_MAX;
    Row edge{i(big), i(-1), N, N, N};
    REQUIRE_THROWS_AS(eval("a + 1", edge), DbError);
    REQUIRE_THROWS_AS(eval("a * 2", edge), DbError);
    REQUIRE_THROWS_AS(eval("-a - 2", edge), DbError);
    REQUIRE(eval("a - 1", edge) == i(big - 1));
    Row min{i(INT64_MIN), i(-1), N, N, N};
    REQUIRE_THROWS_AS(eval("a / b", min), DbError);
    REQUIRE_THROWS_AS(eval("-a", min), DbError);
    try {
        eval("a / b", row);
    } catch (const DbError& e) {
        REQUIRE(std::string(e.what()) == "division by zero");
    }
}
