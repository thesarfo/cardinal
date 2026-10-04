#include "optimizer/default_rules.h"

#include "optimizer/boolean_cleanup.h"
#include "optimizer/column_pruning.h"
#include "optimizer/constant_folding.h"
#include "optimizer/cross_to_inner.h"
#include "optimizer/filter_cleanup.h"
#include "optimizer/filter_pushdown.h"

namespace cardinal {

std::vector<std::vector<std::unique_ptr<Rule>>> default_stages() {
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(std::make_unique<ConstantFolding>());
    rules.push_back(std::make_unique<BooleanCleanup>());
    rules.push_back(std::make_unique<RemoveTrueFilter>());
    rules.push_back(std::make_unique<MergeFilters>());
    rules.push_back(std::make_unique<EmptyFalseFilter>());
    rules.push_back(std::make_unique<FilterPushdown>());
    rules.push_back(std::make_unique<CrossToInnerJoin>());

    std::vector<std::unique_ptr<Rule>> pruning;
    pruning.push_back(std::make_unique<ColumnPruning>());

    std::vector<std::vector<std::unique_ptr<Rule>>> stages;
    stages.push_back(std::move(rules));
    stages.push_back(std::move(pruning));
    return stages;
}

}  // namespace cardinal
