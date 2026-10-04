// Runs sqllogictest-style .test files against the engine.
//
//   # a comment
//   statement ok
//   CREATE TABLE t (a INT)
//
//   statement error
//   SELECT * FROM nope
//   ----
//   unknown table          <- optional: the error message must contain this
//
//   query IT nosort        <- one letter per column: I whole number (or bool as 1/0),
//   SELECT a, b FROM t        T text, R real (shown with 3 decimals)
//   ----                   <- then nosort (the default), rowsort or valuesort
//   1                      <- expected values, one per line, row by row
//   foo                       NULL shows as NULL, empty text as (empty)
//
// Records are separated by blank lines. Usage: cardinal_slt [--no-optimizer] file.test...
// Every file must pass with the optimizer on and with it off, and every query in it is
// also run through the answer checker (engine/answer_checker.h).

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "common/error.h"
#include "engine/answer_checker.h"
#include "engine/database.h"
#include "sql/parser.h"

namespace {

using cardinal::Value;

struct Runner {
    std::string file;
    std::vector<std::string> lines;
    std::size_t pos = 0;
    cardinal::Database db;
    int failures = 0;
    int records = 0;

    int line_number() const { return static_cast<int>(pos) + 1; }

    bool at_end() const { return pos >= lines.size(); }

    static bool blank(const std::string& s) { return s.find_first_not_of(" \t\r") == std::string::npos; }

    static std::string trim_right(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        return s;
    }

    void fail(int at, const std::string& sql, const std::string& what) {
        ++failures;
        std::cerr << file << ":" << at << ": FAIL\n  sql: " << sql << "\n  " << what << "\n";
    }

    // SQL lines up to a blank line or "----".
    std::string read_sql() {
        std::string sql;
        while (!at_end() && !blank(lines[pos]) && trim_right(lines[pos]) != "----") {
            if (!sql.empty()) sql += "\n";
            sql += trim_right(lines[pos++]);
        }
        return sql;
    }

    // Lines after "----" up to a blank line.
    std::vector<std::string> read_expected() {
        std::vector<std::string> out;
        if (!at_end() && trim_right(lines[pos]) == "----") {
            ++pos;
            while (!at_end() && !blank(lines[pos])) out.push_back(trim_right(lines[pos++]));
        }
        return out;
    }

    // One value as the file spells it. Returns false (and says why) on a type mismatch.
    static bool show(const Value& v, char letter, std::string& out, std::string& why) {
        if (v.is_null()) {
            out = "NULL";
            return true;
        }
        switch (letter) {
            case 'I':
                if (v.type() == cardinal::Type::Int) { out = v.to_string(); return true; }
                if (v.type() == cardinal::Type::Bool) { out = v.as_bool() ? "1" : "0"; return true; }
                break;
            case 'R':
                if (v.type() == cardinal::Type::Int || v.type() == cardinal::Type::Double) {
                    char buf[64];
                    double d = v.type() == cardinal::Type::Int ? static_cast<double>(v.as_int()) : v.as_double();
                    std::snprintf(buf, sizeof buf, "%.3f", d);
                    out = buf;
                    return true;
                }
                break;
            case 'T':
                if (v.type() == cardinal::Type::Text) { out = v.as_text().empty() ? "(empty)" : v.as_text(); return true; }
                break;
        }
        why = std::string("column declared ") + letter + " but got " + cardinal::type_name(v.type()) + " " + v.to_string();
        return false;
    }

    void statement(int at, bool expect_ok) {
        std::string sql = read_sql();
        std::vector<std::string> expected = read_expected();
        std::string error;
        try {
            db.execute(sql);
        } catch (const cardinal::ParseError& e) {
            error = e.what();
        } catch (const cardinal::DbError& e) {
            error = e.what();
        }

        if (expect_ok) {
            if (!error.empty()) fail(at, sql, "expected success, got error: " + error);
        } else if (error.empty()) {
            fail(at, sql, "expected an error, but it succeeded");
        } else if (!expected.empty() && error.find(expected[0]) == std::string::npos) {
            fail(at, sql, "error was \"" + error + "\", expected it to contain \"" + expected[0] + "\"");
        }
    }

