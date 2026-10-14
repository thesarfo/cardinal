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
    std::string index;
    std::string column;
    std::optional<Bound> lo, hi;
    std::vector<BoundExprPtr> answered;
    std::vector<BoundExprPtr> rest;
};

// Uses the first indexed column that one of the filter's conditions compares with a constant
// (`=`, `<`, `<=`, `>`, `>=`, either way round). Takes an equality, or else one lower and one
// upper bound. Nothing if the filter is not directly over a scan, or no index fits.
std::optional<IndexMatch> match_index(const LogicalFilter& filter, const Catalog& catalog);

}  // namespace cardinal
