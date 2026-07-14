#include <cstring>

#include "common/version.h"

int main() { return std::strlen(cardinal::version()) > 0 ? 0 : 1; }
