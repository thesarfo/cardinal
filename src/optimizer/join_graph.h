#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "binder/bound_expr.h"
#include "logical/plan.h"

namespace cardinal {

// A tree of inner and cross joins, taken apart. The tables are the points, and the
// conditions between them are the lines. Which table joins first is no longer part of
// it, so a planner can try the orders and put a tree back together.

// One input to the joins: a scan, or whatever sits on a scan (a filter pushed down, a
// prune), anything that is not itself a join.
struct GraphRelation {
    PlanPtr plan;
    std::vector<ColumnId> columns;
};

enum class PredicateKind {
    SingleTable,  // reads one table: runs on that table before any join
    Edge,         // left_expr = right_expr, each side from a different table
    Other,        // anything else: reads several tables, or none
};

// One AND-ed part of a join condition or of a filter above the joins.
struct GraphPredicate {
    BoundExprPtr expr;
    std::uint32_t tables;  // bit i is set if relation i is read
    PredicateKind kind;
};

struct JoinGraph {
    std::vector<GraphRelation> relations;  // in the order they appeared, left to right
    std::vector<GraphPredicate> predicates;
};

constexpr std::size_t kMaxGraphRelations = 32;

// Takes apart `plan` if it is a join, or filters on top of joins. Gives nothing for any
// other plan, or for more than kMaxGraphRelations tables.
std::optional<JoinGraph> extract_join_graph(const PlanPtr& plan);

// Table `table` with its own single-table filters right on top of it.
PlanPtr relation_with_filters(const JoinGraph& graph, std::size_t table);

// `left`, which holds the tables in `joined`, joined to `right`, which is table `table`.
// The condition is every one that the new table makes checkable; with none it is a cross join.
PlanPtr join_onto(const JoinGraph& graph, PlanPtr left, std::uint32_t joined, std::size_t table, PlanPtr right);

// `tree` under a filter for the conditions that read no table, if there are any.
PlanPtr with_constant_filters(const JoinGraph& graph, PlanPtr tree);

// True if some condition joins `table` to the tables in `joined`: it reads both, and
// nothing else.
bool connects(const JoinGraph& graph, std::uint32_t joined, std::size_t table);

// True if adding `table` to `joined` would be a cross join that could have waited:
// nothing joins `table` to the group, but something joins another table to it. A planner
// skips these. Only a group with nothing left to join to it may take a cross join.
bool cross_join_can_wait(const JoinGraph& graph, std::uint32_t joined, std::size_t table);

// The tables joined left-deep in `order` (a permutation of 0..n-1). Each condition
// goes on the first join that has every table it reads, a single-table filter goes
// right on its table, and a join with no condition is a cross join.
PlanPtr build_join_tree(const JoinGraph& graph, const std::vector<std::size_t>& order);

// The tables joined in the order they appeared.
PlanPtr build_join_tree(const JoinGraph& graph);

}  // namespace cardinal
