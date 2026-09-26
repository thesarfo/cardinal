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
    const PhysicalPlan* input = nullptr;
    std::string line = std::visit(
        Overloaded{
            [&](const PhysicalSeqScan& n) { return "SeqScan[" + n.table + "]"; },
            [&](const PhysicalEmpty&) { return std::string("Empty"); },
            [&](const PhysicalFilter& n) {
                input = n.input.get();
                return "Filter[" + format(*n.predicate, position) + "]";
            },
            [&](const PhysicalProject& n) {
                input = n.input.get();
                std::string items;
                for (const ProjectItem& item : n.items)
                    items += (items.empty() ? "" : ", ") + format(*item.expr, position);
                return "Project[" + items + "]";
            },
            [&](const PhysicalSort& n) {
                input = n.input.get();
                std::string keys;
                for (const SortKey& key : n.keys)
                    keys += (keys.empty() ? "" : ", ") + format(*key.expr, position) +
                            (key.descending ? " DESC" : " ASC");
                return "Sort[" + keys + "]";
            },
            [&](const PhysicalLimit& n) {
                input = n.input.get();
                return "Limit[" + std::to_string(n.count) + "]";
            },
        },
        plan.node);
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + line;
    if (input) {
        out += "\n";
        write(*input, depth + 1, out);
    }
}

}  // namespace

std::string print(const PhysicalPlan& plan) {
    std::string out;
    write(plan, 0, out);
    return out;
}

}  // namespace cardinal
