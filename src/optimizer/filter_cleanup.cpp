#include "optimizer/filter_cleanup.h"

#include "logical/plan_util.h"

namespace cardinal {

namespace {

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

// A literal that no row can pass: FALSE or NULL.
bool never_true(const BoundExpr& e) {
    const auto* lit = std::get_if<BoundLiteral>(&e.node);
    return lit && (lit->value.is_null() || !lit->value.as_bool());
}

bool always_true(const BoundExpr& e) {
    const auto* lit = std::get_if<BoundLiteral>(&e.node);
    return lit && !lit->value.is_null() && lit->value.as_bool();
}

}  // namespace

std::optional<PlanPtr> MergeFilters::apply(const PlanPtr& node) const {
    const auto* outer = std::get_if<LogicalFilter>(&node->node);
    if (!outer) return std::nullopt;
    const auto* inner = std::get_if<LogicalFilter>(&outer->input->node);
    if (!inner) return std::nullopt;

    BoundExprPtr both = std::make_shared<const BoundExpr>(
        BoundExpr{BoundBinary{BinaryOp::And, inner->predicate, outer->predicate}, Type::Bool});
    return make(LogicalFilter{inner->input, both});
}

std::optional<PlanPtr> RemoveTrueFilter::apply(const PlanPtr& node) const {
    const auto* filter = std::get_if<LogicalFilter>(&node->node);
    if (!filter || !always_true(*filter->predicate)) return std::nullopt;
    return filter->input;
}

std::optional<PlanPtr> EmptyFalseFilter::apply(const PlanPtr& node) const {
    const auto* filter = std::get_if<LogicalFilter>(&node->node);
    if (!filter || !never_true(*filter->predicate)) return std::nullopt;
    return make(LogicalEmpty{output_columns(*filter->input)});
}

}  // namespace cardinal
