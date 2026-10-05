// cardinal_bench e1 [--sizes 500,1000,2000] [--shares 1,10,50] [--runs 5] [--seed 1]
// cardinal_bench e5 [--rows 30000] [--seed 1]
// cardinal_bench calibrate [--runs 7] [--seed 1]
// cardinal_bench e4 [--runs 3] [--seed 1]
//
// E4: when does a hash join beat a nested loop join? The inner (right) table grows from 10 to
// 1,000,000 rows against a left table of 10 or 1000 rows, and each size is run as a nested
// loop, as a hash join building from either side, and as the cost model chooses. Nested loops
// whose pairs would take minutes are skipped.
//
// calibrate: a grid of joins across table sizes and filter selectivities, each run three ways
// (nested loop, hash building left, hash building right), recording the cost model's price
// and the measured time for every one. bench/analyze_calibration.py fits the cost constants
// and reports how often the cheapest plan is also the fastest.
//
// E5: how wrong do the row-count guesses get? Several shapes of data, the same queries on
// each, and the guess against the real count for every one. Prints CSV (est_rows_root,
// actual_rows_root and max_node_q_error are the point).
//
// E1: how much does filter pushdown help? For each table size and each share of orders
// kept by the filter, the demo query is run with every rule on and with just the
// pushdown rule off. Prints CSV on stdout, with a comment header saying what built it.
//
// Rules for numbers you can trust (PLAN.md section 6): release build only, one warm-up
// run, the median of several runs plus the fastest and slowest, a fixed seed, planning
// timed apart from execution, and ANALYZE before timing. est_cost stays empty until the
// cost model exists (M6).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/datagen.h"

namespace {

using cardinal::Database;
using cardinal::QueryResult;

std::string shell_output(const char* command) {
    std::string out;
    if (FILE* pipe = popen(command, "r")) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, pipe)) out += buf;
        pclose(pipe);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out.empty() ? "unknown" : out;
}

std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("model name", 0) == 0) {
            auto colon = line.find(':');
            if (colon != std::string::npos) return line.substr(colon + 2);
        }
    }
    return "unknown";
}

