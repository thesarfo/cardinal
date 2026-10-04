#include <catch2/catch_test_macros.hpp>

#include <functional>

#include "engine/answer_checker.h"
#include "logical/plan_util.h"
#include "optimizer/default_rules.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Database make_db() {
    Database db;
    db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
    db.execute("CREATE TABLE orders (id INT, user_id INT, amount DOUBLE)");
    db.execute("INSERT INTO users VALUES (1, 'ama', 30), (2, 'kofi', 20), (3, 'esi', 30), (4, 'yaw', NULL)");
    db.execute("INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 70.5), (12, 3, 2.0), (13, NULL, 1.0), (14, 4, NULL)");
    return db;
}

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

// Rules that are wrong on purpose.

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

void with_extra_rule(Database& db, std::function<std::unique_ptr<Rule>()> make_rule) {
    db.set_rule_factory([make_rule] {
        Database::Stages stages = default_stages();
        stages.front().push_back(make_rule());
        return stages;
    });
}

}  // namespace

TEST_CASE("checker: the real rules pass on every kind of query") {
    Database db = make_db();
    const char* queries[] = {
        "SELECT name FROM users",
        "SELECT name FROM users WHERE age > 20 + 5 AND TRUE",
        "SELECT name FROM users WHERE age = age",
        "SELECT name FROM users WHERE 1 > 2",
        "SELECT name FROM users WHERE NULL",
        "SELECT name, age FROM users ORDER BY age, name",
        "SELECT name FROM users ORDER BY age DESC LIMIT 2",
        "SELECT name FROM users LIMIT 3",
        "SELECT u.name, o.id FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 3",
        "SELECT u.name, o.id FROM users u, orders o WHERE u.id = o.user_id AND u.age > 25",
        "SELECT u.name FROM users u, orders o WHERE u.age IS NULL OR o.amount IS NULL",
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id ORDER BY o.amount, u.name",
        "SELECT a.name, b.name FROM users a JOIN users b ON a.age = b.age WHERE a.id < b.id",
    };
    for (const char* sql : queries) {
        INFO(sql);
        CheckOutcome outcome = check_query(db, sql);
        INFO(outcome.detail);
        REQUIRE(outcome.ok);
    }
}

TEST_CASE("checker: leaves the database as it found it") {
    Database db = make_db();
    db.set_optimizer_enabled(true);
    check_query(db, "SELECT name FROM users WHERE age > 20 + 5");
    REQUIRE(db.optimizer_enabled());
    REQUIRE(db.execute("EXPLAIN SELECT name FROM users WHERE age > 20 + 5").message.find("constant-folding") != std::string::npos);
}

TEST_CASE("checker: statements that are not queries, and queries that fail, are not compared") {
    Database db = make_db();
    CheckOutcome create = check_query(db, "CREATE TABLE t (a INT)");
    REQUIRE(create.ok);
    REQUIRE_FALSE(create.detail.empty());

    CheckOutcome failing = check_query(db, "SELECT 1 / 0 FROM users");
    REQUIRE(failing.ok);
    REQUIRE(failing.detail.find("division by zero") != std::string::npos);
}

TEST_CASE("checker: catches a rule that makes a filter weaker") {
    Database db = make_db();
    with_extra_rule(db, [] { return std::make_unique<DropsConjunct>(); });
    CheckOutcome outcome = check_query(db, "SELECT name FROM users WHERE age = 30 AND id < 2");
    REQUIRE_FALSE(outcome.ok);
    REQUIRE(outcome.culprits == std::vector<std::string>{"drops-conjunct"});
    REQUIRE(outcome.detail.find("SELECT name FROM users WHERE age = 30 AND id < 2") != std::string::npos);
    REQUIRE(outcome.detail.find("all rules on") != std::string::npos);
    REQUIRE(outcome.detail.find("turning this off fixes it: drops-conjunct") != std::string::npos);
}

