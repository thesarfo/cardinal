#include <unistd.h>

#include <iostream>
#include <string>

#include "common/error.h"
#include "common/version.h"
#include "engine/database.h"
#include "engine/result_format.h"
#include "sql/parser.h"

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
