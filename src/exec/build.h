#pragma once

#include <memory>

#include "catalog/catalog.h"
#include "exec/operator.h"
#include "physical/plan.h"

namespace cardinal {

// Builds the operators for a physical plan, one for one. The tables must outlive
// the operators. Throws DbError if a scanned table doesn't exist.
std::unique_ptr<Operator> build_operator(const PhysicalPlan& plan, const Catalog& catalog);

}  // namespace cardinal
