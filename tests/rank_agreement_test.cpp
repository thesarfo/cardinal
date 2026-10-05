#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>

#include "engine/database.h"
#include "engine/datagen.h"

using namespace cardinal;

// Does the plan with the lowest estimated cost also do the least work?
//
// Timing is for bench/analyze_calibration.py: a stopwatch is too noisy for a test. This uses the
// work counter instead (the rows every operator hands up plus the row pairs each join tests),
// which gives the same answer every run. It checks the model and the row-count guesses together:
// the model prices each way to run a join from the estimator's guesses, and the work counter
// says what really happened.

namespace {

struct Method {
    const char* name;
    PlannerOptions options;
    const char* option_name;
};

const Method kMethods[] = {
    {"nested loop", PlannerOptions{JoinMethod::NestedLoop}, "NestedLoopJoin"},
    {"hash, build right", PlannerOptions{JoinMethod::Hash, false}, "HashJoin (build right)"},
    {"hash, build left", PlannerOptions{JoinMethod::Hash, true}, "HashJoin (build left)"},
};

struct Outcome {
    std::string cell;
    double cost[3];
    double work[3];
};

std::vector<Outcome> run_grid() {
    std::vector<Outcome> outcomes;
    for (int users : {5, 20, 60, 150, 400}) {
        for (int per_user : {1, 4}) {
            Database db;
            generate_users_orders(db, {.users = users, .orders_per_user = per_user});
            db.execute("ANALYZE users");
            db.execute("ANALYZE orders");
            for (int share : {2, 20, 100}) {
                std::string sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > " +
                                  std::to_string(1000 - 10 * share);
                CostBasedPlan priced = db.cost_of(sql);
                const StepOptions* join = nullptr;
                const PlanOption* chosen = nullptr;
                for (const StepOptions& step : priced.trace)
                    for (const PlanOption& o : step.options)
                        if (o.name.find("Join") != std::string::npos) join = &step;
                for (const PlanOption& o : join->options)
                    if (o.chosen) chosen = &o;

                Outcome outcome;
                outcome.cell = "u" + std::to_string(users) + "-k" + std::to_string(per_user) + "-s" + std::to_string(share);
                for (int m = 0; m < 3; ++m) {
                    for (const PlanOption& o : join->options)
                        if (o.name == kMethods[m].option_name) outcome.cost[m] = priced.cost.total - chosen->cost.total + o.cost.total;
                    db.set_planner_options(kMethods[m].options);
                    outcome.work[m] = static_cast<double>(db.execute(sql).stats.rows_processed);
                    db.use_cost_based_planner();
                }
                outcomes.push_back(outcome);
            }
        }
    }
    return outcomes;
}

}  // namespace

TEST_CASE("rank agreement: the cheapest-priced way to run a join is usually the one that does the least work") {
    std::vector<Outcome> grid = run_grid();

    int cheapest_is_least_work = 0, pairs = 0, pairs_agree = 0;
    for (const Outcome& o : grid) {
        int cheapest = 0, least = 0;
        for (int m = 1; m < 3; ++m) {
            if (o.cost[m] < o.cost[cheapest]) cheapest = m;
            if (o.work[m] < o.work[least]) least = m;
        }
        cheapest_is_least_work += o.work[cheapest] == o.work[least];
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b) {
                if (o.work[a] == o.work[b] || o.cost[a] == o.cost[b]) continue;
                ++pairs;
                pairs_agree += (o.cost[a] < o.cost[b]) == (o.work[a] < o.work[b]);
            }
    }
    double top1 = static_cast<double>(cheapest_is_least_work) / static_cast<double>(grid.size());
    double ordered = static_cast<double>(pairs_agree) / pairs;
    INFO(grid.size() << " queries: the cheapest-priced method did the least work in " << cheapest_is_least_work << " ("
                     << 100 * top1 << "%), and " << pairs_agree << " of " << pairs << " pairs were ordered right ("
                     << 100 * ordered << "%)");
    WARN("rank agreement on work done: " << cheapest_is_least_work << " of " << grid.size() << " queries ("
                                         << std::lround(100 * top1) << "%), " << pairs_agree << " of " << pairs
                                         << " pairs ordered right (" << std::lround(100 * ordered) << "%)");

    // PLAN.md: below about 70% agreement, suspect the estimator before the formulas.
    REQUIRE(top1 >= 0.7);
    REQUIRE(ordered >= 0.8);
}

TEST_CASE("rank agreement: the planner never picks a method that does vastly more work than the best") {
    for (const Outcome& o : run_grid()) {
        int cheapest = 0;
        double least = o.work[0];
        for (int m = 1; m < 3; ++m) {
            if (o.cost[m] < o.cost[cheapest]) cheapest = m;
            least = std::min(least, o.work[m]);
        }
        INFO(o.cell << ": picked " << kMethods[cheapest].name << " with " << o.work[cheapest] << " work, best was " << least);
        REQUIRE(o.work[cheapest] <= 3 * least);
    }
}
