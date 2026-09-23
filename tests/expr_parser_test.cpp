#include <catch2/catch_test_macros.hpp>

#include "sql/ast_printer.h"
#include "sql/parser.h"

using namespace cardinal;

namespace {

std::string parsed(const char* sql) { return print(*parse_expression(sql)); }

}  // namespace

TEST_CASE("expr: precedence") {
    struct Case {
        const char* sql;
        const char* tree;
    };
    const Case cases[] = {
        // AND binds tighter than OR
        {"a OR b AND c", "(or (col a) (and (col b) (col c)))"},
        {"a AND b OR c", "(or (and (col a) (col b)) (col c))"},
        // NOT binds looser than comparison
        {"NOT a = b", "(not (= (col a) (col b)))"},
        {"NOT a AND b", "(and (not (col a)) (col b))"},
        {"NOT NOT a", "(not (not (col a)))"},
        {"1 + 2 * 3", "(+ 1 (* 2 3))"},
        {"1 * 2 + 3", "(+ (* 1 2) 3)"},
        {"(1 + 2) * 3", "(* (+ 1 2) 3)"},
        {"10 - 4 - 3", "(- (- 10 4) 3)"},
        {"8 / 4 / 2", "(/ (/ 8 4) 2)"},
        {"7 % 3 * 2", "(* (% 7 3) 2)"},
        // unary minus binds tightest
        {"-x + 1", "(+ (neg (col x)) 1)"},
        {"- 2 * 3", "(* (neg 2) 3)"},
        {"1 - -2", "(- 1 (neg 2))"},
        // arithmetic binds tighter than comparison, comparison tighter than AND
        {"a + 1 > b * 2 AND c", "(and (> (+ (col a) 1) (* (col b) 2)) (col c))"},
        {"a < 1 OR a >= 9", "(or (< (col a) 1) (>= (col a) 9))"},
        {"a <> b", "(<> (col a) (col b))"},
        {"a != b", "(<> (col a) (col b))"},
        // BETWEEN's AND is its own
        {"x BETWEEN 1 AND 5 AND y", "(and (between (col x) 1 5) (col y))"},
        {"y AND x BETWEEN 1 AND 5", "(and (col y) (between (col x) 1 5))"},
        {"x BETWEEN a + 1 AND b * 2", "(between (col x) (+ (col a) 1) (* (col b) 2))"},
        {"x NOT BETWEEN 1 AND 5", "(not-between (col x) 1 5)"},
        {"x IN (1, 2, 3)", "(in (col x) 1 2 3)"},
        {"x IN (1)", "(in (col x) 1)"},
        {"x NOT IN (1 + 1, 'a')", "(not-in (col x) (+ 1 1) 'a')"},
        {"x IN (1) OR y", "(or (in (col x) 1) (col y))"},
        {"a IS NULL", "(is-null (col a))"},
        {"a IS NOT NULL", "(is-not-null (col a))"},
        {"a IS NULL AND b IS NOT NULL", "(and (is-null (col a)) (is-not-null (col b)))"},
        {"NOT a IS NULL", "(not (is-null (col a)))"},
        {"42", "42"},
        {"3.5", "3.5"},
        {"'it''s'", "'it''s'"},
        {"TRUE and false", "(and true false)"},
        {"NULL", "null"},
        {"t.a = u.b", "(= (col t.a) (col u.b))"},
        {"Name", "(col Name)"},
    };
    for (const Case& c : cases) {
        INFO(c.sql);
        REQUIRE(parsed(c.sql) == c.tree);
    }
}

TEST_CASE("expr: errors have positions") {
    struct Case {
        const char* sql;
        const char* message;
        int column;
    };
    const Case cases[] = {
        {"", "expected an expression", 1},
        {"1 +", "expected an expression", 4},
        {"(1 + 2", "expected ')'", 7},
        {"1 2", "expected end of input", 3},
        {"x BETWEEN 1 5", "expected 'AND'", 13},
        {"x IN ()", "expected an expression", 7},
        {"x IN (1", "expected ')'", 8},
        {"t.", "expected a column name after '.'", 3},
        {"a IS 5", "expected 'NULL'", 6},
        {"a NOT 5", "expected end of input", 3},
        {"* 2", "expected an expression", 1},
        {"'oops", "unterminated string", 1},
        {"a @ b", "unexpected character '@'", 3},
        {"99999999999999999999", "integer is too large", 1},
    };
    for (const Case& c : cases) {
        INFO(c.sql);
        try {
            parse_expression(c.sql);
            FAIL("expected a ParseError");
        } catch (const ParseError& e) {
            REQUIRE(e.message() == c.message);
            REQUIRE(e.line() == 1);
            REQUIRE(e.column() == c.column);
        }
    }
}
