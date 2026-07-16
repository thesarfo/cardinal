#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "common/version.h"

TEST_CASE("arithmetic works") { REQUIRE(1 + 1 == 2); }

TEST_CASE("version is not empty") { REQUIRE(std::strlen(cardinal::version()) > 0); }
