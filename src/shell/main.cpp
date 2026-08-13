#include <iostream>
#include <string>

#include "common/version.h"
#include "sql/ast_printer.h"
#include "sql/parser.h"

int main() {
    std::cout << "cardinal " << cardinal::version() << '\n';

    std::string line;
    while (std::cout << "cardinal> " && std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t") == std::string::npos) continue;

        // Anything that isn't SQL starts with a dot, so it can't clash with SQL.
        if (line[0] == '.') {
            if (line == ".quit") break;
            std::cout << "unknown command: " << line << '\n';
            continue;
        }

        // Until the engine can run statements, show how each one was understood.
        try {
            std::cout << cardinal::print(cardinal::parse_statement(line)) << '\n';
        } catch (const cardinal::ParseError& e) {
            std::cout << cardinal::format_error(line, e) << '\n';
        }
    }
    return 0;
}
