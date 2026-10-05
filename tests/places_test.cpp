#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>

#include "stats/q_error.h"

#include "engine/datagen.h"

using namespace cardinal;
using Catch::Approx;

namespace {

Database places(const PlacesOptions& options) {
    Database db;
    generate_places(db, options);
    db.execute("ANALYZE people");
    return db;
}

size_t count(Database& db, const std::string& condition) {
    return db.execute("SELECT id FROM people WHERE " + condition).rows.size();
}

ExplainAnalyzeOutput run(Database& db, const std::string& condition) {
    return db.explain_analyze("SELECT id FROM people WHERE " + condition);
}

double q_error_of(Database& db, const std::string& condition) {
    ExplainAnalyzeOutput out = run(db, condition);
    return q_error(out.root_estimate, static_cast<double>(out.root_actual));
}

}  // namespace

TEST_CASE("places: sizes and the same rows for the same options") {
    Database a = places({.rows = 500, .seed = 3});
    Database b = places({.rows = 500, .seed = 3});
    Database c = places({.rows = 500, .seed = 4});
    REQUIRE(a.execute("SELECT id FROM people").rows.size() == 500);
    REQUIRE(a.execute("SELECT * FROM people").rows == b.execute("SELECT * FROM people").rows);
    REQUIRE(a.execute("SELECT * FROM people").rows != c.execute("SELECT * FROM people").rows);
}

TEST_CASE("places: when related, the city decides the country") {
    Database db = places({.rows = 6000, .related = true});
    REQUIRE(count(db, "city = 'Accra'") > 0);
    REQUIRE(count(db, "city = 'Accra' AND country <> 'Ghana'") == 0);
    REQUIRE(count(db, "city = 'Lagos' AND country <> 'Nigeria'") == 0);
    REQUIRE(count(db, "city = 'Kigali' AND country <> 'Rwanda'") == 0);
    REQUIRE(count(db, "country = 'Ghana'") == count(db, "city = 'Accra'") + count(db, "city = 'Kumasi'") + count(db, "city = 'Tamale'"));
}

TEST_CASE("places: when not related, the country has nothing to do with the city") {
    Database db = places({.rows = 6000, .related = false});
    // every city turns up with other countries, in about the usual share (a tenth)
    for (const char* country : {"Ghana", "Nigeria", "Kenya", "Rwanda"}) {
        size_t together = count(db, std::string("city = 'Accra' AND country = '") + country + "'");
        size_t accra = count(db, "city = 'Accra'");
        REQUIRE(together > accra / 20);
        REQUIRE(together < accra / 5);
    }
}

TEST_CASE("places: city skew makes the first city the biggest") {
    Database db = places({.rows = 6000, .city_skew = 1.0});
    size_t accra = count(db, "city = 'Accra'");
    REQUIRE(accra > 6000 / 6);                    // 1 / H(30) is about a quarter
    REQUIRE(accra > 5 * count(db, "city = 'Kano'"));
    REQUIRE(count(db, "city = 'Musanze'") > 0);   // even the smallest city has people
}

TEST_CASE("places: tier is skewed whatever the shape, and age is spread evenly") {
    Database db = places({.rows = 6000});
    REQUIRE(count(db, "tier = 1") > 6000 / 5);
    REQUIRE(count(db, "tier = 1") > 10 * count(db, "tier = 30"));
    REQUIRE(count(db, "age < 18 OR age > 77") == 0);
    size_t over_47 = count(db, "age > 47");
    REQUIRE(over_47 > 6000 * 45 / 100);
    REQUIRE(over_47 < 6000 * 55 / 100);
}

// ---- E5: where the guesses go wrong -------------------------------------------------------
//
// These pin the q-errors of the current estimator, which treats conditions as independent. They
// are meant to stay bad. If a change makes one better, even by accident, the test fails: look
// at why, and if it is deliberate (say, statistics over pairs of columns), update the numbers
// here and the E5 write-up in docs/benchmarks.md together.

TEST_CASE("E5: independent columns are guessed well") {
    Database db = places({.rows = 30000, .related = false});
    REQUIRE(q_error_of(db, "city = 'Accra'") < 1.1);
    REQUIRE(q_error_of(db, "country = 'Ghana'") < 1.1);
    REQUIRE(q_error_of(db, "city = 'Accra' AND country = 'Ghana'") < 1.3);
    REQUIRE(q_error_of(db, "age > 40 AND city = 'Lagos'") < 1.2);
}

TEST_CASE("E5: related columns break the independence assumption") {
    Database db = places({.rows = 30000, .related = true});
    // Each column alone is guessed right...
    REQUIRE(q_error_of(db, "city = 'Accra'") < 1.1);
    REQUIRE(q_error_of(db, "country = 'Ghana'") < 1.1);

    // ...but Accra is only in Ghana, so the second condition removes nothing. Multiplying
    // 1/30 by 1/10 leaves a tenth of the real count.
    ExplainAnalyzeOutput both = run(db, "city = 'Accra' AND country = 'Ghana'");
    REQUIRE(both.root_actual == static_cast<std::int64_t>(count(db, "city = 'Accra'")));
    REQUIRE(both.root_estimate < both.root_actual / 8.0);
    REQUIRE(q_error_of(db, "city = 'Accra' AND country = 'Ghana'") == Approx(10.4).margin(0.6));
    REQUIRE(q_error_of(db, "city = 'Accra' AND country = 'Ghana'") > 8.0);
}

TEST_CASE("E5: a combination that cannot happen is guessed as a crowd") {
    Database db = places({.rows = 30000, .related = true});
    ExplainAnalyzeOutput nobody = run(db, "city = 'Accra' AND country = 'Nigeria'");
    REQUIRE(nobody.root_actual == 0);
    REQUIRE(nobody.root_estimate > 50);  // about 94 rows guessed for none
    REQUIRE(q_error_of(db, "city = 'Accra' AND country = 'Nigeria'") == Approx(94.5).margin(5.0));
}

TEST_CASE("E5: the same mistake is much worse when one city dominates") {
    Database db = places({.rows = 30000, .related = true, .city_skew = 1.0});
    // Accra is a quarter of the table and Nigeria is the other big country, so the guess is a
    // large number of people who do not exist.
    REQUIRE(q_error_of(db, "city = 'Accra' AND country = 'Nigeria'") > 500);
    // while a related pair that is a big part of the table is hurt less than in the even case
    REQUIRE(q_error_of(db, "city = 'Accra' AND country = 'Ghana'") == Approx(2.2).margin(0.3));
}

TEST_CASE("E5: skew by itself is handled, apart from the long tail") {
    Database db = places({.rows = 30000, .related = false, .city_skew = 1.0});
    REQUIRE(q_error_of(db, "tier = 1") < 1.05);       // a common value is exact
    REQUIRE(q_error_of(db, "tier > 10") < 1.05);      // common values and the histogram
    // A value in the tail gets an equal slice of what the common values leave, but the tail
    // itself slopes, so a value deep in it is guessed about twice too high.
    REQUIRE(q_error_of(db, "tier = 40") == Approx(1.9).margin(0.3));
}
