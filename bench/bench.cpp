// cardinal_bench e1 [--sizes 500,1000,2000] [--shares 1,10,50] [--runs 5] [--seed 1]
//
// E1: how much does filter pushdown help? For each table size and each share of orders
// kept by the filter, the demo query is run with every rule on and with just the
// pushdown rule off. Prints CSV on stdout, with a comment header saying what built it.
//
// Rules for numbers you can trust (PLAN.md section 6): release build only, one warm-up
// run, the median of several runs plus the fastest and slowest, a fixed seed, planning
// timed apart from execution. There are no statistics yet, so there is no ANALYZE.

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
    uint64_t seed = 1;
};

struct Cell {
    QueryResult first;
    std::vector<double> exec_ms, plan_ms;
};

Cell measure(Database& db, const std::string& sql, int runs) {
    Cell cell;
    db.execute(sql);  // warm-up
    for (int i = 0; i < runs; ++i) {
        QueryResult r = db.execute(sql);
        cell.exec_ms.push_back(r.stats.exec_ms);
        cell.plan_ms.push_back(r.stats.plan_ms);
        if (i == 0) cell.first = std::move(r);
    }
    return cell;
}

int run_e1(const Options& opt) {
    std::printf("# experiment: e1, filter pushdown on versus off\n");
    std::printf("# query: SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id WHERE u.country = 'Ghana' AND o.amount > T\n");
    std::printf("# data: users = table_rows, orders = 4 x users, even spread, every order has a user; about 5%% of users are in Ghana\n");
    std::printf("# runs: 1 warm-up, then %d timed; exec_ms is the median, with the fastest and slowest\n", opt.runs);
    std::printf("# statistics: none (ANALYZE does not exist yet); est_* and max_node_q_error are empty\n");
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
                std::printf("e1,%s,%s,%d,%s,,,%zu,,%llu,%.3f,%.3f,%.3f,%.3f,%s,%llu\n", query_id.c_str(), variant, users,
                            cell.first.stats.plan_hash.c_str(), cell.first.rows.size(),
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
    if (argc < 2 || std::string(argv[1]) != "e1") {
        std::fprintf(stderr, "usage: cardinal_bench e1 [--sizes 500,1000,2000] [--shares 1,10,50] [--runs 5] [--seed 1]\n");
        return 2;
    }
    Options opt;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--sizes") opt.sizes = parse_list(value);
        else if (flag == "--shares") opt.shares = parse_list(value);
        else if (flag == "--runs") opt.runs = std::max(1, std::atoi(value.c_str()));
        else if (flag == "--seed") opt.seed = std::strtoull(value.c_str(), nullptr, 10);
        else {
            std::fprintf(stderr, "unknown option %s\n", flag.c_str());
            return 2;
        }
    }
    return run_e1(opt);
}
