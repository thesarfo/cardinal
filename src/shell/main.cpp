#include <iostream>

#include "common/version.h"

int main() {
    std::cout << "cardinal " << cardinal::version() << '\n';
    return 0;
}
