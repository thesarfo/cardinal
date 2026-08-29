#pragma once

#include "binder/binder.h"
#include "logical/plan.h"

namespace cardinal {

// Builds   Limit(Project(Sort(Filter(Scan))))   leaving out the steps the query
// doesn't use (Project is always there).
//
// Sort goes below Project, not above it. ORDER BY keys are bound against the
// table's columns, and a key such as `ORDER BY id` is often not in the select
// list. Sorting first means the keys are always still available, and Project
// keeps the order it is given.
PlanPtr plan_select(const BoundSelect& select);

}  // namespace cardinal
