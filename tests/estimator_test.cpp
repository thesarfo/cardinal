#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "binder/binder.h"
#include "engine/database.h"
#include "sql/parser.h"
#include "stats/estimator.h"

using namespace cardinal;
using Catch::Approx;

namespace {

// A table of 2000 rows built without randomness, so every number below is reproducible:
//   a     1 to 1000, each value twice          (evenly spread)
//   s     value k appears 600 / k times        (skewed: a few values dominate), then padded with 1000+
//   b     0 to 9 in a pattern unrelated to a   (independent of a)
//   n     NULL in 20% of rows, otherwise 0 to 4
//   city  Accra 50%, Kumasi 30%, Tamale 15%, Wa 5%
//   f     TRUE in 70% of rows, FALSE in 20%, NULL in 10%
struct Fixture {
    Database db;
    Scope scope;
    static constexpr int kRows = 2000;

    Fixture() {
        db.execute("CREATE TABLE t (a INT, s INT, b INT, n INT, city TEXT, f BOOL)");
        std::vector<int> skewed;
        for (int k = 1; k <= 60; ++k)
            for (int c = 0; c < 600 / k; ++c) skewed.push_back(k);
        for (int pad = 1000; static_cast<int>(skewed.size()) < kRows; ++pad) skewed.push_back(pad);
        skewed.resize(kRows);

        std::vector<Row> rows;
        for (int i = 0; i < kRows; ++i) {
            int a = i / 2 + 1;
            int b = static_cast<int>((static_cast<long>(i) * 7919) % 10);
            Value n = i % 5 == 0 ? Value() : Value(std::int64_t{(i * 31) % 5});
            const char* city = i % 20 < 10 ? "Accra" : i % 20 < 16 ? "Kumasi" : i % 20 < 19 ? "Tamale" : "Wa";
            Value f = i % 10 == 9 ? Value() : Value(i % 10 < 7);
            rows.push_back({Value(std::int64_t{a}), Value(std::int64_t{skewed[static_cast<std::size_t>(i)]}),
                            Value(std::int64_t{b}), n, Value(std::string(city)), f});
        }
        db.table("t")->insert_rows(std::move(rows));
        scope.add("t", db.table("t")->info());
    }

    void analyze() { db.execute("ANALYZE t"); }

    BoundExprPtr bind(const std::string& condition) const {
        return bind_expression(*parse_expression(condition), scope);
    }

    // The guess, as a number of rows.
    double estimate(const std::string& condition) const {
        StatsLookup lookup = StatsLookup::for_scope(scope, db.catalog());
        return estimate_selectivity(*bind(condition), lookup) * kRows;
    }
    double estimate_fraction(const std::string& condition) const { return estimate(condition) / kRows; }

    // The truth.
    double actual(const std::string& condition) {
        return static_cast<double>(db.execute("SELECT a FROM t WHERE " + condition).rows.size());
    }

    // How far off the guess is, as a share of the whole table.
    double error(const std::string& condition) { return std::fabs(estimate(condition) - actual(condition)) / kRows; }
};

}  // namespace

TEST_CASE("equals: a common value uses its recorded share, so it is exact") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("s = 1") == Approx(600));
    REQUIRE(f.actual("s = 1") == 600);
    REQUIRE(f.estimate("s = 2") == Approx(300));
    REQUIRE(f.estimate("city = 'Accra'") == Approx(1000));
    REQUIRE(f.estimate("city = 'Kumasi'") == Approx(600));
    REQUIRE(f.estimate("city = 'Tamale'") == Approx(300));
    // 'Wa' appears more than once, so it is in the list too.
    REQUIRE(f.estimate("city = 'Wa'") == Approx(100));
}

TEST_CASE("equals: any other value gets an equal slice of what the common values leave") {
    Fixture f;
    f.analyze();
    // a has 1000 values, each twice. Ten of them are in the common list, with 2 rows each.
    // The rest: (2000 - 20 common rows) / 990 remaining values = 2 rows each.
    REQUIRE(f.estimate("a = 500") == Approx(2));
    REQUIRE(f.actual("a = 500") == 2);
    REQUIRE(f.error("a = 777") < 0.001);
    // s: a value past the first 10 is one of many with fewer rows
    REQUIRE(f.error("s = 40") < 0.01);
}

TEST_CASE("equals: a value outside the column's range has no rows") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("a = 5000") == Approx(0));
    REQUIRE(f.estimate("a = 0") == Approx(0));
    REQUIRE(f.estimate("city = 'Zzz'") == Approx(0));
}

TEST_CASE("not equals: everything that is not NULL and not the value") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("s <> 1") == Approx(1400));
    REQUIRE(f.actual("s <> 1") == 1400);
    REQUIRE(f.error("n <> 2") < 0.001);  // NULLs are not "not equal" either
}

