#include "physical/physical_planner.h"

#include <algorithm>
#include <stdexcept>

#include "optimizer/conjuncts.h"
#include "physical/join_keys.h"


namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <class Node>
PhysicalPtr make(Node node) {
    return std::make_shared<const PhysicalPlan>(PhysicalPlan{std::move(node)});
}

BoundExprPtr make_expr(auto node, const BoundExpr& like) {
    return std::make_shared<const BoundExpr>(BoundExpr{std::move(node), like.type});
}

// Which ColumnId sits at each position of the rows a step produces.
using Layout = std::vector<ColumnId>;

BoundExprPtr to_positions(const BoundExprPtr& expr, const Layout& layout) {
    return std::visit(
        Overloaded{
            [&](const BoundLiteral&) { return expr; },
            [&](const BoundColumn& n) {
                auto it = std::find(layout.begin(), layout.end(), n.id);
                if (it == layout.end())
                    throw std::logic_error("column #" + std::to_string(n.id.value) +
                                           " is not produced by the step below");
                auto position = static_cast<std::uint32_t>(it - layout.begin());
                return make_expr(BoundColumn{ColumnId{position}}, *expr);
            },
            [&](const BoundUnary& n) {
                return make_expr(BoundUnary{n.op, to_positions(n.operand, layout)}, *expr);
            },
            [&](const BoundBinary& n) {
                return make_expr(BoundBinary{n.op, to_positions(n.left, layout),
                                             to_positions(n.right, layout)},
                                 *expr);
            },
            [&](const BoundIsNull& n) {
                return make_expr(BoundIsNull{to_positions(n.operand, layout), n.negated}, *expr);
            },
        },
        expr->node);
}

struct Planned {
    PhysicalPtr plan;
    Layout layout;  // empty after a Project: its rows hold values, not columns
};

// `column op constant`, or `constant op column` turned around, where op is a comparison.
struct Comparison {
    ColumnId column;
    BinaryOp op;
    Value value;
};

std::optional<Comparison> as_comparison(const BoundExprPtr& expr) {
    const auto* b = std::get_if<BoundBinary>(&expr->node);
    if (!b) return std::nullopt;
    BinaryOp flipped;
    switch (b->op) {
        case BinaryOp::Eq: flipped = BinaryOp::Eq; break;
        case BinaryOp::Lt: flipped = BinaryOp::Gt; break;
        case BinaryOp::Le: flipped = BinaryOp::Ge; break;
        case BinaryOp::Gt: flipped = BinaryOp::Lt; break;
        case BinaryOp::Ge: flipped = BinaryOp::Le; break;
        default: return std::nullopt;
    }
    const auto* lc = std::get_if<BoundColumn>(&b->left->node);
    const auto* rl = std::get_if<BoundLiteral>(&b->right->node);
    if (lc && rl && !rl->value.is_null()) return Comparison{lc->id, b->op, rl->value};
    const auto* ll = std::get_if<BoundLiteral>(&b->left->node);
    const auto* rc = std::get_if<BoundColumn>(&b->right->node);
    if (ll && rc && !ll->value.is_null()) return Comparison{rc->id, flipped, ll->value};
    return std::nullopt;
}

// A Filter over a Scan, read through an index. Uses the first indexed column the filter
// compares with a constant, takes an equality, or else one lower and one upper bound, and
// leaves every other condition in a filter above.
std::optional<Planned> plan_index_scan(const LogicalFilter& filter, const PlannerOptions& options) {
    if (!options.use_indexes || !options.catalog) return std::nullopt;
    const auto* scan = std::get_if<LogicalScan>(&filter.input->node);
    if (!scan) return std::nullopt;
    const Table* table = options.catalog->get_table(scan->table);
    if (!table || table->indexes().empty()) return std::nullopt;

    std::vector<BoundExprPtr> conjuncts = split_ands(filter.predicate);
    for (const BoundExprPtr& first : conjuncts) {
        std::optional<Comparison> head = as_comparison(first);
        if (!head) continue;
        auto at = std::find(scan->columns.begin(), scan->columns.end(), head->column);
        if (at == scan->columns.end()) continue;
        const std::string& column = table->info().columns[static_cast<std::size_t>(at - scan->columns.begin())].name;
        const Table::IndexEntry* entry = table->index_on(column);
        if (!entry) continue;
        std::string index_name;
        for (const auto& [name, e] : table->indexes())
            if (&e == entry) index_name = name;

        PhysicalIndexScan node{scan->table, index_name, column, std::nullopt, std::nullopt};
        std::vector<BoundExprPtr> rest;
        bool exact = false;
        for (const BoundExprPtr& conjunct : conjuncts) {
            std::optional<Comparison> c = as_comparison(conjunct);
            bool used = false;
            if (c && c->column == head->column && !exact) {
                if (c->op == BinaryOp::Eq && !node.lo && !node.hi) {
                    node.lo = Bound{c->value, true};
                    node.hi = Bound{c->value, true};
                    exact = used = true;
                } else if ((c->op == BinaryOp::Gt || c->op == BinaryOp::Ge) && !node.lo) {
                    node.lo = Bound{c->value, c->op == BinaryOp::Ge};
                    used = true;
                } else if ((c->op == BinaryOp::Lt || c->op == BinaryOp::Le) && !node.hi) {
                    node.hi = Bound{c->value, c->op == BinaryOp::Le};
                    used = true;
                }
            }
            if (!used) rest.push_back(conjunct);
        }
        PhysicalPtr plan = make(std::move(node));
        if (!rest.empty()) plan = make(PhysicalFilter{plan, to_positions(and_all(rest), scan->columns)});
        return Planned{plan, scan->columns};
    }
    return std::nullopt;
}

