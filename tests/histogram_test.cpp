#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "engine/database.h"
#include "stats/analyze.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

TableStats analyzed(Database& db, const char* table) {
    db.execute(std::string("ANALYZE ") + table);
    return *db.table(table)->stats();
}

// One INT column v holding the given values.
TableStats ints(const std::vector<std::int64_t>& values) {
    Database db;
    db.execute("CREATE TABLE t (v INT)");
    std::vector<Row> rows;
    for (std::int64_t v : values) rows.push_back({i(v)});
    db.table("t")->insert_rows(std::move(rows));
    return analyzed(db, "t");
}

}  // namespace

TEST_CASE("evenly spread values: no common values, a full histogram of equal-depth buckets") {
    std::vector<std::int64_t> values;
    for (int n = 1; n <= 100; ++n) values.push_back(n);
    TableStats stats = ints(values);
    const ColumnStats& v = stats.columns[0];

    REQUIRE(v.common.empty());  // every value appears once
    REQUIRE(v.histogram.has_value());
    const Histogram& h = *v.histogram;
    REQUIRE(h.buckets() == 32);
    REQUIRE(h.rows == 100);
    REQUIRE(h.bounds.size() == 33);
    REQUIRE(h.bounds.front() == 1);
    REQUIRE(h.bounds.back() == 100);
    REQUIRE(h.bounds[16] == 50);  // the middle bucket edge sits near the middle value
    REQUIRE(std::is_sorted(h.bounds.begin(), h.bounds.end()));

    // The values are 1 to 100, so a bucket's width is how many values it spans: 100 / 32 is 3.1,
    // so every bucket spans 3 or 4.
    for (int b = 0; b < h.buckets(); ++b) {
        double width = h.bounds[static_cast<std::size_t>(b) + 1] - h.bounds[static_cast<std::size_t>(b)];
        REQUIRE(width >= 3);
        REQUIRE(width <= 4);
    }
}

TEST_CASE("skewed values: the common ones are listed with exact counts and left out of the histogram") {
    std::vector<std::int64_t> values;
    for (int n = 0; n < 50; ++n) values.push_back(7);
    for (int n = 0; n < 20; ++n) values.push_back(3);
    for (int n = 0; n < 30; ++n) values.push_back(200 + n);  // thirty different values, once each
    TableStats stats = ints(values);
    const ColumnStats& v = stats.columns[0];

    REQUIRE(v.distinct == 32);
    REQUIRE(v.common.size() == 2);
    REQUIRE(v.common[0].value == i(7));
    REQUIRE(v.common[0].count == 50);
    REQUIRE(v.common[1].value == i(3));
    REQUIRE(v.common[1].count == 20);

    REQUIRE(v.histogram.has_value());
    REQUIRE(v.histogram->rows == 30);
    REQUIRE(v.histogram->buckets() == 29);  // 30 values left: one bucket per gap
    REQUIRE(v.histogram->bounds.front() == 200);
    REQUIRE(v.histogram->bounds.back() == 229);
    // 3 and 7 are not in the histogram's range at all.
    REQUIRE(v.histogram->bounds.front() > 7);
}

TEST_CASE("every value the same: one common value, no histogram") {
    TableStats stats = ints(std::vector<std::int64_t>(40, 5));
    const ColumnStats& v = stats.columns[0];
    REQUIRE(v.distinct == 1);
    REQUIRE(v.common.size() == 1);
    REQUIRE(v.common[0].value == i(5));
    REQUIRE(v.common[0].count == 40);
    REQUIRE_FALSE(v.histogram.has_value());
}

TEST_CASE("common values: at most ten, most frequent first, ties by smaller value") {
    std::vector<std::int64_t> values;
    for (int n = 1; n <= 12; ++n) {
        values.push_back(n);
        values.push_back(n);  // twelve values, each twice
    }
    for (int n = 0; n < 5; ++n) values.push_back(99);  // one that appears five times
    TableStats stats = ints(values);
    const ColumnStats& v = stats.columns[0];

    REQUIRE(v.common.size() == static_cast<std::size_t>(kMaxCommonValues));
    REQUIRE(v.common[0].value == i(99));
    REQUIRE(v.common[0].count == 5);
    // The rest are the nine smallest of the twice-appearing values.
    for (int k = 1; k < 10; ++k) {
        REQUIRE(v.common[static_cast<std::size_t>(k)].value == i(k));
        REQUIRE(v.common[static_cast<std::size_t>(k)].count == 2);
    }
    // 10, 11 and 12 did not make the list, so they are what the histogram describes.
    REQUIRE(v.histogram.has_value());
    REQUIRE(v.histogram->rows == 6);
    REQUIRE(v.histogram->bounds.front() == 10);
    REQUIRE(v.histogram->bounds.back() == 12);
}

