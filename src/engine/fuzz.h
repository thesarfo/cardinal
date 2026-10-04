#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/database.h"

namespace cardinal {

// A true/false condition as a small tree, so a failing query can be shrunk by cutting
// branches. A Leaf holds the SQL text of one comparison or test.
struct BoolExpr {
    enum class Kind { Leaf, And, Or, Not };
    Kind kind = Kind::Leaf;
    std::string text;
    std::vector<BoolExpr> kids;
};

struct JoinSpec {
    std::string table;
    std::string alias;
    std::optional<BoolExpr> on;  // none: a comma join
};

// A query as parts, for generating, printing and shrinking.
struct QuerySpec {
    std::string from_table;
    std::string from_alias;
    std::vector<JoinSpec> joins;
    std::vector<std::string> select;  // SQL text of each item; "*" on its own is allowed
    std::optional<BoolExpr> where;
    std::vector<std::pair<std::string, bool>> order_by;  // expression, descending
    std::optional<int> limit;
};

// The query on one line, ready to run.
std::string to_sql(const QuerySpec& spec);
// The same query with one clause per line, for reading.
std::string to_lines(const QuerySpec& spec);

// Creates tables t1, t2, t3, each with columns (id INT, a INT, b INT, c DOUBLE, s TEXT, f BOOL),
// and fills them with a few random rows from small value ranges, so joins and filters
// find matches, ties are common, and NULLs show up. The same seed gives the same data.
void create_fuzz_tables(Database& db, uint64_t seed);

// Query number `index` for `seed`. Every query depends only on those two numbers, so a
// failure can be reproduced from them alone. Always type-correct and never divides.
QuerySpec generate_query(uint64_t seed, int index);

// Every spec one step simpler than `spec`: a clause dropped, a join dropped, a branch of
// a condition cut, a select item removed.
std::vector<QuerySpec> simplifications(const QuerySpec& spec);

// Keeps taking a simplification that `still_fails` accepts, until none is left.
QuerySpec shrink(const QuerySpec& spec, const std::function<bool(const QuerySpec&)>& still_fails);

struct FuzzFailure {
    uint64_t seed;
    int index;
    QuerySpec original;
    QuerySpec shrunk;
    std::string detail;  // the answer checker's explanation, for the shrunk query
};

struct FuzzReport {
    int queries = 0;
    int compared = 0;  // ran every way and the answers agreed
    int skipped = 0;   // the query failed even without the optimizer, so was not compared
    std::optional<FuzzFailure> failure;
};

// Runs `count` generated queries through the answer checker, stopping at the first
// disagreement and shrinking it. `db` must hold the tables from create_fuzz_tables.
FuzzReport fuzz(Database& db, uint64_t seed, int count);

// "seed 7, query #12", the original and shrunk queries, and the checker's detail.
std::string describe(const FuzzFailure& failure);

}  // namespace cardinal