std::vector<int> parse_list(const std::string& text) {
    std::vector<int> out;
    std::stringstream stream(text);
    for (std::string item; std::getline(stream, item, ',');) out.push_back(std::atoi(item.c_str()));
    return out;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

struct Options {
    std::vector<int> sizes{500, 1000, 2000};
    std::vector<int> shares{1, 10, 50};
    int runs = 5;
    int rows = 30000;
    uint64_t seed = 1;
};

struct Cell {
    QueryResult first;
    cardinal::ExplainAnalyzeOutput analysis;  // the guesses against the real counts
    std::vector<double> exec_ms, plan_ms;
};

Cell measure(Database& db, const std::string& sql, int runs) {
    Cell cell;
    db.execute(sql);  // warm-up
    cell.analysis = db.explain_analyze(sql);
    for (int i = 0; i < runs; ++i) {
        QueryResult r = db.execute(sql);
        cell.exec_ms.push_back(r.stats.exec_ms);
        cell.plan_ms.push_back(r.stats.plan_ms);
        if (i == 0) cell.first = std::move(r);
    }
    return cell;
}

int run_e4(const Options& opt) {
    std::printf("# experiment: e4, hash join against nested loop join by the size of the inner table\n");
    std::printf("# query: SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id, users = the outer (left) table, orders = the inner (right) table\n");
    std::printf("# every order has a user, spread evenly; a nested loop whose pairs would pass 2 x 10^8 is skipped\n");
    std::printf("# runs: 1 warm-up, then %d timed; exec_ms is the median\n", opt.runs);
    std::printf("# statistics: ANALYZE before every point; est_cost is the cost model's price for the whole plan\n");
    std::printf("# compiler: %s\n", __VERSION__);
    std::printf("# build: %s, flags: %s\n", CARDINAL_BUILD_TYPE, CARDINAL_FLAGS);
    std::printf("# cpu: %s\n", cpu_model().c_str());
    std::string commit = shell_output("git rev-parse --short HEAD 2>/dev/null");
    std::printf("# commit: %s\n", commit.c_str());
    std::printf("experiment,query_id,variant,table_rows,plan_hash,est_cost,est_rows_root,actual_rows_root,"
                "max_node_q_error,rows_processed,plan_ms,exec_ms,exec_ms_min,exec_ms_max,commit,seed,outer_rows\n");

    struct Method {
        const char* name;
        cardinal::PlannerOptions options;
        const char* option_name;
    };
    const Method methods[] = {
        {"nested_loop", cardinal::PlannerOptions{cardinal::JoinMethod::NestedLoop}, "NestedLoopJoin"},
        {"hash_build_right", cardinal::PlannerOptions{cardinal::JoinMethod::Hash, false}, "HashJoin (build right)"},
        {"hash_build_left", cardinal::PlannerOptions{cardinal::JoinMethod::Hash, true}, "HashJoin (build left)"},
    };
    const std::string sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id";

    for (int outer : {10, 1000}) {
        for (int inner : {10, 100, 1000, 10000, 100000, 1000000}) {
            Database db;
            cardinal::generate_users_orders(db, {.users = outer, .orders = inner, .seed = opt.seed});
            db.execute("ANALYZE users");
            db.execute("ANALYZE orders");
            std::string query_id = "e4-o" + std::to_string(outer) + "-i" + std::to_string(inner);

            cardinal::CostBasedPlan priced = db.cost_of(sql);
            const cardinal::StepOptions* join_step = nullptr;
            const cardinal::PlanOption* chosen = nullptr;
            for (const cardinal::StepOptions& step : priced.trace)
                for (const cardinal::PlanOption& o : step.options)
                    if (o.name.find("Join") != std::string::npos) join_step = &step;
            for (const cardinal::PlanOption& o : join_step->options)
                if (o.chosen) chosen = &o;

            auto record = [&](const std::string& variant, double est, Cell cell) {
                std::printf("e4,%s,%s,%d,%s,%.4f,,%zu,,%llu,%.3f,%.4f,%.4f,%.4f,%s,%llu,%d\n", query_id.c_str(), variant.c_str(),
                            inner, cell.first.stats.plan_hash.c_str(), est, cell.first.rows.size(),
                            static_cast<unsigned long long>(cell.first.stats.rows_processed), median(cell.plan_ms),
                            median(cell.exec_ms), *std::min_element(cell.exec_ms.begin(), cell.exec_ms.end()),
                            *std::max_element(cell.exec_ms.begin(), cell.exec_ms.end()), commit.c_str(),
                            static_cast<unsigned long long>(opt.seed), outer);
                std::fflush(stdout);
            };

            for (const Method& method : methods) {
                double est = std::nan("");
                for (const cardinal::PlanOption& o : join_step->options)
                    if (o.name == method.option_name) est = priced.cost.total - chosen->cost.total + o.cost.total;
                if (method.options.join_method == cardinal::JoinMethod::NestedLoop && double(outer) * inner > 2e8) {
                    std::fprintf(stderr, "skipping the nested loop at %d x %d\n", outer, inner);
                    continue;
                }
                db.set_planner_options(method.options);
                record(method.name, est, measure(db, sql, opt.runs));
            }
            db.use_cost_based_planner();
            record(std::string("cost_based=") + (chosen->name == "NestedLoopJoin" ? "nested_loop"
                                                  : chosen->name == "HashJoin (build left)" ? "hash_build_left" : "hash_build_right"),
                   priced.cost.total, measure(db, sql, opt.runs));
        }
    }
    return 0;
}

int run_calibrate(const Options& opt) {
    std::printf("# experiment: calibration, estimated cost against measured time for joins run three ways\n");
    std::printf("# query: SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > T\n");
    std::printf("# grid: users in 10..1000, orders per user 1 or 4, the filter keeps 2%%, 20%% or 100%% of orders\n");
    std::printf("# runs: 1 warm-up, then %d timed; exec_ms is the median. Joins are forced to each method in turn.\n", opt.runs);
    std::printf("# est_cost is the cost model's price for the whole plan with that method chosen at the join\n");
    std::printf("# statistics: ANALYZE before every cell\n");
    std::printf("# compiler: %s\n", __VERSION__);
    std::printf("# build: %s, flags: %s\n", CARDINAL_BUILD_TYPE, CARDINAL_FLAGS);
    std::printf("# cpu: %s\n", cpu_model().c_str());
    std::string commit = shell_output("git rev-parse --short HEAD 2>/dev/null");
    std::printf("# commit: %s\n", commit.c_str());
    std::printf("experiment,query_id,variant,table_rows,plan_hash,est_cost,est_rows_root,actual_rows_root,"
                "max_node_q_error,rows_processed,plan_ms,exec_ms,exec_ms_min,exec_ms_max,commit,seed,"
                "left_rows,right_rows,pairs,build_rows,probe_rows,join_out_rows,join_self_ms\n");

    struct Method {
        const char* name;
        cardinal::PlannerOptions options;
        const char* option_name;  // as the planner names it
    };
    const Method methods[] = {
        {"nested_loop", cardinal::PlannerOptions{cardinal::JoinMethod::NestedLoop}, "NestedLoopJoin"},
        {"hash_build_right", cardinal::PlannerOptions{cardinal::JoinMethod::Hash, false}, "HashJoin (build right)"},
        {"hash_build_left", cardinal::PlannerOptions{cardinal::JoinMethod::Hash, true}, "HashJoin (build left)"},
    };

    for (int users : {10, 30, 100, 300, 1000}) {
        for (int per_user : {1, 4}) {
            Database db;
            cardinal::generate_users_orders(db, {.users = users, .orders_per_user = per_user, .seed = opt.seed});
            db.execute("ANALYZE users");
            db.execute("ANALYZE orders");
            for (int share : {2, 20, 100}) {
                std::string sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id "
                                  "WHERE o.amount > " + std::to_string(1000 - 10 * share);
                std::string query_id = "grid-u" + std::to_string(users) + "-k" + std::to_string(per_user) + "-s" + std::to_string(share);

                cardinal::CostBasedPlan priced = db.cost_of(sql);
                const cardinal::PlanOption* chosen = nullptr;
                const cardinal::StepOptions* join_step = nullptr;
                for (const cardinal::StepOptions& step : priced.trace)
                    for (const cardinal::PlanOption& o : step.options)
                        if (o.name.find("Join") != std::string::npos) join_step = &step;
                for (const cardinal::PlanOption& o : join_step->options)
                    if (o.chosen) chosen = &o;

                for (const Method& method : methods) {
                    double est = -1;
                    for (const cardinal::PlanOption& o : join_step->options)
                        if (o.name == method.option_name) est = priced.cost.total - chosen->cost.total + o.cost.total;

                    db.set_planner_options(method.options);
                    db.execute(sql);  // warm-up
                    std::vector<double> exec, plan, join_self;
                    QueryResult first;
                    for (int i = 0; i < opt.runs; ++i) {
                        QueryResult r = db.execute(sql);
                        exec.push_back(r.stats.exec_ms);
                        plan.push_back(r.stats.plan_ms);
                        for (const cardinal::OperatorStat& op : r.stats.operators)
                            if (op.name.find("Join") != std::string::npos) join_self.push_back(op.self_ms);
                        if (i == 0) first = std::move(r);
                    }
                    db.use_cost_based_planner();

                    std::uint64_t left = 0, right = 0, pairs = 0, build = 0, probe = 0, out = 0;
                    for (const cardinal::OperatorStat& op : first.stats.operators) {
                        if (op.name.find("Join") == std::string::npos) continue;
                        left = op.input_rows[0];
                        right = op.input_rows[1];
                        out = op.rows_out;
                        pairs = op.rows_scanned;
                        if (op.name == "HashJoin") {
                            build = method.options.build_left ? left : right;
                            probe = method.options.build_left ? right : left;
                        }
                    }
                    if (est < 0) est = std::nan("");
                    std::printf("calibration,%s,%s,%d,%s,%.4f,,%zu,,%llu,%.3f,%.4f,%.4f,%.4f,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.4f\n",
                                query_id.c_str(), method.name, users, first.stats.plan_hash.c_str(), est, first.rows.size(),
                                static_cast<unsigned long long>(first.stats.rows_processed), median(plan), median(exec),
                                *std::min_element(exec.begin(), exec.end()), *std::max_element(exec.begin(), exec.end()), commit.c_str(),
                                static_cast<unsigned long long>(opt.seed), static_cast<unsigned long long>(left),
                                static_cast<unsigned long long>(right), static_cast<unsigned long long>(pairs),
                                static_cast<unsigned long long>(build), static_cast<unsigned long long>(probe),
                                static_cast<unsigned long long>(out), median(join_self));
                }
                std::fflush(stdout);
            }
        }
    }
    return 0;
}

int run_e5(const Options& opt) {
    std::printf("# experiment: e5, how wrong do the row-count guesses get?\n");
    std::printf("# table: people(id, city, country, age, tier), %d rows; 30 cities, 3 in each of 10 countries\n", opt.rows);
    std::printf("# shapes: even = cities equally common, country picked on its own; related = city decides country;\n");
    std::printf("#         skewed = city sizes fall off as 1/rank; tier is skewed (a few values dominate) in every shape\n");
    std::printf("# statistics: ANALYZE people before every query; this experiment times nothing\n");
    std::printf("# compiler: %s\n", __VERSION__);
    std::printf("# build: %s, flags: %s\n", CARDINAL_BUILD_TYPE, CARDINAL_FLAGS);
    std::string commit = shell_output("git rev-parse --short HEAD 2>/dev/null");
    std::printf("# commit: %s\n", commit.c_str());
    std::printf("experiment,query_id,variant,table_rows,plan_hash,est_cost,est_rows_root,actual_rows_root,"
                "max_node_q_error,rows_processed,plan_ms,exec_ms,exec_ms_min,exec_ms_max,commit,seed\n");

    struct Shape {
        const char* name;
        cardinal::PlacesOptions options;
    };
    const Shape shapes[] = {
        {"even", {.rows = opt.rows, .related = false, .city_skew = 0, .seed = opt.seed}},
        {"related", {.rows = opt.rows, .related = true, .city_skew = 0, .seed = opt.seed}},
        {"skewed", {.rows = opt.rows, .related = false, .city_skew = 1.0, .seed = opt.seed}},
        {"related+skewed", {.rows = opt.rows, .related = true, .city_skew = 1.0, .seed = opt.seed}},
    };
    struct Query {
        const char* id;
        const char* condition;
    };
    const Query queries[] = {
        {"city", "city = 'Accra'"},
        {"country", "country = 'Ghana'"},
        {"city-and-country", "city = 'Accra' AND country = 'Ghana'"},
        {"city-and-wrong-country", "city = 'Accra' AND country = 'Nigeria'"},
        {"tier-top", "tier = 1"},
        {"tier-tail", "tier = 40"},
        {"tier-range", "tier > 10"},
        {"age-and-city", "age > 40 AND city = 'Lagos'"},
    };
    for (const Shape& shape : shapes) {
        Database db;
        cardinal::generate_places(db, shape.options);
        db.execute("ANALYZE people");
        for (const Query& query : queries) {
            std::string sql = std::string("SELECT id FROM people WHERE ") + query.condition;
            cardinal::ExplainAnalyzeOutput out = db.explain_analyze(sql);
            std::printf("e5,%s,%s,%d,,,%.0f,%lld,%.2f,,,,,,%s,%llu\n", query.id, shape.name, opt.rows, out.root_estimate,
                        static_cast<long long>(out.root_actual), out.max_q_error, commit.c_str(),
                        static_cast<unsigned long long>(opt.seed));
        }
    }
    return 0;
}

int run_e1(const Options& opt) {
    std::printf("# experiment: e1, filter pushdown on versus off\n");
    std::printf("# query: SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.country = 'Ghana' AND o.amount > T\n");
    std::printf("# data: users = table_rows, orders = 4 x users, even spread, every order has a user; about 5%% of users are in Ghana\n");
    std::printf("# runs: 1 warm-up, then %d timed; exec_ms is the median, with the fastest and slowest\n", opt.runs);
    std::printf("# variants: pushdown_on or pushdown_off, plus the join: nested_loop (forced) or cost_based (the planner's choice)\n");
    std::printf("# statistics: ANALYZE users and ANALYZE orders before timing; est_cost is not recorded for this experiment\n");
    std::printf("# compiler: %s\n", __VERSION__);
    std::printf("# build: %s, flags: %s\n", CARDINAL_BUILD_TYPE, CARDINAL_FLAGS);
    std::printf("# cpu: %s\n", cpu_model().c_str());
    std::printf("# commit: %s\n", shell_output("git rev-parse --short HEAD 2>/dev/null").c_str());
#ifndef NDEBUG
    std::printf("# WARNING: not a release build, so these timings mean little\n");
#endif
    std::printf("experiment,query_id,variant,table_rows,plan_hash,est_cost,est_rows_root,actual_rows_root,"
                "max_node_q_error,rows_processed,plan_ms,exec_ms,exec_ms_min,exec_ms_max,commit,seed\n");
    std::string commit = shell_output("git rev-parse --short HEAD 2>/dev/null");

    for (int users : opt.sizes) {
        Database db;
        cardinal::generate_users_orders(db, {.users = users, .seed = opt.seed});
        db.execute("ANALYZE users");
        db.execute("ANALYZE orders");
        for (int share : opt.shares) {
            std::string sql = "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id "
                              "WHERE u.country = 'Ghana' AND o.amount > " + std::to_string(1000 - 10 * share);
            std::string query_id = "demo-keep" + std::to_string(share);

            // The join is run twice over: forced to a nested loop (as E1 first measured it), and
            // chosen by cost, which now picks a hash join.
            for (bool nested_loop : {true, false}) {
                if (nested_loop) db.set_planner_options(cardinal::PlannerOptions{cardinal::JoinMethod::NestedLoop});
                else db.use_cost_based_planner();
                const char* join = nested_loop ? "nested_loop" : "cost_based";

                Cell on, off;
                db.set_disabled_rules({});
                on = measure(db, sql, opt.runs);
                db.set_disabled_rules({"filter-pushdown"});
                off = measure(db, sql, opt.runs);
                db.set_disabled_rules({});

                if (on.first.rows.size() != off.first.rows.size()) {
                    std::fprintf(stderr, "different row counts with and without pushdown for %s at %d users\n",
                                 query_id.c_str(), users);
                    return 1;
                }
                if (on.first.stats.plan_hash == off.first.stats.plan_hash)
                    std::fprintf(stderr, "warning: pushdown changed nothing for %s at %d users (%s)\n", query_id.c_str(), users, join);

                for (auto& [variant, cell] : {std::pair<const char*, Cell&>{"pushdown_on", on}, {"pushdown_off", off}}) {
                    std::printf("e1,%s,%s+%s,%d,%s,,%.0f,%zu,%.2f,%llu,%.3f,%.3f,%.3f,%.3f,%s,%llu\n", query_id.c_str(), variant,
                                join, users, cell.first.stats.plan_hash.c_str(), cell.analysis.root_estimate, cell.first.rows.size(),
                                cell.analysis.max_q_error,
                                static_cast<unsigned long long>(cell.first.stats.rows_processed), median(cell.plan_ms),
                                median(cell.exec_ms), *std::min_element(cell.exec_ms.begin(), cell.exec_ms.end()),
                                *std::max_element(cell.exec_ms.begin(), cell.exec_ms.end()), commit.c_str(),
                                static_cast<unsigned long long>(opt.seed));
                }
                std::fflush(stdout);
            }
            db.use_cost_based_planner();
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string experiment = argc > 1 ? argv[1] : "";
    if (experiment != "e1" && experiment != "e4" && experiment != "e5" && experiment != "calibrate") {
        std::fprintf(stderr,
                     "usage: cardinal_bench e1 [--sizes 500,1000,2000] [--shares 1,10,50] [--runs 5] [--seed 1]\n"
                     "       cardinal_bench e5 [--rows 30000] [--seed 1]\n"
                     "       cardinal_bench calibrate [--runs 7] [--seed 1]\n"
                     "       cardinal_bench e4 [--runs 3] [--seed 1]\n");
        return 2;
    }
    Options opt;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--sizes") opt.sizes = parse_list(value);
        else if (flag == "--shares") opt.shares = parse_list(value);
        else if (flag == "--runs") opt.runs = std::max(1, std::atoi(value.c_str()));
        else if (flag == "--rows") opt.rows = std::max(1, std::atoi(value.c_str()));
        else if (flag == "--seed") opt.seed = std::strtoull(value.c_str(), nullptr, 10);
        else {
            std::fprintf(stderr, "unknown option %s\n", flag.c_str());
            return 2;
        }
    }
    if (experiment == "calibrate") return run_calibrate(opt);
    if (experiment == "e4") return run_e4(opt);
    return experiment == "e5" ? run_e5(opt) : run_e1(opt);
}
