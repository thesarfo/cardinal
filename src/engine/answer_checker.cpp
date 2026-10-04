#include "engine/answer_checker.h"

#include <algorithm>
#include <exception>

#include "sql/parser.h"

namespace cardinal {

namespace {

enum class Mode { Rows, RowsAndKeys, KeysOnly, CountOnly };

// What a run of the query produced, in the parts that must agree.
struct Observation {
    bool failed = false;
    std::string error;
    std::vector<Row> rows;  // sorted, so order doesn't matter (Rows, RowsAndKeys)
    std::vector<Row> keys;  // in the order the query returned them (RowsAndKeys, KeysOnly)
    std::size_t count = 0;

    bool operator==(const Observation& o) const {
        return failed == o.failed && rows == o.rows && keys == o.keys && count == o.count;
    }
};

std::string row_text(const Row& row) {
    std::string out;
    for (const Value& v : row) out += (v.is_null() ? "N:" : std::string(type_name(v.type())) + ":") + v.to_string() + "|";
    return out;
}

void sort_rows(std::vector<Row>& rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return row_text(a) < row_text(b); });
}

std::string describe(const Observation& o) {
    if (o.failed) return "error: " + o.error;
    std::string out = std::to_string(o.count) + " rows";
    auto list = [&](const char* label, const std::vector<Row>& rows) {
        if (rows.empty()) return;
        out += std::string(" ") + label + ":";
        std::size_t shown = 0;
        for (const Row& r : rows) {
            if (++shown > 8) { out += " ..."; break; }
            out += " (" + row_text(r) + ")";
        }
    };
    list("rows", o.rows);
    list("keys", o.keys);
    return out;
}

Mode mode_of(const Select& select) {
    if (!select.order_by.empty()) return select.limit ? Mode::KeysOnly : Mode::RowsAndKeys;
    return select.limit ? Mode::CountOnly : Mode::Rows;
}

// The same query selecting only its sort keys, so the keys of the rows it returns can be compared.
Statement key_query(const Select& select) {
    Select keys = clone(select);
    keys.items.clear();
    for (const OrderKey& key : select.order_by) keys.items.push_back({clone(*key.expr)});
    return Statement{std::move(keys)};
}

Observation observe(Database& db, const Statement& rows_stmt, const Statement& keys_stmt, Mode mode) {
    Observation o;
    try {
        if (mode == Mode::Rows || mode == Mode::RowsAndKeys || mode == Mode::CountOnly) {
            QueryResult r = db.execute(rows_stmt);
            o.count = r.rows.size();
            if (mode != Mode::CountOnly) {
                o.rows = std::move(r.rows);
                sort_rows(o.rows);
            }
        }
        if (mode == Mode::RowsAndKeys || mode == Mode::KeysOnly) {
            QueryResult r = db.execute(keys_stmt);
            o.count = r.rows.size();
            o.keys = std::move(r.rows);
        }
    } catch (const std::exception& e) {
        o = Observation{};
        o.failed = true;
        o.error = e.what();
    }
    return o;
}

// Puts the database's settings back when the check is over, however it ends.
class RestoreSettings {
public:
    explicit RestoreSettings(Database& db) : db_(db), enabled_(db.optimizer_enabled()) {}
    ~RestoreSettings() {
        db_.set_disabled_rules({});
        db_.set_optimizer_enabled(enabled_);
    }

private:
    Database& db_;
    bool enabled_;
};

}  // namespace

CheckOutcome check_query(Database& db, std::string_view sql) {
    Statement statement = parse_statement(sql);
    const auto* select = std::get_if<Select>(&statement.node);
    if (!select) return {true, "not a SELECT, nothing to compare", {}};

    const Mode mode = mode_of(*select);
    Statement keys = mode == Mode::Rows || mode == Mode::CountOnly ? Statement{clone(*select)} : key_query(*select);

    RestoreSettings restore(db);
    auto run = [&](bool optimize, std::set<std::string> disabled) {
        db.set_optimizer_enabled(optimize);
        db.set_disabled_rules(std::move(disabled));
        return observe(db, statement, keys, mode);
    };

    const Observation reference = run(false, {});
    if (reference.failed) return {true, "the query fails without the optimizer, so it was not compared: " + reference.error, {}};

    struct Setup {
        std::string name;
        Observation seen;
    };
    std::vector<Setup> wrong;
    const Observation all_on = run(true, {});
    if (!(all_on == reference)) wrong.push_back({"all rules on", all_on});

    std::vector<std::string> culprits;
    for (const std::string& rule : db.rule_names()) {
        Observation without = run(true, {rule});
        if (!(without == reference)) {
            wrong.push_back({"all rules but " + rule, without});
        } else if (!(all_on == reference)) {
            culprits.push_back(rule);
        }
    }

    if (wrong.empty()) return {};

    CheckOutcome outcome;
    outcome.ok = false;
    outcome.culprits = culprits;
    outcome.detail = "different answers for: " + std::string(sql) + "\n  without the optimizer: " + describe(reference);
    for (const Setup& s : wrong) outcome.detail += "\n  " + s.name + ": " + describe(s.seen);
    if (!culprits.empty()) {
        outcome.detail += "\n  turning this off fixes it:";
        for (const std::string& c : culprits) outcome.detail += " " + c;
    }
    return outcome;
}

}  // namespace cardinal
