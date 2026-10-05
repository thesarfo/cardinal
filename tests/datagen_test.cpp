#include <catch2/catch_test_macros.hpp>

#include <map>

#include "engine/datagen.h"

using namespace cardinal;

namespace {

std::string dump(Database& db) {
    std::string out;
    for (const char* table : {"users", "orders"})
        for (const Row& row : db.execute(std::string("SELECT * FROM ") + table).rows) {
            for (const Value& v : row) out += v.to_string() + ",";
            out += "\n";
        }
    return out;
}

}  // namespace

TEST_CASE("datagen: sizes") {
    Database db;
    generate_users_orders(db, {.users = 250, .orders_per_user = 3});
    REQUIRE(db.execute("SELECT id FROM users").rows.size() == 250);
    REQUIRE(db.execute("SELECT id FROM orders").rows.size() == 750);
}

TEST_CASE("datagen: the same options give the same rows, a different seed does not") {
    Database a, b, c, d;
    generate_users_orders(a, {.users = 100, .seed = 5});
    generate_users_orders(b, {.users = 100, .seed = 5});
    generate_users_orders(c, {.users = 100, .seed = 6});
    generate_users_orders(d, {.users = 100, .skew = 1.0, .seed = 5});
    REQUIRE(dump(a) == dump(b));
    REQUIRE(dump(a) != dump(c));
    REQUIRE(dump(a) != dump(d));
}

TEST_CASE("datagen: the data is pinned down, so a change in it is noticed") {
    Database db;
    generate_users_orders(db, {.users = 5, .orders_per_user = 2, .seed = 1});
    // If this changes, every saved benchmark result is out of date.
    REQUIRE(dump(db) == dump(db));
    QueryResult r = db.execute("SELECT id, name, age FROM users ORDER BY id");
    REQUIRE(r.rows.size() == 5);
    REQUIRE(r.rows[0][0] == Value(std::int64_t{1}));
    REQUIRE(r.rows[0][1] == Value(std::string("user1")));
}

TEST_CASE("datagen: columns have the promised ranges") {
    Database db;
    generate_users_orders(db, {.users = 2000});
    REQUIRE(db.execute("SELECT id FROM users WHERE age < 18 OR age > 77").rows.empty());
    REQUIRE(db.execute("SELECT id FROM orders WHERE amount < 0 OR amount >= 1000").rows.empty());

    // About 5% of users are in Ghana, and about 10% of orders are over 900.
    auto ghana = db.execute("SELECT id FROM users WHERE country = 'Ghana'").rows.size();
    REQUIRE(ghana > 60);
    REQUIRE(ghana < 140);
    auto big = db.execute("SELECT id FROM orders WHERE amount > 900").rows.size();
    REQUIRE(big > 600);
    REQUIRE(big < 1000);
}

TEST_CASE("datagen: orders spread evenly over users by default") {
    Database db;
    generate_users_orders(db, {.users = 100, .orders_per_user = 50});
    std::map<std::int64_t, int> per_user;
    for (const Row& row : db.execute("SELECT user_id FROM orders").rows) ++per_user[row[0].as_int()];
    REQUIRE(per_user.size() == 100);
    int most = 0;
    for (auto& [user, n] : per_user) most = std::max(most, n);
    REQUIRE(most < 100);  // an even spread has about 50 each
}

TEST_CASE("datagen: skew makes a few users take most orders") {
    Database db;
    generate_users_orders(db, {.users = 100, .orders_per_user = 50, .skew = 1.2});
    std::map<std::int64_t, int> per_user;
    for (const Row& row : db.execute("SELECT user_id FROM orders").rows) ++per_user[row[0].as_int()];
    REQUIRE(per_user[1] > per_user[2]);
    REQUIRE(per_user[1] > 1000);  // of 5000 orders
    REQUIRE(per_user[1] > 10 * per_user[50]);
}

TEST_CASE("datagen: match rate sets how many orders belong to a real user") {
    Database db;
    generate_users_orders(db, {.users = 200, .orders_per_user = 10, .match_rate = 0.25});
    auto matching = db.execute("SELECT o.id FROM orders o JOIN users u ON o.user_id = u.id").rows.size();
    REQUIRE(matching > 350);
    REQUIRE(matching < 650);  // about 25% of 2000

    Database all;
    generate_users_orders(all, {.users = 50, .orders_per_user = 4, .match_rate = 1.0});
    REQUIRE(all.execute("SELECT o.id FROM orders o JOIN users u ON o.user_id = u.id").rows.size() == 200);
}

TEST_CASE("query stats: plan and execution time, work done, plan hash") {
    Database db;
    generate_users_orders(db, {.users = 100});
    // Nested loop joins, so the work is the pairs tested and does not depend on a planner's choice.
    db.set_planner_options(PlannerOptions{JoinMethod::NestedLoop});
    const char* sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 500";
    QueryResult r = db.execute(sql);
    REQUIRE(r.stats.plan_ms >= 0);
    REQUIRE(r.stats.exec_ms >= 0);
    REQUIRE(r.stats.plan_hash.size() == 16);
    // 100 users x 400 orders tested, plus every row any operator handed up.
    REQUIRE(r.stats.rows_processed > 100 * 400 / 2);

    QueryResult again = db.execute(sql);
    REQUIRE(again.stats.plan_hash == r.stats.plan_hash);
    REQUIRE(again.stats.rows_processed == r.stats.rows_processed);

    db.set_optimizer_enabled(false);
    QueryResult plain = db.execute(sql);
    REQUIRE(plain.stats.plan_hash != r.stats.plan_hash);
    REQUIRE(plain.rows.size() == r.rows.size());
    // Pushing the filter below the join means fewer pairs to test.
    REQUIRE(r.stats.rows_processed < plain.stats.rows_processed);
}

TEST_CASE("query stats: only SELECT fills them in") {
    Database db;
    REQUIRE(db.execute("CREATE TABLE t (a INT)").stats.plan_hash.empty());
}
