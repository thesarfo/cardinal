// cardinal_fuzz [seed] [count]
// Runs random queries through the answer checker. Prints nothing but a summary if all
// agree; on a disagreement prints the seed, the query, the shrunk query and the details.

#include <cstdlib>
#include <iostream>

#include "engine/fuzz.h"

int main(int argc, char** argv) {
    uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    int count = argc > 2 ? std::atoi(argv[2]) : 10000;

    cardinal::Database db;
    cardinal::create_fuzz_tables(db, seed);
    cardinal::FuzzReport report = cardinal::fuzz(db, seed, count);

    std::cout << "seed " << seed << ": " << report.queries << " queries, " << report.compared << " compared, "
              << report.skipped << " skipped\n";
    if (report.failure) {
        std::cout << "FAILED\n" << cardinal::describe(*report.failure) << "\n";
        return 1;
    }
    return report.skipped == 0 ? 0 : 1;
}
