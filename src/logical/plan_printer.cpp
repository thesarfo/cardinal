#include "logical/plan_printer.h"

#include "binder/format.h"
#include "logical/plan_util.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

void write(const LogicalPlan& plan, const Scope& scope, int depth, std::string& out) {
    std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    std::vector<PlanPtr> children = children_of(plan);
    std::string line = std::visit(
        Overloaded{
            [&](const LogicalScan& n) {
                return "Scan[" + n.table + (n.alias == n.table ? "" : " AS " + n.alias) + "]";
            },
            [&](const LogicalEmpty&) { return std::string("Empty"); },
            [&](const LogicalJoin& n) {
                if (n.type == JoinType::Cross) return std::string("Join[CROSS]");
                return "Join[INNER ON " + format(*n.condition, scope) + "]";
            },
            [&](const LogicalFilter& n) {
                return "Filter[" + format(*n.predicate, scope) + "]";
            },
            [&](const LogicalProject& n) {
                std::string items;
                for (const ProjectItem& item : n.items)
                    items += (items.empty() ? "" : ", ") + format(*item.expr, scope);
                return "Project[" + items + "]";
            },
            [&](const LogicalSort& n) {
                std::string keys;
                for (const SortKey& key : n.keys)
                    keys += (keys.empty() ? "" : ", ") + format(*key.expr, scope) +
                            (key.descending ? " DESC" : " ASC");
                return "Sort[" + keys + "]";
            },
            [&](const LogicalLimit& n) {
                return "Limit[" + std::to_string(n.count) + "]";
            },
        },
        plan.node);
    out += indent + line;
    for (const PlanPtr& child : children) {
        out += "\n";
        write(*child, scope, depth + 1, out);
    }
}

}  // namespace

std::string print(const LogicalPlan& plan, const Scope& scope) {
    std::string out;
    write(plan, scope, 0, out);
    return out;
}

}  // namespace cardinal
