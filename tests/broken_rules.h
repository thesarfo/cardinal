#pragma once

// Rewrite rules that are wrong on purpose, for checking that the answer checker and the
// fuzzer notice. Each one breaks a different promise.

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "engine/database.h"
#include "optimizer/default_rules.h"
#include "optimizer/rule.h"

namespace cardinal::testing {

inline PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

// Drops the second half of every AND: a filter quietly gets weaker.
class DropsConjunct : public Rule {
public:
    std::string name() const override { return "drops-conjunct"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        const auto* f = std::get_if<LogicalFilter>(&node->node);
        if (!f) return std::nullopt;
        const auto* b = std::get_if<BoundBinary>(&f->predicate->node);
        if (!b || b->op != BinaryOp::And) return std::nullopt;
        return make(LogicalFilter{f->input, b->left});
    }
};

// The classic NULL mistake: x = x is "always true".
class EqualsSelfIsTrue : public Rule {
public:
    std::string name() const override { return "x-equals-x"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        const auto* f = std::get_if<LogicalFilter>(&node->node);
        if (!f) return std::nullopt;
        const auto* b = std::get_if<BoundBinary>(&f->predicate->node);
        if (!b || b->op != BinaryOp::Eq || !expr_equal(*b->left, *b->right)) return std::nullopt;
        auto yes = std::make_shared<const BoundExpr>(BoundExpr{BoundLiteral{Value(true)}, Type::Bool});
        return make(LogicalFilter{f->input, yes});
    }
};

// Flips every sort direction.
class FlipsSort : public Rule {
public:
    std::string name() const override { return "flips-sort"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        const auto* s = std::get_if<LogicalSort>(&node->node);
        if (!s || s->keys.size() != 1 || s->keys[0].descending) return std::nullopt;
        return make(LogicalSort{s->input, {{s->keys[0].expr, true}}});
    }
};

// Adds a tie-breaker to a one-key sort. Which of several tied rows comes first changes,
// but the sort keys come out in the same order, so this is a legal rewrite.
class BreaksTiesBackwards : public Rule {
public:
    std::string name() const override { return "breaks-ties-backwards"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        const auto* s = std::get_if<LogicalSort>(&node->node);
        if (!s || s->keys.size() != 1) return std::nullopt;
        auto id = std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{ColumnId{0}}, Type::Int});
        return make(LogicalSort{s->input, {s->keys[0], {id, true}}});
    }
};

// Throws away the join condition.
class DropsJoinCondition : public Rule {
public:
    std::string name() const override { return "drops-join-condition"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        const auto* j = std::get_if<LogicalJoin>(&node->node);
        if (!j || !j->condition) return std::nullopt;
        return make(LogicalJoin{j->left, j->right, JoinType::Cross, nullptr});
    }
};


inline void with_extra_rule(Database& db, std::function<std::unique_ptr<Rule>()> make_rule) {
    db.set_rule_factory([make_rule] {
        Database::Stages stages = default_stages();
        stages.front().push_back(make_rule());
        return stages;
    });
}


}  // namespace cardinal::testing
