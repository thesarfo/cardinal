// cardinal_bench e1 [--sizes 500,1000,2000] [--shares 1,10,50] [--runs 5] [--seed 1]
// cardinal_bench e5 [--rows 30000] [--seed 1]
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
    std::printf("# statistics: ANALYZE users and ANALYZE orders before timing; est_cost is empty until the cost model (M6)\n");
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
                std::fprintf(stderr, "warning: pushdown changed nothing for %s at %d users\n", query_id.c_str(), users);

            for (auto& [variant, cell] : {std::pair<const char*, Cell&>{"pushdown_on", on}, {"pushdown_off", off}}) {
                std::printf("e1,%s,%s,%d,%s,,%.0f,%zu,%.2f,%llu,%.3f,%.3f,%.3f,%.3f,%s,%llu\n", query_id.c_str(), variant, users,
                            cell.first.stats.plan_hash.c_str(), cell.analysis.root_estimate, cell.first.rows.size(),
                            cell.analysis.max_q_error,
                            static_cast<unsigned long long>(cell.first.stats.rows_processed), median(cell.plan_ms),
                            median(cell.exec_ms), *std::min_element(cell.exec_ms.begin(), cell.exec_ms.end()),
                            *std::max_element(cell.exec_ms.begin(), cell.exec_ms.end()), commit.c_str(),
                            static_cast<unsigned long long>(opt.seed));
            }
            std::fflush(stdout);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string experiment = argc > 1 ? argv[1] : "";
    if (experiment != "e1" && experiment != "e5") {
        std::fprintf(stderr,
                     "usage: cardinal_bench e1 [--sizes 500,1000,2000] [--shares 1,10,50] [--runs 5] [--seed 1]\n"
                     "       cardinal_bench e5 [--rows 30000] [--seed 1]\n");
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
    return experiment == "e5" ? run_e5(opt) : run_e1(opt);
}
