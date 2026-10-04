#include "optimizer/column_pruning.h"

#include <algorithm>

#include "logical/plan_util.h"

namespace cardinal {

namespace {

using ColumnSet = std::set<ColumnId>;

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

void add_columns(ColumnSet& into, const BoundExpr& expr) {
    ColumnSet used = columns_used(expr);
    into.insert(used.begin(), used.end());
}

ColumnSet restricted_to(const ColumnSet& wanted, const std::vector<ColumnId>& available) {
    ColumnSet out;
    for (ColumnId id : available)
        if (wanted.count(id)) out.insert(id);
    return out;
}

bool has_extras(const std::vector<ColumnId>& output, const ColumnSet& required) {
    return std::any_of(output.begin(), output.end(), [&](ColumnId id) { return !required.count(id); });
}

std::vector<ColumnId> in_output_order(const std::vector<ColumnId>& output, const ColumnSet& required) {
    std::vector<ColumnId> out;
    for (ColumnId id : output)
        if (required.count(id)) out.push_back(id);
    return out;
}

PlanPtr prune_join(const PlanPtr& node, const ColumnSet& required);

// Prunes the inside of `node` without wrapping `node` itself.
PlanPtr narrow(const PlanPtr& node, const ColumnSet& required);

// `node` as an input to something that needs exactly `required` from it. Hands back
// `node` itself when it is already right, which is what makes a second run a no-op.
PlanPtr prune_side(const PlanPtr& node, const ColumnSet& required) {
    const auto* existing = std::get_if<LogicalPrune>(&node->node);
    PlanPtr base = existing ? existing->input : node;
    PlanPtr narrowed = narrow(base, required);
    std::vector<ColumnId> output = output_columns(*narrowed);

    if (!has_extras(output, required)) return existing ? narrowed : (narrowed == node ? node : narrowed);

    std::vector<ColumnId> keep = in_output_order(output, required);
    if (existing && narrowed == existing->input && existing->columns == keep) return node;
    return make(LogicalPrune{narrowed, keep});
}

PlanPtr narrow(const PlanPtr& node, const ColumnSet& required) {
    if (const auto* filter = std::get_if<LogicalFilter>(&node->node)) {
        ColumnSet needed = required;
        add_columns(needed, *filter->predicate);
        PlanPtr input = prune_side(filter->input, needed);
        return input == filter->input ? node : make(LogicalFilter{input, filter->predicate});
    }
    if (std::holds_alternative<LogicalJoin>(node->node)) return prune_join(node, required);
    return node;
}

PlanPtr prune_join(const PlanPtr& node, const ColumnSet& required) {
    const auto& join = std::get<LogicalJoin>(node->node);
    ColumnSet needed = required;
    if (join.condition) add_columns(needed, *join.condition);

    PlanPtr left = prune_side(join.left, restricted_to(needed, output_columns(*join.left)));
    PlanPtr right = prune_side(join.right, restricted_to(needed, output_columns(*join.right)));
    if (left == join.left && right == join.right) return node;
    return make(LogicalJoin{left, right, join.type, join.condition});
}

// The Filter and Sort steps between a Project and the joins below it only add to what
// is needed; the join (if any) is where pruning starts.
PlanPtr descend(const PlanPtr& node, const ColumnSet& required) {
    if (const auto* filter = std::get_if<LogicalFilter>(&node->node)) {
        ColumnSet needed = required;
        add_columns(needed, *filter->predicate);
        PlanPtr input = descend(filter->input, needed);
        return input == filter->input ? node : make(LogicalFilter{input, filter->predicate});
    }
    if (const auto* sort = std::get_if<LogicalSort>(&node->node)) {
        ColumnSet needed = required;
        for (const SortKey& key : sort->keys) add_columns(needed, *key.expr);
        PlanPtr input = descend(sort->input, needed);
        return input == sort->input ? node : make(LogicalSort{input, sort->keys});
    }
    if (std::holds_alternative<LogicalJoin>(node->node)) return prune_join(node, required);
    return node;
}

}  // namespace

std::optional<PlanPtr> ColumnPruning::apply(const PlanPtr& node) const {
    const auto* project = std::get_if<LogicalProject>(&node->node);
    if (!project) return std::nullopt;

    ColumnSet required;
    for (const ProjectItem& item : project->items) add_columns(required, *item.expr);

    PlanPtr input = descend(project->input, required);
    if (input == project->input) return std::nullopt;
    return make(LogicalProject{input, project->items});
}

}  // namespace cardinal
