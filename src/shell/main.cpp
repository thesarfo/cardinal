#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/error.h"
#include "common/version.h"
#include "engine/database.h"
#include "engine/datagen.h"
#include "engine/result_format.h"
#include "sql/parser.h"
#include "stats/analyze.h"

namespace {

bool ends_with_semicolon(const std::string& text) {
    std::size_t last = text.find_last_not_of(" \t\r\n");
    return last != std::string::npos && text[last] == ';';
}

bool is_blank(const std::string& text) { return text.find_first_not_of(" \t\r\n") == std::string::npos; }

void run(cardinal::Database& db, const std::string& sql) {
    try {
        std::cout << cardinal::format_result(db.execute(sql)) << '\n';
    } catch (const cardinal::ParseError& e) {
        std::cout << cardinal::format_error(sql, e) << '\n';
    } catch (const cardinal::DbError& e) {
        std::cout << "error: " << e.what() << '\n';
    }
}

}  // namespace

int main() {
    // Prompts and the banner are for people; piped input gets clean output.
    const bool interactive = isatty(STDIN_FILENO);
    if (interactive) std::cout << "cardinal " << cardinal::version() << "\nType .quit to leave. End statements with ;\n";

    cardinal::Database db;
    std::string pending;  // a statement still waiting for its ';'
    std::string line;
    while (true) {
        if (interactive) std::cout << (pending.empty() ? "cardinal> " : "     ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;

        // Anything that isn't SQL starts with a dot, so it can't clash with SQL.
        if (pending.empty() && !line.empty() && line[0] == '.') {
            if (line == ".quit") return 0;
            if (line == ".bushy on" || line == ".bushy off") {
                db.set_bushy_joins(line == ".bushy on");
                std::cout << "bushy joins " << (db.bushy_joins() ? "on" : "off") << '\n';
                continue;
            }
            // `.demo` or `.demo 5000`: the users and orders tables the experiments use, analyzed.
            if (line == ".demo" || line.rfind(".demo ", 0) == 0) {
                int users = 1000;
                if (line.size() > 6) users = std::max(1, std::atoi(line.c_str() + 6));
                if (db.catalog().get_table("users") || db.catalog().get_table("orders")) {
                    std::cout << "the tables users and orders already exist\n";
                    continue;
                }
                cardinal::generate_users_orders(db, {.users = users});
                db.execute("ANALYZE users");
                db.execute("ANALYZE orders");
                std::cout << "users(id, name, country, age): " << users << " rows\n"
                          << "orders(id, user_id, amount): " << users * 4 << " rows\n"
                          << "both analyzed. Try: EXPLAIN ANALYZE VERBOSE SELECT u.name FROM users u JOIN orders o\n"
                          << "ON u.id = o.user_id WHERE u.country = 'Ghana' AND o.amount > 100;\n";
                continue;
            }
            if (line == ".indexes on" || line == ".indexes off") {
                db.set_use_indexes(line == ".indexes on");
                std::cout << "index scans " << (db.use_indexes() ? "on" : "off") << '\n';
                continue;
            }
            if (line.rfind(".stats ", 0) == 0) {
                std::string name = line.substr(7);
                const cardinal::Table* table = db.catalog().get_table(name);
                if (!table) std::cout << "unknown table " << name << '\n';
                else if (!table->stats()) std::cout << "no statistics for " << name << "; run ANALYZE " << name << ";\n";
                else std::cout << cardinal::format_stats(*table, *table->stats()) << '\n';
                continue;
            }
            std::cout << "unknown command: " << line << '\n';
            continue;
        }

        if (pending.empty() && is_blank(line)) continue;
        pending += pending.empty() ? line : "\n" + line;
        if (ends_with_semicolon(pending)) {
            run(db, pending);
            pending.clear();
        }
    }
    // Input ended in the middle of a statement: run what we have.
    if (!is_blank(pending)) run(db, pending);
    return 0;
}
