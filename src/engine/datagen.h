#pragma once

#include <cstdint>

#include "engine/database.h"

namespace cardinal {

struct DataOptions {
    int users = 1000;
    int orders_per_user = 4;  // orders = users * orders_per_user
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

}  // namespace cardinal
