#include "physical/physical_printer.h"

#include "binder/format.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

std::string position(ColumnId id) { return "#" + std::to_string(id.value); }

void write(const PhysicalPlan& plan, int depth, std::string& out) {
    std::vector<const PhysicalPlan*> children;
    std::string line = std::visit(
        Overloaded{
            [&](const PhysicalSeqScan& n) { return "SeqScan[" + n.table + "]"; },
            [&](const PhysicalEmpty&) { return std::string("Empty"); },
            [&](const PhysicalHashJoin& n) {
                children = {n.left.get(), n.right.get()};
                auto left = [](ColumnId id) { return "l#" + std::to_string(id.value); };
                auto right = [](ColumnId id) { return "r#" + std::to_string(id.value); };
                std::string keys;
                for (std::size_t i = 0; i < n.left_keys.size(); ++i)
                    keys += (i ? " AND " : "") + format(*n.left_keys[i], left) + " = " + format(*n.right_keys[i], right);
                std::string text = std::string("HashJoin[build ") + (n.build_left ? "left" : "right") + ", " + keys;
                if (n.residual) text += ", then " + format(*n.residual, position);
                return text + "]";
            },
            [&](const PhysicalNestedLoopJoin& n) {
                children = {n.left.get(), n.right.get()};
                return n.condition ? "NestedLoopJoin[" + format(*n.condition, position) + "]"
                                   : std::string("NestedLoopJoin[CROSS]");
            },
            [&](const PhysicalFilter& n) {
                children = {n.input.get()};
                return "Filter[" + format(*n.predicate, position) + "]";
            },
            [&](const PhysicalProject& n) {
                children = {n.input.get()};
                std::string items;
                for (const ProjectItem& item : n.items)
                    items += (items.empty() ? "" : ", ") + format(*item.expr, position);
                return "Project[" + items + "]";
            },
            [&](const PhysicalSort& n) {
                children = {n.input.get()};
                std::string keys;
                for (const SortKey& key : n.keys)
                    keys += (keys.empty() ? "" : ", ") + format(*key.expr, position) +
                            (key.descending ? " DESC" : " ASC");
                return "Sort[" + keys + "]";
            },
            [&](const PhysicalLimit& n) {
                children = {n.input.get()};
                return "Limit[" + std::to_string(n.count) + "]";
            },
        },
        plan.node);
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + line;
    for (const PhysicalPlan* child : children) {
        out += "\n";
        write(*child, depth + 1, out);
    }
}

}  // namespace

std::string print(const PhysicalPlan& plan) {
    std::string out;
    write(plan, 0, out);
    return out;
}

}  // namespace cardinal
