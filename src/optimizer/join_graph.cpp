#include "optimizer/join_graph.h"

#include <algorithm>
#include <bit>
#include <numeric>
#include <stdexcept>

#include "logical/plan_util.h"
#include "optimizer/conjuncts.h"

namespace cardinal {

namespace {

PlanPtr make(auto node) { return std::make_shared<const LogicalPlan>(LogicalPlan{std::move(node)}); }

bool is_join_region(const PlanPtr& plan) {
    if (const auto* join = std::get_if<LogicalJoin>(&plan->node)) return join->type == JoinType::Inner || join->type == JoinType::Cross;
    if (const auto* filter = std::get_if<LogicalFilter>(&plan->node)) return is_join_region(filter->input);
    if (const auto* prune = std::get_if<LogicalPrune>(&plan->node)) return is_join_region(prune->input);
    return false;
}

struct Collected {
    std::vector<GraphRelation> relations;
    std::vector<BoundExprPtr> parts;  // the AND-ed parts, in the order met
};

void collect(const PlanPtr& plan, Collected& out) {
    if (const auto* join = std::get_if<LogicalJoin>(&plan->node); join && is_join_region(plan)) {
        collect(join->left, out);
        collect(join->right, out);
        if (join->condition)
            for (BoundExprPtr& part : split_ands(join->condition)) out.parts.push_back(std::move(part));
        return;
    }
    if (const auto* filter = std::get_if<LogicalFilter>(&plan->node); filter && is_join_region(plan)) {
        collect(filter->input, out);
        for (BoundExprPtr& part : split_ands(filter->predicate)) out.parts.push_back(std::move(part));
        return;
    }
    if (const auto* prune = std::get_if<LogicalPrune>(&plan->node); prune && is_join_region(plan)) {
        collect(prune->input, out);  // only narrows the rows; column pruning can put it back
        return;
    }
    out.relations.push_back({plan, output_columns(*plan)});
}

// The one table `expr` reads, or nullopt if it reads none or several.
std::optional<std::size_t> only_table(const BoundExpr& expr, const std::vector<GraphRelation>& relations) {
    std::optional<std::size_t> found;
    for (ColumnId column : columns_used(expr)) {
        for (std::size_t i = 0; i < relations.size(); ++i) {
            const auto& cols = relations[i].columns;
            if (std::find(cols.begin(), cols.end(), column) == cols.end()) continue;
            if (found && *found != i) return std::nullopt;
            found = i;
        }
    }
    return found;
}

// Bit i set if `expr` reads relation i. Nothing if it reads a column no relation has.
std::optional<std::uint32_t> tables_read(const BoundExpr& expr, const std::vector<GraphRelation>& relations) {
    std::uint32_t tables = 0;
    for (ColumnId column : columns_used(expr)) {
        bool found = false;
        for (std::size_t i = 0; i < relations.size() && !found; ++i) {
            const auto& cols = relations[i].columns;
            found = std::find(cols.begin(), cols.end(), column) != cols.end();
            if (found) tables |= std::uint32_t{1} << i;
        }
        if (!found) return std::nullopt;
    }
    return tables;
}

}  // namespace

std::optional<JoinGraph> extract_join_graph(const PlanPtr& plan) {
    if (!is_join_region(plan)) return std::nullopt;
    Collected collected;
    collect(plan, collected);
    if (collected.relations.size() < 2 || collected.relations.size() > kMaxGraphRelations) return std::nullopt;

    JoinGraph graph;
    graph.relations = std::move(collected.relations);
    for (BoundExprPtr& part : collected.parts) {
        std::optional<std::uint32_t> tables = tables_read(*part, graph.relations);
        if (!tables) return std::nullopt;

        PredicateKind kind = PredicateKind::Other;
        std::uint32_t eq_left = 0, eq_right = 0;
        if (std::popcount(*tables) == 1) {
            kind = PredicateKind::SingleTable;
        } else if (const auto* b = std::get_if<BoundBinary>(&part->node); b && b->op == BinaryOp::Eq) {
            std::optional<std::size_t> l = only_table(*b->left, graph.relations);
            std::optional<std::size_t> r = only_table(*b->right, graph.relations);
            if (l && r && *l != *r) kind = PredicateKind::Edge;
            eq_left = *tables_read(*b->left, graph.relations);
            eq_right = *tables_read(*b->right, graph.relations);
        }
        graph.predicates.push_back({std::move(part), *tables, kind, eq_left, eq_right});
    }
    return graph;
}

PlanPtr relation_with_filters(const JoinGraph& graph, std::size_t table) {
    std::vector<BoundExprPtr> own;
    for (const GraphPredicate& p : graph.predicates)
        if (p.kind == PredicateKind::SingleTable && p.tables == std::uint32_t{1} << table) own.push_back(p.expr);
    PlanPtr plan = graph.relations[table].plan;
    return own.empty() ? plan : make(LogicalFilter{plan, and_all(own)});
}

PlanPtr join_onto(const JoinGraph& graph, PlanPtr left, std::uint32_t joined, std::size_t table, PlanPtr right) {
    const std::uint32_t after = joined | std::uint32_t{1} << table;
    std::vector<BoundExprPtr> condition;
    for (const GraphPredicate& p : graph.predicates) {
        if (p.kind == PredicateKind::SingleTable || p.tables == 0) continue;
        if ((p.tables & ~after) == 0 && (p.tables & ~joined) != 0) condition.push_back(p.expr);
    }
    return condition.empty() ? make(LogicalJoin{std::move(left), std::move(right), JoinType::Cross, nullptr})
                             : make(LogicalJoin{std::move(left), std::move(right), JoinType::Inner, and_all(condition)});
}

PlanPtr with_constant_filters(const JoinGraph& graph, PlanPtr tree) {
    // Conditions that read no table are the same wherever they go.
    std::vector<BoundExprPtr> constants;
    for (const GraphPredicate& p : graph.predicates)
        if (p.tables == 0) constants.push_back(p.expr);
    return constants.empty() ? tree : make(LogicalFilter{std::move(tree), and_all(constants)});
}

bool connects(const JoinGraph& graph, std::uint32_t joined, std::size_t table) {
    const std::uint32_t bit = std::uint32_t{1} << table, after = joined | bit;
    for (const GraphPredicate& p : graph.predicates) {
        if (p.kind == PredicateKind::SingleTable || p.tables == 0) continue;
        if ((p.tables & ~after) == 0 && (p.tables & bit) != 0 && (p.tables & joined) != 0) return true;
    }
    return false;
}

bool hash_joinable(const JoinGraph& graph, std::uint32_t joined, std::size_t table) {
    const std::uint32_t bit = std::uint32_t{1} << table, after = joined | bit;
    auto within = [](std::uint32_t tables, std::uint32_t mask) { return tables != 0 && (tables & ~mask) == 0; };
    for (const GraphPredicate& p : graph.predicates) {
        if (p.kind == PredicateKind::SingleTable || (p.tables & ~after) != 0 || (p.tables & bit) == 0) continue;
        if ((within(p.eq_left, joined) && within(p.eq_right, bit)) || (within(p.eq_left, bit) && within(p.eq_right, joined)))
            return true;
    }
    return false;
}

bool cross_join_can_wait(const JoinGraph& graph, std::uint32_t joined, std::size_t table) {
    if (connects(graph, joined, table)) return false;
    for (std::size_t other = 0; other < graph.relations.size(); ++other)
        if ((joined >> other & 1) == 0 && connects(graph, joined, other)) return true;
    return false;
}

PlanPtr build_join_tree(const JoinGraph& graph, const std::vector<std::size_t>& order) {
    const std::size_t n = graph.relations.size();
    std::vector<std::size_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::size_t> expected(n);
    std::iota(expected.begin(), expected.end(), std::size_t{0});
    if (sorted != expected) throw std::invalid_argument("the order must list every table exactly once");

    PlanPtr tree = relation_with_filters(graph, order[0]);
    std::uint32_t joined = std::uint32_t{1} << order[0];
    for (std::size_t step = 1; step < n; ++step) {
        tree = join_onto(graph, tree, joined, order[step], relation_with_filters(graph, order[step]));
        joined |= std::uint32_t{1} << order[step];
    }
    return with_constant_filters(graph, tree);
}

PlanPtr build_join_tree(const JoinGraph& graph) {
    std::vector<std::size_t> order(graph.relations.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    return build_join_tree(graph, order);
}

}  // namespace cardinal
