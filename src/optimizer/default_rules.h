#pragma once

#include <memory>
#include <vector>

#include "optimizer/rule.h"

namespace cardinal {

// The rules the engine runs, in the order it runs them within a pass.
std::vector<std::unique_ptr<Rule>> default_rules();

}  // namespace cardinal