TEST_CASE("a value that appears once is never a common value") {
    TableStats stats = ints({1, 2, 2, 3, 4, 5});
    REQUIRE(stats.columns[0].common.size() == 1);
    REQUIRE(stats.columns[0].common[0].value == i(2));
}

TEST_CASE("NULLs are in neither the common list nor the histogram") {
    Database db;
    db.execute("CREATE TABLE t (v INT)");
    db.execute("INSERT INTO t VALUES (NULL), (NULL), (NULL), (1), (1), (2), (3)");
    TableStats stats = analyzed(db, "t");
    const ColumnStats& v = stats.columns[0];
    REQUIRE(v.common.size() == 1);
    REQUIRE(v.common[0].value == i(1));
    REQUIRE(v.common[0].count == 2);
    REQUIRE(v.histogram->rows == 2);  // 2 and 3
}

TEST_CASE("double columns get histograms; text and bool columns get common values only") {
    Database db;
    db.execute("CREATE TABLE t (d DOUBLE, name TEXT, f BOOL)");
    db.execute("INSERT INTO t VALUES (0.5, 'ama', TRUE), (1.5, 'ama', TRUE), (2.5, 'kofi', FALSE), "
               "(3.5, 'esi', TRUE), (4.5, 'kofi', FALSE)");
    TableStats stats = analyzed(db, "t");

    const ColumnStats& d = stats.columns[0];
    REQUIRE(d.common.empty());
    REQUIRE(d.histogram.has_value());
    REQUIRE(d.histogram->buckets() == 4);
    REQUIRE(d.histogram->bounds == std::vector<double>{0.5, 1.5, 2.5, 3.5, 4.5});

    const ColumnStats& name = stats.columns[1];
    REQUIRE(name.common.size() == 2);
    REQUIRE(name.common[0].value == s("ama"));  // ama and kofi both appear twice; ama is smaller
    REQUIRE(name.common[1].value == s("kofi"));
    REQUIRE_FALSE(name.histogram.has_value());

    const ColumnStats& f = stats.columns[2];
    REQUIRE(f.common.size() == 2);
    REQUIRE(f.common[0].value == Value(true));
    REQUIRE(f.common[0].count == 3);
    REQUIRE(f.common[1].value == Value(false));
    REQUIRE_FALSE(f.histogram.has_value());
}

TEST_CASE("empty tables and all-NULL columns have neither") {
    Database db;
    db.execute("CREATE TABLE e (v INT)");
    REQUIRE(analyzed(db, "e").columns[0].common.empty());
    REQUIRE_FALSE(analyzed(db, "e").columns[0].histogram.has_value());

    db.execute("INSERT INTO e VALUES (NULL), (NULL)");
    TableStats stats = analyzed(db, "e");
    REQUIRE(stats.columns[0].common.empty());
    REQUIRE_FALSE(stats.columns[0].histogram.has_value());
}

TEST_CASE("tiny columns: two values make one bucket, one value makes a zero-width bucket") {
    TableStats two = ints({10, 20});
    REQUIRE(two.columns[0].histogram->buckets() == 1);
    REQUIRE(two.columns[0].histogram->bounds == std::vector<double>{10, 20});

    TableStats one = ints({7});
    REQUIRE(one.columns[0].histogram->buckets() == 1);
    REQUIRE(one.columns[0].histogram->bounds == std::vector<double>{7, 7});
}

TEST_CASE("the histogram agrees with the data on a larger skewed table") {
    // Zipf-like: value k appears about 600 / k times, for k up to 60.
    std::vector<std::int64_t> values;
    for (int k = 1; k <= 60; ++k)
        for (int n = 0; n < 600 / k; ++n) values.push_back(k);
    TableStats stats = ints(values);
    const ColumnStats& v = stats.columns[0];

    REQUIRE(v.common.size() == 10);
    REQUIRE(v.common[0].value == i(1));
    REQUIRE(v.common[0].count == 600);
    for (std::size_t k = 1; k < v.common.size(); ++k) REQUIRE(v.common[k - 1].count >= v.common[k].count);

    std::int64_t in_common = 0;
    for (const CommonValue& c : v.common) in_common += c.count;
    REQUIRE(v.histogram->rows == static_cast<std::int64_t>(values.size()) - in_common);
    REQUIRE(v.histogram->bounds.front() == 11);  // 1 to 10 are the ten most common
    REQUIRE(v.histogram->bounds.back() == 60);
}
