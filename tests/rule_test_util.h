#pragma once

// Helpers for testing rewrite rules against plans built from SQL.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "engine/database.h"
#include "exec/build.h"
#include "logical/plan_printer.h"
#include "logical/plan_util.h"
#include "logical/planner.h"
#include "optimizer/rule_optimizer.h"
#include "physical/physical_planner.h"
#include "sql/parser.h"

namespace cardinal::testing {

// t(a INT, b INT, x DOUBLE, name TEXT, flag BOOL), users(id, name, age), orders(id, user_id, amount) and items(id, order_id, sku).
inline const Catalog& test_catalog() {
    static const Catalog catalog = [] {
        Catalog c;
        c.create_table({"t",
                        {{"a", Type::Int},
                         {"b", Type::Int},
                         {"x", Type::Double},
                         {"name", Type::Text},
                         {"flag", Type::Bool}}});
        c.create_table({"users", {{"id", Type::Int}, {"name", Type::Text}, {"age", Type::Int}}});
        c.create_table({"orders", {{"id", Type::Int}, {"user_id", Type::Int}, {"amount", Type::Double}}});
        c.create_table({"items", {{"id", Type::Int}, {"order_id", Type::Int}, {"sku", Type::Text}}});
        return c;
    }();
    return catalog;
}

struct Planned {
    BoundSelect bound;
    PlanPtr plan;
};

inline Planned plan_sql(const std::string& sql) {
    BoundSelect bound = bind_select(std::get<Select>(parse_statement(sql).node), test_catalog());
    PlanPtr plan = plan_select(bound);
    return {std::move(bound), std::move(plan)};
}

inline std::string plan_text(const std::string& sql) {
    Planned p = plan_sql(sql);
    return print(*p.plan, p.bound.scope);
}

inline RuleOptimizer optimizer_of(std::unique_ptr<Rule> rule) {
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(std::move(rule));
    return RuleOptimizer(std::move(rules));
}

template <class... Rules>
RuleOptimizer optimizer_of_all() {
    std::vector<std::unique_ptr<Rule>> rules;
    (rules.push_back(std::make_unique<Rules>()), ...);
    return RuleOptimizer(std::move(rules));
}

// Runs `rule` alone on the plan for `before_sql` and checks the result prints as
// `expected_plan`. Then runs it again and checks that changes nothing: a rule that
// keeps firing on its own output is a bug.
inline void assert_rewrite_to_text(std::unique_ptr<Rule> rule, const std::string& before_sql,
                                   const std::string& expected_plan) {
    INFO(before_sql);
    Planned p = plan_sql(before_sql);
    RuleOptimizer optimizer = optimizer_of(std::move(rule));

    OptimizeResult first = optimizer.optimize(p.plan);
    REQUIRE(print(*first.plan, p.bound.scope) == expected_plan);
    REQUIRE_FALSE(first.hit_pass_limit);

    OptimizeResult second = optimizer.optimize(first.plan);
    REQUIRE(second.trace.empty());
    REQUIRE(second.plan == first.plan);
}

// Same, but the expected plan is the plan for another query: "this SQL should end up
// looking like that SQL".
inline void assert_rewrite(std::unique_ptr<Rule> rule, const std::string& before_sql,
                           const std::string& after_sql) {
    assert_rewrite_to_text(std::move(rule), before_sql, plan_text(after_sql));
}

// The rule changes nothing on this query.
inline void assert_unchanged(std::unique_ptr<Rule> rule, const std::string& sql) {
    INFO(sql);
    Planned p = plan_sql(sql);
    OptimizeResult result = optimizer_of(std::move(rule)).optimize(p.plan);
    REQUIRE(result.trace.empty());
    REQUIRE(result.plan == p.plan);
}

}  // namespace cardinal::testing
