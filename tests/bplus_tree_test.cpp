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

TEST_CASE("b+ tree: range honours open and closed ends, across leaves") {
    BPlusTree tree(3);
    for (std::int64_t k = 1; k <= 20; ++k) tree.insert(i(k), static_cast<std::size_t>(k * 10));
    auto b = [](std::int64_t v, bool inclusive) { return std::optional<Bound>(Bound{Value(v), inclusive}); };
    REQUIRE(tree.range(b(3, true), b(6, true)) == std::vector<std::size_t>{30, 40, 50, 60});
    REQUIRE(tree.range(b(3, false), b(6, false)) == std::vector<std::size_t>{40, 50});
    REQUIRE(tree.range(b(19, true), std::nullopt) == std::vector<std::size_t>{190, 200});
    REQUIRE(tree.range(std::nullopt, b(2, false)) == std::vector<std::size_t>{10});
    REQUIRE(tree.range(std::nullopt, std::nullopt).size() == 20);
    REQUIRE(tree.range(b(6, true), b(3, true)).empty());
    REQUIRE(tree.range(b(25, true), std::nullopt).empty());
    REQUIRE(tree.range(b(0, true), b(0, true)).empty());
}

TEST_CASE("b+ tree: erase removes one entry and reports a missing one") {
    BPlusTree tree(4);
    for (std::size_t r = 0; r < 10; ++r) tree.insert(i(static_cast<std::int64_t>(r % 3)), r);
    REQUIRE(tree.erase(i(1), 4));
    REQUIRE_FALSE(tree.erase(i(1), 4));
    REQUIRE_FALSE(tree.erase(i(1), 0));  // key present, but not with that row
    REQUIRE_FALSE(tree.erase(i(9), 0));
    REQUIRE(tree.lookup(i(1)) == std::vector<std::size_t>{1, 7});
    REQUIRE(tree.size() == 9);
    tree.check();
}

TEST_CASE("b+ tree: erasing everything leaves an empty, valid tree, in any order") {
    for (int order = 0; order < 3; ++order) {
        BPlusTree tree(4);
        std::vector<std::size_t> rows(300);
        std::iota(rows.begin(), rows.end(), 0);
        for (std::size_t r : rows) tree.insert(i(static_cast<std::int64_t>(r)), r);
        if (order == 1) std::reverse(rows.begin(), rows.end());
        if (order == 2) std::shuffle(rows.begin(), rows.end(), std::mt19937_64(4));
        for (std::size_t r : rows) {
            REQUIRE(tree.erase(i(static_cast<std::int64_t>(r)), r));
            REQUIRE_NOTHROW(tree.check());
        }
        REQUIRE(tree.size() == 0);
        REQUIRE(tree.height() == 1);
        REQUIRE(tree.range(std::nullopt, std::nullopt).empty());
    }
}

TEST_CASE("b+ tree: random inserts and deletes agree with std::multimap") {
    for (std::size_t width : {3u, 4u, 7u, 64u}) {
        BPlusTree tree(width);
        std::multimap<std::int64_t, std::size_t> model;  // entries kept in (key, row) order below
        std::mt19937_64 rng(width * 17);
        std::size_t next_row = 0;
        for (int step = 0; step < 6000; ++step) {
            if (model.empty() || rng() % 100 < 60) {
                auto key = static_cast<std::int64_t>(rng() % 40);
                tree.insert(i(key), next_row);
                model.emplace(key, next_row++);
            } else {
                auto it = model.begin();
                std::advance(it, static_cast<std::ptrdiff_t>(rng() % model.size()));
                REQUIRE(tree.erase(i(it->first), it->second));
                model.erase(it);
            }
            if (step % 500 == 0) REQUIRE_NOTHROW(tree.check());
        }
        tree.check();
        INFO("width " << width);
        REQUIRE(tree.size() == model.size());
        for (std::int64_t key = -1; key <= 41; ++key) {
            std::vector<std::size_t> want;
            for (auto [lo, hi] = model.equal_range(key); lo != hi; ++lo) want.push_back(lo->second);
            std::sort(want.begin(), want.end());
            REQUIRE(tree.lookup(i(key)) == want);
        }
        std::vector<std::size_t> all;
        for (auto& [k, r] : model) all.push_back(r);
        REQUIRE(tree.range(std::nullopt, std::nullopt).size() == all.size());
        auto got = tree.range(Bound{i(10), true}, Bound{i(20), false});
        std::size_t want = 0;
        for (auto& [k, r] : model) want += (k >= 10 && k < 20);
        REQUIRE(got.size() == want);
    }
}
