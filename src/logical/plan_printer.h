#pragma once

#include <string>

#include "binder/scope.h"
#include "logical/plan.h"

namespace cardinal {

// An indented tree, one step per line, root first, with the key details in
// brackets. The same plan always prints the same text, so tests can compare it.
//   Limit[3]
//     Project[name, score * 2]
//       Filter[active AND (id >= 1 AND id <= 10)]
//         Scan[users]
// `scope` supplies the column names. No trailing newline.
std::string print(const LogicalPlan& plan, const Scope& scope);

}  // namespace cardinal
