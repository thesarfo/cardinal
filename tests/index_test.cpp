#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

#include "common/error.h"
#include "storage/table.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }

Bound at(std::int64_t v, bool inclusive = true) { return Bound{Value(v), inclusive}; }

}  // namespace

TEST_CASE("index: lookup finds every row with the key, in insertion order") {
    OrderedIndex idx;
    idx.insert(i(5), 0);
    idx.insert(i(3), 1);
    idx.insert(i(5), 2);
    REQUIRE(idx.lookup(i(5)) == std::vector<std::size_t>{0, 2});
    REQUIRE(idx.lookup(i(3)) == std::vector<std::size_t>{1});
    REQUIRE(idx.lookup(i(4)).empty());
}

TEST_CASE("index: NULL is never stored or found") {
    OrderedIndex idx;
    idx.insert(Value(), 0);
    idx.insert(i(1), 1);
    REQUIRE(idx.size() == 1);
    REQUIRE(idx.lookup(Value()).empty());
    REQUIRE(idx.range(std::nullopt, std::nullopt) == std::vector<std::size_t>{1});
}

TEST_CASE("index: range honours open and closed ends") {
    OrderedIndex idx;
    for (std::int64_t k = 1; k <= 5; ++k) idx.insert(i(k), static_cast<std::size_t>(k * 10));
    REQUIRE(idx.range(at(2), at(4)) == std::vector<std::size_t>{20, 30, 40});
    REQUIRE(idx.range(at(2, false), at(4, false)) == std::vector<std::size_t>{30});
    REQUIRE(idx.range(at(4), std::nullopt) == std::vector<std::size_t>{40, 50});
    REQUIRE(idx.range(std::nullopt, at(2, false)) == std::vector<std::size_t>{10});
    REQUIRE(idx.range(at(4), at(2)).empty());
    REQUIRE(idx.range(at(3, false), at(3, false)).empty());
}

TEST_CASE("index: an INT key finds a DOUBLE stored key") {
    OrderedIndex idx;
    idx.insert(Value(2.0), 7);
    REQUIRE(idx.lookup(i(2)) == std::vector<std::size_t>{7});
}

TEST_CASE("table: an index is built from existing rows and follows later inserts") {
    Table t(TableInfo{"t", {{"a", Type::Int}, {"b", Type::Text}}});
    t.insert({i(1), Value(std::string("x"))});
    t.insert({i(2), Value(std::string("y"))});
    t.create_index("t_a", "a");
    t.insert({i(2), Value(std::string("z"))});
    t.insert_rows({{i(3), Value()}, {i(1), Value()}});
    const auto* e = t.index_on("a");
    REQUIRE(e != nullptr);
    REQUIRE(e->index.lookup(i(2)) == std::vector<std::size_t>{1, 2});
    REQUIRE(e->index.lookup(i(1)) == std::vector<std::size_t>{0, 4});
    REQUIRE(t.index_on("b") == nullptr);
    REQUIRE_THROWS_AS(t.create_index("t_a", "b"), DbError);
    REQUIRE_THROWS_AS(t.create_index("t_q", "q"), DbError);
}

TEST_CASE("index: random inserts agree with a scan") {
    std::mt19937_64 rng(5);
    OrderedIndex idx;
    std::vector<std::int64_t> keys;
    for (std::size_t r = 0; r < 2000; ++r) {
        keys.push_back(static_cast<std::int64_t>(rng() % 100));
        idx.insert(i(keys.back()), r);
    }
    for (int q = 0; q < 50; ++q) {
        std::int64_t lo = static_cast<std::int64_t>(rng() % 100), hi = static_cast<std::int64_t>(rng() % 100);
        bool li = rng() % 2, hi_in = rng() % 2;
        std::vector<std::size_t> got = idx.range(at(lo, li), at(hi, hi_in));
        std::sort(got.begin(), got.end());
        std::vector<std::size_t> want;
        for (std::size_t r = 0; r < keys.size(); ++r)
            if ((li ? keys[r] >= lo : keys[r] > lo) && (hi_in ? keys[r] <= hi : keys[r] < hi)) want.push_back(r);
        REQUIRE(got == want);
    }
}
