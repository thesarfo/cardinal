#pragma once

#include <functional>
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

// Same, with `note(step)` added to the end of each line (put your own spacing in it).
// Return an empty string for no note.
using NodeNote = std::function<std::string(const LogicalPlan&)>;
std::string print(const LogicalPlan& plan, const Scope& scope, const NodeNote& note);

}  // namespace cardinal
