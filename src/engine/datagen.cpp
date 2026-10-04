#include "engine/datagen.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace cardinal {

namespace {

const char* const kCountries[] = {"Ghana",  "Nigeria", "Kenya",   "Togo",   "Benin",   "Mali",    "Niger",
                                  "Chad",   "Egypt",   "Ethiopia", "Uganda", "Rwanda",  "Zambia",  "Malawi",
                                  "Angola", "Namibia", "Senegal", "Gambia", "Liberia", "Tunisia"};

// std::uniform_* distributions give different numbers on different standard libraries,
// so the draws are done by hand to keep the same seed giving the same data everywhere.
class Random {
public:
    explicit Random(uint64_t seed) : rng_(seed) {}
    uint64_t below(uint64_t n) { return rng_() % n; }
    double unit() { return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0); }

private:
    std::mt19937_64 rng_;
};

}  // namespace

void generate_users_orders(Database& db, const DataOptions& options) {
    db.execute("CREATE TABLE users (id INT, name TEXT, country TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    Table& users = *db.table("users");
    Table& orders = *db.table("orders");

    const int n = options.users;
    Random random(options.seed);

    std::vector<Row> user_rows;
    user_rows.reserve(static_cast<std::size_t>(n));
    for (int id = 1; id <= n; ++id) {
        user_rows.push_back({Value(std::int64_t{id}), Value("user" + std::to_string(id)),
                             Value(std::string(kCountries[random.below(20)])),
                             Value(std::int64_t{18 + static_cast<std::int64_t>(random.below(60))})});
    }
    users.insert_rows(std::move(user_rows));

    // Zipf: the chance of user k is proportional to 1 / k^skew. Pick by walking a running total.
    std::vector<double> running_total;
    if (options.skew > 0) {
        double total = 0;
        for (int k = 1; k <= n; ++k) {
            total += 1.0 / std::pow(static_cast<double>(k), options.skew);
            running_total.push_back(total);
        }
    }
    auto pick_user = [&]() -> std::int64_t {
        if (running_total.empty()) return 1 + static_cast<std::int64_t>(random.below(static_cast<uint64_t>(n)));
        double target = random.unit() * running_total.back();
        auto it = std::lower_bound(running_total.begin(), running_total.end(), target);
        return 1 + (it - running_total.begin());
    };

    const std::int64_t order_count = static_cast<std::int64_t>(n) * options.orders_per_user;
    std::vector<Row> order_rows;
    order_rows.reserve(static_cast<std::size_t>(order_count));
    for (std::int64_t id = 1; id <= order_count; ++id) {
        std::int64_t user = random.unit() < options.match_rate
                                ? pick_user()
                                : n + 1 + static_cast<std::int64_t>(random.below(static_cast<uint64_t>(n) + 1));
        double amount = std::floor(random.unit() * 100000.0) / 100.0;
        order_rows.push_back({Value(id), Value(user), Value(amount)});
    }
    orders.insert_rows(std::move(order_rows));
}

}  // namespace cardinal
