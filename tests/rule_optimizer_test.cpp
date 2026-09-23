#include "logical/plan_util.h"
#include "rule_test_util.h"

using namespace cardinal;
using namespace cardinal::testing;

namespace {

// Raises every LIMIT by one, so it always has something to do.
class AlwaysFires : public Rule {
public:
    std::string name() const override { return "always-fires"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        if (const auto* limit = std::get_if<LogicalLimit>(&node->node))
            return std::make_shared<const LogicalPlan>(LogicalPlan{LogicalLimit{limit->input, limit->count + 1}});
        return std::nullopt;
    }
};

// Turns LIMIT 1 into LIMIT 2 once, and does nothing otherwise.
class OneShot : public Rule {
public:
    std::string name() const override { return "one-shot"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        const auto* limit = std::get_if<LogicalLimit>(&node->node);
        if (!limit || limit->count != 1) return std::nullopt;
        return std::make_shared<const LogicalPlan>(LogicalPlan{LogicalLimit{limit->input, 2}});
    }
};

class Idle : public Rule {
public:
    std::string name() const override { return "idle"; }
    std::optional<PlanPtr> apply(const PlanPtr&) const override { return std::nullopt; }
};

// Claims to have changed the plan but hands back the same node.
class ReturnsSame : public Rule {
public:
    std::string name() const override { return "returns-same"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override { return node; }
};

// Counts how many Filters it was shown, in a counter that outlives the rule.
class CountsFilters : public Rule {
public:
    explicit CountsFilters(int& seen) : seen_(seen) {}
    std::string name() const override { return "counts-filters"; }
    std::optional<PlanPtr> apply(const PlanPtr& node) const override {
        if (std::holds_alternative<LogicalFilter>(node->node)) ++seen_;
        return std::nullopt;
    }

private:
    int& seen_;
};

}  // namespace

TEST_CASE("optimizer: a rule that always fires stops at the pass limit") {
    Planned p = plan_sql("SELECT a FROM t LIMIT 3");
    OptimizeResult result = optimizer_of(std::make_unique<AlwaysFires>()).optimize(p.plan);

    REQUIRE(result.hit_pass_limit);
    REQUIRE(result.trace.size() == 20);
    REQUIRE(std::get<LogicalLimit>(result.plan->node).count == 23);
    REQUIRE(result.trace.front().pass == 1);
    REQUIRE(result.trace.back().pass == 20);
}

TEST_CASE("optimizer: the pass limit can be set") {
    Planned p = plan_sql("SELECT a FROM t LIMIT 3");
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(std::make_unique<AlwaysFires>());
    OptimizeResult result = RuleOptimizer(std::move(rules), 5).optimize(p.plan);
    REQUIRE(result.hit_pass_limit);
    REQUIRE(result.trace.size() == 5);
    REQUIRE(std::get<LogicalLimit>(result.plan->node).count == 8);
}

TEST_CASE("optimizer: stops as soon as a pass changes nothing") {
    Planned p = plan_sql("SELECT a FROM t LIMIT 1");
    OptimizeResult result = optimizer_of(std::make_unique<OneShot>()).optimize(p.plan);
    REQUIRE_FALSE(result.hit_pass_limit);
    REQUIRE(result.trace.size() == 1);
    REQUIRE(std::get<LogicalLimit>(result.plan->node).count == 2);
}

TEST_CASE("optimizer: no rules, or rules with nothing to do, give back the same plan") {
    Planned p = plan_sql("SELECT a FROM t WHERE a > 1 ORDER BY a LIMIT 3");
    OptimizeResult none = RuleOptimizer({}).optimize(p.plan);
    REQUIRE(none.plan == p.plan);
    REQUIRE(none.trace.empty());

    OptimizeResult idle = optimizer_of(std::make_unique<Idle>()).optimize(p.plan);
    REQUIRE(idle.plan == p.plan);
    REQUIRE(idle.trace.empty());
}

TEST_CASE("optimizer: a rule that returns the same node counts as no change") {
    Planned p = plan_sql("SELECT a FROM t LIMIT 3");
    OptimizeResult result = optimizer_of(std::make_unique<ReturnsSame>()).optimize(p.plan);
    REQUIRE(result.trace.empty());
    REQUIRE(result.plan == p.plan);
}

TEST_CASE("optimizer: rules see every node in the tree") {
    Planned p = plan_sql("SELECT a FROM t WHERE a > 1 LIMIT 3");
    int seen = 0;
    optimizer_of(std::make_unique<CountsFilters>(seen)).optimize(p.plan);
    REQUIRE(seen == 1);
}

TEST_CASE("optimizer: a change deep in the tree rebuilds the nodes above it and shares the rest") {
    // Only the Limit changes, so everything below it must be the very same objects.
    Planned p = plan_sql("SELECT a FROM t WHERE a > 1 LIMIT 1");
    OptimizeResult result = optimizer_of(std::make_unique<OneShot>()).optimize(p.plan);
    REQUIRE(result.plan != p.plan);
    REQUIRE(input_of(*result.plan) == input_of(*p.plan));
}

TEST_CASE("trace: records the rule, the pass and the plan on both sides") {
    Planned p = plan_sql("SELECT a FROM t LIMIT 1");
    OptimizeResult result = optimizer_of(std::make_unique<OneShot>()).optimize(p.plan);
    REQUIRE(format_trace(result.trace, p.bound.scope) ==
            "1. one-shot (pass 1)\n"
            "   before:\n"
            "     Limit[1]\n"
            "       Project[a]\n"
            "         Scan[t]\n"
            "   after:\n"
            "     Limit[2]\n"
            "       Project[a]\n"
            "         Scan[t]");
}

TEST_CASE("trace: several firings, and an empty one") {
    Planned p = plan_sql("SELECT a FROM t LIMIT 3");
    std::vector<std::unique_ptr<Rule>> rules;
    rules.push_back(std::make_unique<AlwaysFires>());
    OptimizeResult result = RuleOptimizer(std::move(rules), 2).optimize(p.plan);
    REQUIRE(format_trace(result.trace, p.bound.scope) ==
            "1. always-fires (pass 1)\n"
            "   before:\n"
            "     Limit[3]\n"
            "       Project[a]\n"
            "         Scan[t]\n"
            "   after:\n"
            "     Limit[4]\n"
            "       Project[a]\n"
            "         Scan[t]\n"
            "\n"
            "2. always-fires (pass 2)\n"
            "   before:\n"
            "     Limit[4]\n"
            "       Project[a]\n"
            "         Scan[t]\n"
            "   after:\n"
            "     Limit[5]\n"
            "       Project[a]\n"
            "         Scan[t]");
    REQUIRE(format_trace({}, p.bound.scope) == "no rules fired");
}

TEST_CASE("assert_rewrite: the helper itself") {
    assert_rewrite(std::make_unique<OneShot>(), "SELECT a FROM t LIMIT 1", "SELECT a FROM t LIMIT 2");
    assert_rewrite_to_text(std::make_unique<OneShot>(), "SELECT a FROM t LIMIT 1",
                           "Limit[2]\n  Project[a]\n    Scan[t]");
    assert_unchanged(std::make_unique<OneShot>(), "SELECT a FROM t LIMIT 5");
    assert_unchanged(std::make_unique<Idle>(), "SELECT a FROM t");
}
