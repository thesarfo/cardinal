#pragma once

#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "logical/plan.h"

namespace cardinal {

// How a Filter over a Scan could be read through an index: which index and column, the bounds,
// the conditions the bounds answer, and the conditions left for a filter above.
struct IndexMatch {
    const LogicalPlan* scan_node = nullptr;  // the plan node holding `scan`
    const LogicalScan* scan = nullptr;
    const LogicalPrune* prune = nullptr;  // a Prune between the filter and the scan, if there is one
    std::string index;
    std::string column;
    std::optional<Bound> lo, hi;
    std::vector<BoundExprPtr> answered;
    std::vector<BoundExprPtr> rest;
};

// Uses the first indexed column that one of the filter's conditions compares with a constant
// (`=`, `<`, `<=`, `>`, `>=`, either way round). Takes an equality, or else one lower and one
// upper bound. The filter may sit directly on the scan or on a Prune of it (column pruning puts
// one there). Nothing if it is over anything else, or no index fits.
std::optional<IndexMatch> match_index(const LogicalFilter& filter, const Catalog& catalog);

}  // namespace cardinal
