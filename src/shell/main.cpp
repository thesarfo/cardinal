#include <iostream>
#include <string>

#include "common/version.h"

int main() {
    std::cout << "cardinal " << cardinal::version() << '\n';

    std::string line;
    while (std::cout << "cardinal> " && std::getline(std::cin, line)) {
        if (line == ".quit") break;
        std::cout << line << '\n';
    }
    return 0;
}
