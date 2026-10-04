#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/database.h"

namespace cardinal {

struct CheckOutcome {
    bool ok = true;
    // When not ok: the query, which setups gave a different answer, and what they gave.
    // When ok but nothing was compared: why not.
    std::string detail;
    // Rules that are the likely cause: wrong when they are the only rule on, or turning
    // them off fixes an answer that was wrong.
    std::vector<std::string> culprits;
};

// Runs a SELECT every way the engine can run it and checks the answers agree:
//   - with the optimizer off (the reference: the plan exactly as written),
//   - with all rules on,
//   - with all rules on except one, for each rule in turn, and
//   - with just one rule on, for each rule in turn.
// The last two find a wrong rule even when another rule happens to hide its mistake.
// The database's own settings are put back afterwards. Anything that is not a SELECT,
// or a SELECT the reference run cannot finish (an error), is not compared.
//
// What "agree" means depends on the query, because SQL leaves some choices open:
//   no ORDER BY, no LIMIT   the same rows, in any order
//   ORDER BY                the sort-key values of the rows, in order (rows with equal
//                           keys may come out in any order), plus the same rows if
//                           there is no LIMIT
//   LIMIT without ORDER BY  the same number of rows (which rows is not defined)
CheckOutcome check_query(Database& db, std::string_view sql);

}  // namespace cardinal