TEST_CASE("ranges on evenly spread data stay within 2% of the table") {
    Fixture f;
    f.analyze();
    for (int cut = 50; cut <= 950; cut += 50) {
        for (const char* op : {"<", "<=", ">", ">="}) {
            std::string condition = std::string("a ") + op + " " + std::to_string(cut);
            INFO(condition);
            REQUIRE(f.error(condition) < 0.02);
        }
    }
}

TEST_CASE("ranges on skewed data use the common values and the histogram together") {
    Fixture f;
    f.analyze();
    for (int cut : {2, 5, 11, 20, 35, 60, 500}) {
        for (const char* op : {"<", "<=", ">", ">="}) {
            std::string condition = std::string("s ") + op + " " + std::to_string(cut);
            INFO(condition);
            REQUIRE(f.error(condition) < 0.04);
        }
    }
}

TEST_CASE("ranges: a cut outside the data keeps all or none of it") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("a > 1000") == Approx(0));
    REQUIRE(f.estimate("a < 1") == Approx(0));
    REQUIRE(f.estimate("a >= 1") == Approx(2000));
    REQUIRE(f.estimate("a <= 1000") == Approx(2000));
    REQUIRE(f.estimate("a > 5000") == Approx(0));
    REQUIRE(f.estimate("a < 5000") == Approx(2000));
}

TEST_CASE("a literal on the left means the same as on the right") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("300 < a") == f.estimate("a > 300"));
    REQUIRE(f.estimate("300 >= a") == f.estimate("a <= 300"));
    REQUIRE(f.estimate("1 = s") == f.estimate("s = 1"));
}

TEST_CASE("comparing with NULL keeps nothing") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("a = NULL") == Approx(0));
    REQUIRE(f.estimate("a < NULL") == Approx(0));
    REQUIRE(f.estimate("NULL") == Approx(0));
}

TEST_CASE("IS NULL comes straight from the NULL share") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("n IS NULL") == Approx(400));
    REQUIRE(f.actual("n IS NULL") == 400);
    REQUIRE(f.estimate("n IS NOT NULL") == Approx(1600));
    REQUIRE(f.estimate("a IS NULL") == Approx(0));
    REQUIRE(f.estimate("f IS NULL") == Approx(200));
}

TEST_CASE("NULLs are left out of every comparison on that column") {
    Fixture f;
    f.analyze();
    // 20% of n is NULL, so even "everything" is only 80% of the rows.
    REQUIRE(f.error("n >= 0") < 0.001);
    REQUIRE(f.error("n < 100") < 0.001);
    REQUIRE(f.estimate("n >= 0") == Approx(1600));
}

TEST_CASE("AND multiplies the shares of unrelated conditions") {
    Fixture f;
    f.analyze();
    // a < 1000 keeps half, b < 5 keeps about half: a quarter
    REQUIRE(f.estimate_fraction("a <= 1000 AND b < 5") == f.estimate_fraction("a <= 1000") * f.estimate_fraction("b < 5"));
    REQUIRE(f.error("a <= 500 AND b < 5") < 0.03);
    REQUIRE(f.error("a > 1500 AND b >= 2 AND n >= 1") < 0.03);
}

TEST_CASE("OR adds the shares and takes off the overlap") {
    Fixture f;
    f.analyze();
    double a = f.estimate_fraction("a < 400");
    double b = f.estimate_fraction("b >= 8");
    REQUIRE(f.estimate_fraction("a < 400 OR b >= 8") == a + b - a * b);
    REQUIRE(f.error("a < 400 OR b >= 8") < 0.03);
}

TEST_CASE("OR of equalities on one column just adds, since a row cannot be both") {
    Fixture f;
    f.analyze();
    // s = 1 is 30% of rows and s = 2 is 15%: 45% together, not 30 + 15 - 4.5
    REQUIRE(f.estimate("s = 1 OR s = 2") == Approx(900));
    REQUIRE(f.actual("s = 1 OR s = 2") == 900);
    REQUIRE(f.estimate("s IN (1, 2)") == Approx(900));
    REQUIRE(f.estimate("city IN ('Accra', 'Kumasi', 'Tamale')") == Approx(1900));
    REQUIRE(f.error("a IN (5, 6, 7, 8)") < 0.005);
}

TEST_CASE("NOT is one minus the share") {
    Fixture f;
    f.analyze();
    // The binder turns NOT into the opposite comparison; NOT on a bare column is the case that remains.
    REQUIRE(f.estimate_fraction("NOT f") == 1.0 - f.estimate_fraction("f"));
}

