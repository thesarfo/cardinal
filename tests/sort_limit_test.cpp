#include <catch2/catch_test_macros.hpp>

#include "binder/binder.h"
#include "exec/filter.h"
#include "exec/limit.h"
#include "exec/project.h"
#include "exec/seq_scan.h"
#include "exec/sort.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

// t(a INT, b TEXT, c DOUBLE): positions 0, 1, 2
struct Fixture {
    Catalog catalog;
    Table* t;
    Scope scope;

    Fixture() {
        t = &catalog.create_table({"t", {{"a", Type::Int}, {"b", Type::Text}, {"c", Type::Double}}});
        scope.add("t", t->info());
    }

    void add(Value a, const char* b, Value c) { t->insert({std::move(a), s(b), std::move(c)}); }

    BoundExprPtr expr(const char* sql) const { return bind_expression(*parse_expression(sql), scope); }
    std::unique_ptr<Operator> scan() const { return std::make_unique<SeqScan>(*t); }

    std::unique_ptr<Operator> sort(std::vector<std::pair<const char*, bool>> keys) const {
        std::vector<SortKey> k;
        for (auto& [sql, desc] : keys) k.push_back({expr(sql), desc});
        return std::make_unique<Sort>(scan(), std::move(k));
    }
};

std::vector<Row> drain(Operator& op) {
    std::vector<Row> rows;
    while (std::optional<Row> row = op.next()) rows.push_back(*row);
    return rows;
}

std::vector<Value> column(const std::vector<Row>& rows, std::size_t index) {
    std::vector<Value> out;
    for (const Row& r : rows) out.push_back(r[index]);
    return out;
}

}  // namespace

TEST_CASE("sort: ascending and descending") {
    Fixture f;
    f.add(i(3), "c", Value(1.0));
    f.add(i(1), "a", Value(2.0));
    f.add(i(2), "b", Value(3.0));

    auto up = f.sort({{"a", false}});
    REQUIRE(column(drain(*up), 0) == std::vector<Value>{i(1), i(2), i(3)});
    auto down = f.sort({{"a", true}});
    REQUIRE(column(drain(*down), 0) == std::vector<Value>{i(3), i(2), i(1)});
}

TEST_CASE("sort: whole rows move together") {
    Fixture f;
    f.add(i(2), "two", Value(0.5));
    f.add(i(1), "one", Value(1.5));
    auto op = f.sort({{"a", false}});
    auto rows = drain(*op);
    REQUIRE(rows[0] == Row{i(1), s("one"), Value(1.5)});
    REQUIRE(rows[1] == Row{i(2), s("two"), Value(0.5)});
}

TEST_CASE("sort: several keys, each with its own direction") {
    Fixture f;
    f.add(i(1), "x", Value(1.0));
    f.add(i(2), "y", Value(1.0));
    f.add(i(1), "z", Value(2.0));
    f.add(i(2), "w", Value(2.0));
    auto op = f.sort({{"a", true}, {"b", false}});
    auto rows = drain(*op);
    REQUIRE(column(rows, 1) == std::vector<Value>{s("w"), s("y"), s("x"), s("z")});
}

TEST_CASE("sort: equal keys keep their arrival order") {
    Fixture f;
    f.add(i(1), "first", Value(0.0));
    f.add(i(0), "zero", Value(0.0));
    f.add(i(1), "second", Value(0.0));
    f.add(i(1), "third", Value(0.0));
    auto up = f.sort({{"a", false}});
    REQUIRE(column(drain(*up), 1) == std::vector<Value>{s("zero"), s("first"), s("second"), s("third")});
    // Descending is stable too: ties are not reversed.
    auto down = f.sort({{"a", true}});
    REQUIRE(column(drain(*down), 1) == std::vector<Value>{s("first"), s("second"), s("third"), s("zero")});
}

TEST_CASE("sort: NULLs come first ascending and last descending") {
    Fixture f;
    f.add(i(2), "b", Value(2.0));
    f.add(Value(), "n1", Value());
    f.add(i(1), "a", Value(1.0));
    f.add(Value(), "n2", Value());

    auto up = f.sort({{"a", false}});
    REQUIRE(column(drain(*up), 1) == std::vector<Value>{s("n1"), s("n2"), s("a"), s("b")});
    auto down = f.sort({{"a", true}});
    REQUIRE(column(drain(*down), 1) == std::vector<Value>{s("b"), s("a"), s("n1"), s("n2")});
}

