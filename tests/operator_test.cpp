#include <catch2/catch_test_macros.hpp>

#include "binder/binder.h"
#include "common/error.h"
#include "exec/filter.h"
#include "exec/project.h"
#include "exec/seq_scan.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

Value i(std::int64_t v) { return Value(v); }
Value s(const char* v) { return Value(std::string(v)); }

// people(id, name, score): five rows, one with a NULL score.
struct Fixture {
    Catalog catalog;
    Table* people;
    Scope scope;

    Fixture() {
        people = &catalog.create_table(
            {"people", {{"id", Type::Int}, {"name", Type::Text}, {"score", Type::Double}}});
        people->insert({i(1), s("ama"), Value(9.5)});
        people->insert({i(2), s("kofi"), Value(3.0)});
        people->insert({i(3), s("esi"), Value()});
        people->insert({i(4), s("yaw"), Value(7.25)});
        people->insert({i(5), s("abena"), Value(1.0)});
        scope.add("people", people->info());
    }

    // Single table, so the binder's ids already are row positions.
    BoundExprPtr expr(const char* sql) const { return bind_expression(*parse_expression(sql), scope); }

    std::unique_ptr<Operator> scan() const { return std::make_unique<SeqScan>(*people); }
};

std::vector<Row> drain(Operator& op) {
    std::vector<Row> rows;
    while (std::optional<Row> row = op.next()) rows.push_back(*row);
    return rows;
}

}  // namespace

TEST_CASE("scan: every row, in insertion order") {
    Fixture f;
    SeqScan scan(*f.people);
    auto rows = drain(scan);
    REQUIRE(rows.size() == 5);
    REQUIRE(rows[0] == Row{i(1), s("ama"), Value(9.5)});
    REQUIRE(rows[2] == Row{i(3), s("esi"), Value()});
    REQUIRE(rows[4][1] == s("abena"));
    REQUIRE(scan.stats().rows_out == 5);
}

TEST_CASE("scan: an empty table gives nothing") {
    Catalog catalog;
    Table& t = catalog.create_table({"t", {{"a", Type::Int}}});
    SeqScan scan(t);
    REQUIRE_FALSE(scan.next().has_value());
    REQUIRE(scan.stats().rows_out == 0);
}

TEST_CASE("scan: stays finished once it runs out") {
    Fixture f;
    SeqScan scan(*f.people);
    drain(scan);
    REQUIRE_FALSE(scan.next().has_value());
    REQUIRE_FALSE(scan.next().has_value());
    REQUIRE(scan.stats().rows_out == 5);
}

TEST_CASE("filter: keeps rows where the predicate is true") {
    Fixture f;
    Filter filter(f.scan(), f.expr("score > 5"));
    auto rows = drain(filter);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][0] == i(1));
    REQUIRE(rows[1][0] == i(4));
}

TEST_CASE("filter: NULL counts as not true") {
    Fixture f;
    // esi has a NULL score, so neither score > 5 nor its opposite keeps her.
    Filter high(f.scan(), f.expr("score > 5"));
    Filter low(f.scan(), f.expr("score <= 5"));
    REQUIRE(drain(high).size() + drain(low).size() == 4);
    Filter missing(f.scan(), f.expr("score IS NULL"));
    auto rows = drain(missing);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1] == s("esi"));
}

TEST_CASE("filter: matches nothing, matches everything") {
    Fixture f;
    Filter none(f.scan(), f.expr("id > 100"));
    REQUIRE(drain(none).empty());
    Filter all(f.scan(), f.expr("id > 0"));
    REQUIRE(drain(all).size() == 5);
}

TEST_CASE("filter: text and combined conditions") {
    Fixture f;
    Filter filter(f.scan(), f.expr("name < 'b' AND id <> 1"));
    auto rows = drain(filter);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1] == s("abena"));
}

TEST_CASE("project: one value per expression") {
    Fixture f;
    Project project(f.scan(), {f.expr("name"), f.expr("id * 10"), f.expr("score IS NULL")});
    auto rows = drain(project);
    REQUIRE(rows.size() == 5);
    REQUIRE(rows[0] == Row{s("ama"), i(10), Value(false)});
    REQUIRE(rows[2] == Row{s("esi"), i(30), Value(true)});
}

TEST_CASE("project: columns can be reordered and repeated") {
    Fixture f;
    Project project(f.scan(), {f.expr("score"), f.expr("id"), f.expr("id")});
    auto rows = drain(project);
    REQUIRE(rows[1] == Row{Value(3.0), i(2), i(2)});
}

TEST_CASE("project: NULL carries through") {
    Fixture f;
    Project project(f.scan(), {f.expr("score + 1")});
    auto rows = drain(project);
    REQUIRE(rows[0][0] == Value(10.5));
    REQUIRE(rows[2][0].is_null());
}

TEST_CASE("project: an evaluation error comes out of next()") {
    Fixture f;
    Project project(f.scan(), {f.expr("1 / (id - 3)")});
    REQUIRE(project.next().has_value());  // id 1
    REQUIRE(project.next().has_value());  // id 2
    REQUIRE_THROWS_AS(project.next(), DbError);  // id 3
}

TEST_CASE("operators chain: scan -> filter -> project") {
    Fixture f;
    auto filter = std::make_unique<Filter>(f.scan(), f.expr("score > 2"));
    Project project(std::move(filter), {f.expr("name")});
    auto rows = drain(project);
    REQUIRE(rows == std::vector<Row>{{s("ama")}, {s("kofi")}, {s("yaw")}});
}

TEST_CASE("stats: rows out at each step") {
    Fixture f;
    auto scan = std::make_unique<SeqScan>(*f.people);
    const Operator* scan_ptr = scan.get();
    auto filter = std::make_unique<Filter>(std::move(scan), f.expr("score > 2"));
    const Operator* filter_ptr = filter.get();
    Project project(std::move(filter), {f.expr("name")});
    drain(project);

    REQUIRE(scan_ptr->stats().rows_out == 5);
    REQUIRE(filter_ptr->stats().rows_out == 3);
    REQUIRE(project.stats().rows_out == 3);
}

TEST_CASE("stats: time counts the operators below, so it only grows upward") {
    Fixture f;
    auto scan = std::make_unique<SeqScan>(*f.people);
    const Operator* scan_ptr = scan.get();
    Project project(std::move(scan), {f.expr("id")});
    drain(project);
    REQUIRE(scan_ptr->stats().time.count() >= 0);
    REQUIRE(project.stats().time >= scan_ptr->stats().time);
}
