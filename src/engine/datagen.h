#pragma once

#include <cstdint>

#include "engine/database.h"

namespace cardinal {

struct DataOptions {
    int users = 1000;
    int orders_per_user = 4;  // orders = users * orders_per_user...
    int orders = -1;          // ...unless this is set, which gives the number of orders directly
    // 0 spreads orders evenly over users. Above 0 it is a Zipf exponent: user 1 gets the
    // most orders, user 2 about 1/2^skew as many, and so on. 1.0 is a typical heavy skew.
    double skew = 0;
    // The share of orders whose user_id points at a real user. The rest point past the
    // last user, so they join with nothing.
    double match_rate = 1.0;
    uint64_t seed = 1;
};

// Creates and fills two tables. The same options always give the same rows.
//   users(id INT, name TEXT, country TEXT, age INT)
//     id is 1..users, country is one of 20 names chosen evenly, age is 18..77.
//   orders(id INT, user_id INT, amount DOUBLE)
//     id is 1..orders, amount is spread evenly over 0..1000 with two decimals.
// So `country = 'Ghana'` keeps about 5% of users and `amount > 900` keeps about 10% of orders.
void generate_users_orders(Database& db, const DataOptions& options);

struct PlacesOptions {
    int rows = 20000;
    // true: a row's country is the one its city is in, so city decides country.
    // false: the country is picked on its own, with no link to the city.
    bool related = true;
    // 0 gives every city the same number of people. Above 0 it is a Zipf exponent: the first
    // city is the biggest.
    double city_skew = 0;
    // Zipf exponent for `tier`, a column where a few of its 50 values hold most of the rows.
    double tier_skew = 1.2;
    uint64_t seed = 1;
};

// Creates and fills one table, for testing how well row counts are guessed when conditions
// are not independent:
//   people(id INT, city TEXT, country TEXT, age INT, tier INT)
// There are 30 cities, three in each of 10 countries (Accra, Kumasi and Tamale are in Ghana,
// Lagos, Abuja and Kano are in Nigeria, and so on). With `related`, `city = 'Accra'`
// always comes with `country = 'Ghana'`. Age is spread evenly over 18 to 77, and does not
// depend on anything else.
void generate_places(Database& db, const PlacesOptions& options);

}  // namespace cardinal