TEST_CASE("checker: catches the NULL mistake x = x is TRUE") {
    Database db = make_db();
    with_extra_rule(db, [] { return std::make_unique<EqualsSelfIsTrue>(); });
    CheckOutcome outcome = check_query(db, "SELECT name FROM users WHERE age = age");
    REQUIRE_FALSE(outcome.ok);
    REQUIRE(outcome.culprits == std::vector<std::string>{"x-equals-x"});
}

TEST_CASE("checker: catches a rule that drops a join condition") {
    Database db = make_db();
    with_extra_rule(db, [] { return std::make_unique<DropsJoinCondition>(); });
    CheckOutcome outcome = check_query(db, "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    REQUIRE_FALSE(outcome.ok);
    REQUIRE(outcome.culprits == std::vector<std::string>{"drops-join-condition"});
}

TEST_CASE("checker: catches wrong order") {
    Database db = make_db();
    with_extra_rule(db, [] { return std::make_unique<FlipsSort>(); });
    CheckOutcome outcome = check_query(db, "SELECT name FROM users ORDER BY age");
    REQUIRE_FALSE(outcome.ok);
    REQUIRE(outcome.culprits == std::vector<std::string>{"flips-sort"});

    // The order of a LIMIT query decides which rows it returns.
    REQUIRE_FALSE(check_query(db, "SELECT name FROM users WHERE age IS NOT NULL ORDER BY age LIMIT 1").ok);
}

TEST_CASE("checker: rows tied on the sort key may come out in any order") {
    Database db = make_db();
    with_extra_rule(db, [] { return std::make_unique<BreaksTiesBackwards>(); });
    // ama and esi are both 30. With the tie-breaker esi comes first, and with LIMIT 3 the
    // third row could be either of them; the sort keys (20, 30, 30) are the same.
    CheckOutcome outcome = check_query(db, "SELECT name FROM users WHERE age IS NOT NULL ORDER BY age LIMIT 3");
    INFO(outcome.detail);
    REQUIRE(outcome.ok);
    REQUIRE(check_query(db, "SELECT name FROM users WHERE age IS NOT NULL ORDER BY age LIMIT 2").ok);
    REQUIRE(check_query(db, "SELECT name FROM users ORDER BY age").ok);
}

TEST_CASE("checker: a rule that is wrong only on some data is caught on data where it matters") {
    Database db;
    db.execute("CREATE TABLE t (a INT)");
    db.execute("INSERT INTO t VALUES (1), (2)");
    with_extra_rule(db, [] { return std::make_unique<EqualsSelfIsTrue>(); });
    // No NULLs yet, so x = x really is always true and the rule is not caught...
    REQUIRE(check_query(db, "SELECT a FROM t WHERE a = a").ok);
    // ...until a NULL shows up.
    db.execute("INSERT INTO t VALUES (NULL)");
    REQUIRE_FALSE(check_query(db, "SELECT a FROM t WHERE a = a").ok);
}

TEST_CASE("rules can be switched off one at a time") {
    Database db = make_db();
    std::vector<std::string> names = db.rule_names();
    REQUIRE(names == std::vector<std::string>{"constant-folding", "boolean-cleanup", "remove-true-filter",
                                              "merge-filters", "empty-false-filter", "filter-pushdown",
                                              "cross-to-inner-join", "column-pruning"});

    auto fired = [&](const char* sql) { return db.execute(std::string("EXPLAIN ") + sql).message; };
    REQUIRE(fired("SELECT name FROM users WHERE age > 20 + 5").find("constant-folding") != std::string::npos);
    db.set_disabled_rules({"constant-folding"});
    REQUIRE(fired("SELECT name FROM users WHERE age > 20 + 5").find("constant-folding") == std::string::npos);
    db.set_disabled_rules({});
    REQUIRE(fired("SELECT name FROM users WHERE age > 20 + 5").find("constant-folding") != std::string::npos);
}
