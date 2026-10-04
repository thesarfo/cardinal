#pragma once

#include "optimizer/rule.h"

namespace cardinal {

// Drops columns nothing above needs, so the rows a join copies and compares are narrow.
//
// Starting from a Project, it works out which columns the plan still needs and walks
// down to the joins, adding a Prune step above each scan, filter or inner join whose
// rows carry columns the steps above never read:
//
//   Project[name]                          Project[name]
//     Join[ON u.id = user_id]                Join[ON u.id = user_id]
//       Scan[users AS u]          ->           Prune[u.id, name]
//       Scan[orders AS o]                        Scan[users AS u]
//                                              Prune[user_id]
//                                                Scan[orders AS o]
//
// A column survives until the last step that reads it: one used only in a join
// condition, a filter, or an ORDER BY is kept that long and dropped after. Plans with
// no join are left alone, because a narrower row saves nothing when no step copies rows
// around.
class ColumnPruning : public Rule {
public:
    std::string name() const override { return "column-pruning"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override;
};

}  // namespace cardinal
