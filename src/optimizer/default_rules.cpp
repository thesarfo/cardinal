#include "optimizer/default_rules.h"

#include "optimizer/boolean_cleanup.h"
#include "optimizer/constant_folding.h"
#include "optimizer/filter_cleanup.h"
#include "optimizer/filter_pushdown.h"

namespace cardinal {

std::vector<std::unique_ptr<Rule>> default_rules() {
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(std::make_unique<ConstantFolding>());
    rules.push_back(std::make_unique<BooleanCleanup>());
    rules.push_back(std::make_unique<RemoveTrueFilter>());
    rules.push_back(std::make_unique<MergeFilters>());
    rules.push_back(std::make_unique<EmptyFalseFilter>());
    rules.push_back(std::make_unique<FilterPushdown>());
    return rules;
}

}  // namespace cardinal
