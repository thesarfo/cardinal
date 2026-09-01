#pragma once

#include <string>

#include "physical/plan.h"

namespace cardinal {

// Same indented layout as the logical printer. Columns show as their row position,
// `#0`, `#1`, ... since a physical plan no longer knows column names.
//   Project[#1]
//     Sort[#0 ASC]
//       SeqScan[users]
std::string print(const PhysicalPlan& plan);

}  // namespace cardinal