TEST_CASE("sort: text, doubles and expressions") {
    Fixture f;
    f.add(i(1), "pear", Value(2.5));
    f.add(i(2), "apple", Value(10.0));
    f.add(i(3), "fig", Value(0.25));

    auto by_text = f.sort({{"b", false}});
    REQUIRE(column(drain(*by_text), 1) == std::vector<Value>{s("apple"), s("fig"), s("pear")});
    auto by_double = f.sort({{"c", true}});
    REQUIRE(column(drain(*by_double), 0) == std::vector<Value>{i(2), i(1), i(3)});
    auto by_expr = f.sort({{"-a", false}});
    REQUIRE(column(drain(*by_expr), 0) == std::vector<Value>{i(3), i(2), i(1)});
}

TEST_CASE("sort: empty input and one row") {
    Fixture f;
    auto empty = f.sort({{"a", false}});
    REQUIRE(drain(*empty).empty());
    f.add(i(1), "a", Value(1.0));
    auto one = f.sort({{"a", false}});
    REQUIRE(drain(*one).size() == 1);
}

TEST_CASE("limit: first N rows") {
    Fixture f;
    for (int n = 1; n <= 5; ++n) f.add(i(n), "x", Value(0.0));
    Limit limit(f.scan(), 3);
    REQUIRE(column(drain(limit), 0) == std::vector<Value>{i(1), i(2), i(3)});
}

TEST_CASE("limit: zero, more than available, and exactly all") {
    Fixture f;
    for (int n = 1; n <= 3; ++n) f.add(i(n), "x", Value(0.0));
    Limit zero(f.scan(), 0);
    REQUIRE(drain(zero).empty());
    Limit many(f.scan(), 100);
    REQUIRE(drain(many).size() == 3);
    Limit exact(f.scan(), 3);
    REQUIRE(drain(exact).size() == 3);
}

TEST_CASE("limit: stops pulling from its input") {
    Fixture f;
    for (int n = 1; n <= 10; ++n) f.add(i(n), "x", Value(0.0));

    auto scan = std::make_unique<SeqScan>(*f.t);
    const Operator* scan_ptr = scan.get();
    Limit limit(std::move(scan), 4);
    drain(limit);
    REQUIRE(limit.stats().rows_out == 4);
    REQUIRE(scan_ptr->stats().rows_out == 4);

    auto scan0 = std::make_unique<SeqScan>(*f.t);
    const Operator* scan0_ptr = scan0.get();
    Limit none(std::move(scan0), 0);
    drain(none);
    REQUIRE(scan0_ptr->stats().rows_out == 0);
}

TEST_CASE("ORDER BY a DESC, b LIMIT 5 over a filtered table") {
    Fixture f;
    f.add(i(1), "m", Value(1.0));
    f.add(i(3), "b", Value(1.0));
    f.add(i(3), "a", Value(1.0));
    f.add(i(2), "z", Value(1.0));
    f.add(Value(), "q", Value(1.0));
    f.add(i(5), "k", Value(1.0));
    f.add(i(4), "k", Value(0.0));

    auto filter = std::make_unique<Filter>(f.scan(), f.expr("c > 0.5"));
    auto sort = std::make_unique<Sort>(std::move(filter),
                                       std::vector<SortKey>{{f.expr("a"), true}, {f.expr("b"), false}});
    Limit limit(std::move(sort), 5);
    auto rows = drain(limit);
    // a: 5, 3, 3, 2, 1  (the NULL row would sort last, so LIMIT 5 leaves it out)
    REQUIRE(column(rows, 0) == std::vector<Value>{i(5), i(3), i(3), i(2), i(1)});
    REQUIRE(column(rows, 1) == std::vector<Value>{s("k"), s("a"), s("b"), s("z"), s("m")});
}

TEST_CASE("sort then project: the sort key doesn't have to be selected") {
    Fixture f;
    f.add(i(2), "second", Value(0.0));
    f.add(i(1), "first", Value(0.0));
    auto sort = f.sort({{"a", false}});
    Project project(std::move(sort), {f.expr("b")});
    REQUIRE(drain(project) == std::vector<Row>{{s("first")}, {s("second")}});
}