Planned plan_node(const LogicalPlan& plan, const PlannerOptions& options) {
    return std::visit(
        Overloaded{
            [&](const LogicalScan& n) {
                return Planned{make(PhysicalSeqScan{n.table}), n.columns};
            },
            [&](const LogicalEmpty& n) { return Planned{make(PhysicalEmpty{}), n.columns}; },
            [&](const LogicalPrune& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<ProjectItem> items;
                for (ColumnId id : n.columns) {
                    BoundExprPtr column = std::make_shared<const BoundExpr>(BoundExpr{BoundColumn{id}, std::nullopt});
                    items.push_back({to_positions(column, in.layout), ""});
                }
                return Planned{make(PhysicalProject{in.plan, std::move(items)}), n.columns};
            },
            [&](const LogicalJoin& n) {
                Planned left = plan_node(*n.left, options);
                Planned right = plan_node(*n.right, options);
                Layout layout = left.layout;
                layout.insert(layout.end(), right.layout.begin(), right.layout.end());
                JoinChoice choice = options.choose_join ? options.choose_join(n) : JoinChoice{options.join_method, options.build_left};
                if (choice.method == JoinMethod::Hash) {
                    JoinKeys split = split_join_condition(n.condition, left.layout, right.layout);
                    if (!split.keys.empty()) {
                        std::vector<BoundExprPtr> left_keys, right_keys;
                        for (const auto& [left_expr, right_expr] : split.keys) {
                            left_keys.push_back(to_positions(left_expr, left.layout));
                            right_keys.push_back(to_positions(right_expr, right.layout));
                        }
                        BoundExprPtr residual = split.residual.empty() ? nullptr : to_positions(and_all(split.residual), layout);
                        return Planned{make(PhysicalHashJoin{left.plan, right.plan, std::move(left_keys), std::move(right_keys),
                                                             residual, choice.build_left}),
                                       layout};
                    }
                }
                BoundExprPtr condition = n.condition ? to_positions(n.condition, layout) : nullptr;
                return Planned{make(PhysicalNestedLoopJoin{left.plan, right.plan, condition}), layout};
            },
            [&](const LogicalFilter& n) {
                if (std::optional<Planned> indexed = plan_index_scan(n, options)) return *indexed;
                Planned in = plan_node(*n.input, options);
                return Planned{make(PhysicalFilter{in.plan, to_positions(n.predicate, in.layout)}),
                               in.layout};
            },
            [&](const LogicalProject& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<ProjectItem> items;
                for (const ProjectItem& item : n.items)
                    items.push_back({to_positions(item.expr, in.layout), item.name});
                return Planned{make(PhysicalProject{in.plan, std::move(items)}), {}};
            },
            [&](const LogicalAggregate& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<BoundExprPtr> keys;
                Layout layout;
                for (const GroupKey& key : n.keys) {
                    keys.push_back(to_positions(key.expr, in.layout));
                    layout.push_back(key.output);
                }
                std::vector<PhysicalAggregateCall> calls;
                for (const AggregateItem& item : n.aggregates) {
                    calls.push_back({item.func, item.arg ? to_positions(item.arg, in.layout) : nullptr});
                    layout.push_back(item.output);
                }
                AggregateMethod method = options.choose_aggregate ? options.choose_aggregate(n) : options.aggregate_method;
                if (method == AggregateMethod::Sort)
                    return Planned{make(PhysicalSortAggregate{in.plan, std::move(keys), std::move(calls)}), layout};
                return Planned{make(PhysicalHashAggregate{in.plan, std::move(keys), std::move(calls)}), layout};
            },
            [&](const LogicalSort& n) {
                Planned in = plan_node(*n.input, options);
                std::vector<SortKey> keys;
                for (const SortKey& key : n.keys)
                    keys.push_back({to_positions(key.expr, in.layout), key.descending});
                return Planned{make(PhysicalSort{in.plan, std::move(keys)}), in.layout};
            },
            [&](const LogicalLimit& n) {
                Planned in = plan_node(*n.input, options);
                return Planned{make(PhysicalLimit{in.plan, n.count}), in.layout};
            },
        },
        plan.node);
}

}  // namespace

PhysicalPtr plan_physical(const LogicalPlan& plan, const PlannerOptions& options) { return plan_node(plan, options).plan; }

}  // namespace cardinal
