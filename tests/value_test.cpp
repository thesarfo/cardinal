#include <catch2/catch_test_macros.hpp>

#include "common/value.h"

using cardinal::Row;
using cardinal::Type;
using cardinal::Value;

TEST_CASE("values print") {
    REQUIRE(Value().to_string() == "NULL");
    REQUIRE(Value(std::int64_t{42}).to_string() == "42");
    REQUIRE(Value(std::int64_t{-7}).to_string() == "-7");
    REQUIRE(Value(1.5).to_string() == "1.5");
    REQUIRE(Value(2.0).to_string() == "2.0");
    REQUIRE(Value(0.1).to_string() == "0.1");
    REQUIRE(Value(std::string("it's")).to_string() == "it's");
    REQUIRE(Value(true).to_string() == "true");
    REQUIRE(Value(false).to_string() == "false");
}

TEST_CASE("values know their type") {
    REQUIRE(Value().is_null());
    REQUIRE_FALSE(Value(std::int64_t{1}).is_null());
    REQUIRE(Value(std::int64_t{1}).type() == Type::Int);
    REQUIRE(Value(1.0).type() == Type::Double);
    REQUIRE(Value(std::string("a")).type() == Type::Text);
    REQUIRE(Value(true).type() == Type::Bool);
}

TEST_CASE("values compare") {
    REQUIRE(Value(std::int64_t{1}) == Value(std::int64_t{1}));
    REQUIRE(Value(std::int64_t{1}) != Value(std::int64_t{2}));
    REQUIRE(Value() == Value());
    REQUIRE(Value() != Value(std::int64_t{0}));
    // Same number, different type: not equal here.
    REQUIRE(Value(std::int64_t{1}) != Value(1.0));
    REQUIRE(Value(std::string("a")) == Value(std::string("a")));
}

TEST_CASE("rows compare") {
    Row a{Value(std::int64_t{1}), Value(), Value(std::string("x"))};
    Row b{Value(std::int64_t{1}), Value(), Value(std::string("x"))};
    REQUIRE(a == b);
    b[1] = Value(true);
    REQUIRE(a != b);
}

TEST_CASE("type names") {
    REQUIRE(std::string(cardinal::type_name(Type::Int)) == "INT");
    REQUIRE(std::string(cardinal::type_name(Type::Text)) == "TEXT");
}