    void query(int at, const std::string& header) {
        std::istringstream words(header);
        std::string keyword, types, mode = "nosort";
        words >> keyword >> types;
        std::string extra;
        if (words >> extra) mode = extra;

        std::string sql = read_sql();
        std::vector<std::string> expected = read_expected();

        if (types.empty() || types.find_first_not_of("ITR") != std::string::npos) {
            fail(at, sql, "query needs a type string made of I, T and R");
            return;
        }
        if (mode != "nosort" && mode != "rowsort" && mode != "valuesort") {
            fail(at, sql, "unknown sort mode " + mode);
            return;
        }

        cardinal::QueryResult result;
        try {
            result = db.execute(sql);
        } catch (const cardinal::ParseError& e) {
            fail(at, sql, std::string("error: ") + e.what());
            return;
        } catch (const cardinal::DbError& e) {
            fail(at, sql, std::string("error: ") + e.what());
            return;
        }
        if (!result.returns_rows()) {
            fail(at, sql, "statement returned no rows");
            return;
        }
        // Every query must also give the same answer however the optimizer is set.
        if (cardinal::CheckOutcome checked = cardinal::check_query(db, sql); !checked.ok) {
            fail(at, sql, checked.detail);
            return;
        }
        if (result.columns.size() != types.size()) {
            fail(at, sql, "declared " + std::to_string(types.size()) + " columns, query returns " +
                              std::to_string(result.columns.size()));
            return;
        }

        std::vector<std::vector<std::string>> rows;
        for (const cardinal::Row& row : result.rows) {
            std::vector<std::string> shown;
            for (std::size_t i = 0; i < row.size(); ++i) {
                std::string text, why;
                if (!show(row[i], types[i], text, why)) {
                    fail(at, sql, why);
                    return;
                }
                shown.push_back(std::move(text));
            }
            rows.push_back(std::move(shown));
        }

        std::vector<std::string> actual;
        if (mode == "rowsort") std::sort(rows.begin(), rows.end());
        for (const auto& row : rows) actual.insert(actual.end(), row.begin(), row.end());
        if (mode == "valuesort") std::sort(actual.begin(), actual.end());

        if (actual != expected) {
            std::string what = "expected " + std::to_string(expected.size()) + " values, got " +
                               std::to_string(actual.size()) + "\n  expected:";
            for (const auto& v : expected) what += " " + v;
            what += "\n  actual:  ";
            for (const auto& v : actual) what += " " + v;
            fail(at, sql, what);
        }
    }

    void run() {
        while (!at_end()) {
            std::string line = trim_right(lines[pos]);
            int at = line_number();
            if (blank(line) || line[0] == '#') {
                ++pos;
                continue;
            }
            ++pos;
            ++records;
            if (line == "statement ok") {
                statement(at, true);
            } else if (line == "statement error") {
                statement(at, false);
            } else if (line.rfind("query", 0) == 0) {
                query(at, line);
            } else {
                fail(at, line, "unknown record type");
                // Skip to the next blank line so one mistake doesn't cascade.
                while (!at_end() && !blank(lines[pos])) ++pos;
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cardinal_slt [--no-optimizer] file.test...\n";
        return 2;
    }
    int failed = 0;
    bool optimizer = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-optimizer") {
            optimizer = false;
            continue;
        }
        std::ifstream in(argv[i]);
        if (!in) {
            std::cerr << "cannot open " << argv[i] << "\n";
            return 2;
        }
        Runner runner;
        runner.db.set_optimizer_enabled(optimizer);
        runner.file = argv[i];
        for (std::string line; std::getline(in, line);) runner.lines.push_back(line);
        runner.run();
        failed += runner.failures;
        if (runner.records == 0) {
            std::cerr << argv[i] << ": no records found\n";
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}