TEST_CASE("BETWEEN is one range, not two conditions multiplied") {
    Fixture f;
    f.analyze();
    REQUIRE(f.error("a BETWEEN 300 AND 600") < 0.02);
    REQUIRE(f.error("a >= 300 AND a <= 600") < 0.02);
    REQUIRE(f.error("s BETWEEN 5 AND 40") < 0.04);
    // Multiplied (0.70 x 0.60 = 0.42) it would be far off the real 0.30.
    REQUIRE(f.estimate_fraction("a BETWEEN 300 AND 600") < 0.35);
    // Backwards range: nothing
    REQUIRE(f.estimate("a BETWEEN 600 AND 300") == Approx(0));
    // Two ranges on the same column that do not overlap
    REQUIRE(f.estimate("a > 800 AND a < 200") == Approx(0));
}

TEST_CASE("a true/false column used as a condition") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("f") == Approx(1400));
    REQUIRE(f.actual("f") == 1400);
    REQUIRE(f.estimate("f = TRUE") == Approx(1400));
    REQUIRE(f.estimate("f = FALSE") == Approx(400));
}

TEST_CASE("text: common values are exact, ranges place the rest by guess") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate("city < 'Kumasi'") == Approx(1000));                  // 'Accra' is common and below it
    REQUIRE(f.estimate("city > 'Tamale'") == Approx(100));                    // 'Wa'
    REQUIRE(f.estimate("city >= 'Tamale' AND city <= 'Tamale'") == Approx(300));
}

TEST_CASE("without statistics the named fallback numbers are used") {
    Fixture f;  // never analyzed
    REQUIRE(f.estimate_fraction("a = 5") == kDefaultEquality);
    REQUIRE(f.estimate_fraction("a <> 5") == 1.0 - kDefaultEquality);
    REQUIRE(f.estimate_fraction("a < 5") == kDefaultRange);
    REQUIRE(f.estimate_fraction("a >= 5") == kDefaultRange);
    REQUIRE(f.estimate_fraction("a IS NULL") == kDefaultIsNull);
    REQUIRE(f.estimate_fraction("a IS NOT NULL") == 1.0 - kDefaultIsNull);
    REQUIRE(f.estimate_fraction("a = 5 AND b < 3") == kDefaultEquality * kDefaultRange);
    REQUIRE(f.estimate_fraction("a = 5 OR b = 3") == kDefaultEquality + kDefaultEquality - kDefaultEquality * kDefaultEquality);
    REQUIRE(f.estimate_fraction("f") == kDefaultOther);
    // Literal-only conditions still make sense.
    REQUIRE(f.estimate_fraction("TRUE") == 1.0);
    REQUIRE(f.estimate_fraction("FALSE") == 0.0);
}

TEST_CASE("conditions that are not column-against-value use the fallbacks, even with statistics") {
    Fixture f;
    f.analyze();
    REQUIRE(f.estimate_fraction("a = b") == kDefaultEquality);
    REQUIRE(f.estimate_fraction("a < b") == kDefaultRange);
    REQUIRE(f.estimate_fraction("a + 1 > 500") == kDefaultRange);
    REQUIRE(f.estimate_fraction("(a + b) IS NULL") == kDefaultIsNull);
}

TEST_CASE("one column analyzed and another not") {
    Database db;
    db.execute("CREATE TABLE x (v INT)");
    db.execute("CREATE TABLE y (w INT)");
    db.execute("INSERT INTO x VALUES (1), (2), (3), (4)");
    db.execute("INSERT INTO y VALUES (1), (2), (3), (4)");
    db.execute("ANALYZE x");
    Scope scope;
    scope.add("x", db.table("x")->info());
    scope.add("y", db.table("y")->info());
    StatsLookup lookup = StatsLookup::for_scope(scope, db.catalog());
    auto share = [&](const char* condition) {
        return estimate_selectivity(*bind_expression(*parse_expression(condition), scope), lookup);
    };
    // Four values make only three buckets, so the guess is rough; it is clearly not the fallback.
    REQUIRE(share("x.v <= 2") == Approx(0.5).margin(0.1));
    REQUIRE(share("x.v <= 2") != kDefaultRange);
    REQUIRE(share("y.w <= 2") == kDefaultRange);
}

TEST_CASE("every estimate is a fraction between 0 and 1") {
    Fixture f;
    f.analyze();
    const char* conditions[] = {"a < 0 OR a > 5000", "a >= 1 OR s = 1 OR b = 3", "NOT (a < 500 AND b < 5)", "n IS NULL AND n IS NOT NULL",
                                "a BETWEEN 1 AND 1000 AND s BETWEEN 1 AND 1000 AND b BETWEEN 0 AND 9", "s = 1 OR s = 2 OR s = 3 OR s = 4 OR s = 5"};
    for (const char* c : conditions) {
        INFO(c);
        REQUIRE(f.estimate_fraction(c) >= 0.0);
        REQUIRE(f.estimate_fraction(c) <= 1.0);
    }
}
