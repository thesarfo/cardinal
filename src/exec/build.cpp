#include "exec/build.h"

#include "common/error.h"
#include "exec/filter.h"
#include "exec/limit.h"
#include "exec/project.h"
#include "exec/seq_scan.h"
#include "exec/sort.h"

namespace cardinal {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

std::unique_ptr<Operator> build_operator(const PhysicalPlan& plan, const Catalog& catalog) {
    return std::visit(
        Overloaded{
            [&](const PhysicalSeqScan& n) -> std::unique_ptr<Operator> {
                const Table* table = catalog.get_table(n.table);
                if (!table) throw DbError("unknown table " + n.table);
                return std::make_unique<SeqScan>(*table);
            },
            [&](const PhysicalFilter& n) -> std::unique_ptr<Operator> {
                return std::make_unique<Filter>(build_operator(*n.input, catalog), n.predicate);
            },
            [&](const PhysicalProject& n) -> std::unique_ptr<Operator> {
                std::vector<BoundExprPtr> exprs;
                for (const ProjectItem& item : n.items) exprs.push_back(item.expr);
                return std::make_unique<Project>(build_operator(*n.input, catalog), std::move(exprs));
            },
            [&](const PhysicalSort& n) -> std::unique_ptr<Operator> {
                return std::make_unique<Sort>(build_operator(*n.input, catalog), n.keys);
            },
            [&](const PhysicalLimit& n) -> std::unique_ptr<Operator> {
                return std::make_unique<Limit>(build_operator(*n.input, catalog), n.count);
            },
        },
        plan.node);
}

}  // namespace cardinal
