#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <random>

#include "index/bplus_tree.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }

}  // namespace

TEST_CASE("b+ tree: an empty tree finds nothing") {
    BPlusTree tree;
    REQUIRE(tree.size() == 0);
    REQUIRE(tree.height() == 1);
    REQUIRE(tree.lookup(i(1)).empty());
    REQUIRE_NOTHROW(tree.check());
}

TEST_CASE("b+ tree: NULL is not stored") {
    BPlusTree tree;
    tree.insert(Value(), 0);
    REQUIRE(tree.size() == 0);
    REQUIRE(tree.lookup(Value()).empty());
}

TEST_CASE("b+ tree: a node splits when it fills, and the tree grows a level") {
    BPlusTree tree(3);
    for (std::int64_t k = 1; k <= 3; ++k) tree.insert(i(k), static_cast<std::size_t>(k));
    REQUIRE(tree.height() == 1);
    REQUIRE(tree.to_text() == "(1:1 2:2 3:3)\n");
    tree.insert(i(4), 4);  // a fourth entry: the leaf splits and a root appears
    REQUIRE(tree.height() == 2);
    REQUIRE(tree.to_text() ==
            "[3:3]\n"
            "  (1:1 2:2)\n"
            "  (3:3 4:4)\n");
    tree.check();
}

TEST_CASE("b+ tree: an inner node splits too, and the middle separator moves up") {
    BPlusTree tree(3);
    for (std::int64_t k = 1; k <= 9; ++k) tree.insert(i(k), static_cast<std::size_t>(k));
    REQUIRE(tree.height() == 2);  // the root holds 3, 5, 7: full but not over
    tree.insert(i(10), 10);       // a fourth separator, 9, overfills the root
    tree.check();
    REQUIRE(tree.height() == 3);
    REQUIRE(tree.to_text() ==
            "[7:7]\n"
            "  [3:3 | 5:5]\n"
            "    (1:1 2:2)\n"
            "    (3:3 4:4)\n"
            "    (5:5 6:6)\n"
            "  [9:9]\n"
            "    (7:7 8:8)\n"
            "    (9:9 10:10)\n");
}

TEST_CASE("b+ tree: keys inserted in sorted, reverse and random order are all found") {
    for (std::size_t width : {3u, 4u, 5u, 16u}) {
        for (int order = 0; order < 3; ++order) {
            std::vector<std::int64_t> keys(500);
            std::iota(keys.begin(), keys.end(), 0);
            if (order == 1) std::reverse(keys.begin(), keys.end());
            if (order == 2) std::shuffle(keys.begin(), keys.end(), std::mt19937_64(width));
            BPlusTree tree(width);
            for (std::int64_t k : keys) tree.insert(i(k), static_cast<std::size_t>(k) + 1000);
            INFO("width " << width << ", order " << order);
            REQUIRE_NOTHROW(tree.check());
            REQUIRE(tree.size() == 500);
            for (std::int64_t k = 0; k < 500; ++k)
                REQUIRE(tree.lookup(i(k)) == std::vector<std::size_t>{static_cast<std::size_t>(k) + 1000});
            REQUIRE(tree.lookup(i(500)).empty());
            REQUIRE(tree.lookup(i(-1)).empty());
        }
    }
}

TEST_CASE("b+ tree: a key that occurs many times is found whole, across leaves, in row order") {
    BPlusTree tree(4);
    std::mt19937_64 rng(3);
    std::map<std::int64_t, std::vector<std::size_t>> expected;
    for (std::size_t row = 0; row < 600; ++row) {
        auto key = static_cast<std::int64_t>(rng() % 7);
        tree.insert(i(key), row);
        expected[key].push_back(row);
    }
    tree.check();
    for (const auto& [key, rows] : expected) REQUIRE(tree.lookup(i(key)) == rows);
}

TEST_CASE("b+ tree: 100,000 random inserts all look up correctly") {
    BPlusTree tree;
    std::mt19937_64 rng(11);
    std::map<std::int64_t, std::vector<std::size_t>> expected;
    for (std::size_t row = 0; row < 100000; ++row) {
        auto key = static_cast<std::int64_t>(rng() % 50000);
        tree.insert(i(key), row);
        expected[key].push_back(row);
    }
    REQUIRE_NOTHROW(tree.check());
    REQUIRE(tree.size() == 100000);
    for (const auto& [key, rows] : expected) REQUIRE(tree.lookup(i(key)) == rows);
    REQUIRE(tree.height() >= 3);
}

TEST_CASE("b+ tree: INT and DOUBLE keys compare as numbers, text by bytes") {
    BPlusTree numbers(3);
    for (std::size_t r = 0; r < 20; ++r) numbers.insert(Value(static_cast<double>(r)), r);
    REQUIRE(numbers.lookup(i(7)) == std::vector<std::size_t>{7});
    BPlusTree words(3);
    for (const char* w : {"pear", "apple", "fig", "plum", "kiwi", "date"}) words.insert(Value(std::string(w)), words.size());
    REQUIRE(words.lookup(Value(std::string("fig"))) == std::vector<std::size_t>{2});
    words.check();
}
