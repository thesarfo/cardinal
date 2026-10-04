#pragma once

#include <memory>
#include <vector>

#include "optimizer/rule.h"

namespace cardinal {

// The rules the engine runs, in two stages. Stage one is every rewrite that moves or
// simplifies conditions. Stage two is column pruning, which waits until the plan has
// stopped changing shape, because the Prune steps it adds would block pushdown.
std::vector<std::vector<std::unique_ptr<Rule>>> default_stages();

}  // namespace cardinal
