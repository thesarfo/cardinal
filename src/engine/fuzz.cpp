#include "engine/fuzz.h"

#include <random>

#include "engine/answer_checker.h"
#include "sql/parser.h"

namespace cardinal {

namespace {

// ---- printing ----

std::string render(const BoolExpr& e) {
    auto wrapped = [](const BoolExpr& kid) {
        return kid.kind == BoolExpr::Kind::Leaf ? kid.text : "(" + render(kid) + ")";
    };
    switch (e.kind) {
        case BoolExpr::Kind::Leaf: return e.text;
        case BoolExpr::Kind::Not: return "NOT " + (e.kids[0].kind == BoolExpr::Kind::Leaf ? "(" + e.kids[0].text + ")" : wrapped(e.kids[0]));
        case BoolExpr::Kind::And:
        case BoolExpr::Kind::Or: {
            std::string out;
            for (const BoolExpr& kid : e.kids) {
                if (!out.empty()) out += e.kind == BoolExpr::Kind::And ? " AND " : " OR ";
                out += wrapped(kid);
            }
            return out;
        }
    }
    return "";
}

std::vector<std::string> clauses(const QuerySpec& q) {
    std::vector<std::string> out;
    std::string select = "SELECT ";
    for (std::size_t i = 0; i < q.select.size(); ++i) select += (i ? ", " : "") + q.select[i];
    out.push_back(select);
    out.push_back("FROM " + q.from_table + " AS " + q.from_alias);
    for (const JoinSpec& j : q.joins) {
        out.push_back(j.on ? "JOIN " + j.table + " AS " + j.alias + " ON " + render(*j.on)
                           : ", " + j.table + " AS " + j.alias);
    }
    if (q.where) out.push_back("WHERE " + render(*q.where));
    if (!q.order_by.empty()) {
        std::string order = "ORDER BY ";
        for (std::size_t i = 0; i < q.order_by.size(); ++i)
            order += (i ? ", " : "") + q.order_by[i].first + (q.order_by[i].second ? " DESC" : "");
        out.push_back(order);
    }
    if (q.limit) out.push_back("LIMIT " + std::to_string(*q.limit));
    return out;
}

// ---- generating ----

BoolExpr leaf(std::string text) {
    BoolExpr e;
    e.text = std::move(text);
    return e;
}

BoolExpr node(BoolExpr::Kind kind, std::vector<BoolExpr> kids) {
    BoolExpr e;
    e.kind = kind;
    e.kids = std::move(kids);
    return e;
}

class Generator {
public:
    explicit Generator(uint64_t seed) : rng_(seed) {}

    QuerySpec query() {
        QuerySpec q;
        int tables = chance(35) ? 1 : 1 + pick(3);
        q.from_table = table_name();
        q.from_alias = "q1";
        aliases_.push_back("q1");
        for (int i = 2; i <= tables; ++i) {
            JoinSpec join{table_name(), "q" + std::to_string(i), std::nullopt};
            aliases_.push_back(join.alias);
            if (chance(70)) join.on = join_condition();
            q.joins.push_back(std::move(join));
        }
        if (chance(10)) {
            q.select = {"*"};
        } else {
            int items = 1 + pick(4);
            for (int i = 0; i < items; ++i) q.select.push_back(chance(20) ? numeric(1) : any_column());
        }
        if (chance(65)) q.where = condition(2);
        if (chance(35)) {
            int keys = 1 + pick(2);
            for (int i = 0; i < keys; ++i) q.order_by.push_back({chance(25) ? numeric(1) : sortable_column(), chance(40)});
        }
        if (chance(25)) q.limit = 1 + pick(5);
        return q;
    }

private:
    int pick(int n) { return static_cast<int>(rng_() % static_cast<uint64_t>(n)); }
    bool chance(int percent) { return pick(100) < percent; }
    template <class T>
    const T& one_of(const std::vector<T>& items) { return items[static_cast<std::size_t>(pick(static_cast<int>(items.size())))]; }

    std::string table_name() { return "t" + std::to_string(1 + pick(3)); }
    std::string alias() { return one_of(aliases_); }
    std::string int_column() { return alias() + "." + one_of<std::string>({"id", "a", "b"}); }
    std::string any_column() { return alias() + "." + one_of<std::string>({"id", "a", "b", "c", "s", "f"}); }
    std::string sortable_column() { return alias() + "." + one_of<std::string>({"id", "a", "b", "c", "s"}); }
    std::string small_int() { return std::to_string(pick(5)); }
    std::string op() { return one_of<std::string>({"=", "<>", "<", "<=", ">", ">="}); }

    // A number-valued expression: an INT or DOUBLE column, a literal, or a sum, difference or product.
    std::string numeric(int depth) {
        if (depth > 0 && chance(30)) {
            std::string a = numeric(depth - 1);
            std::string b = numeric(depth - 1);
            return "(" + a + " " + one_of<std::string>({"+", "-", "*"}) + " " + b + ")";
        }
        switch (pick(4)) {
            case 0: return int_column();
            case 1: return alias() + ".c";
            case 2: return small_int();
            default: return one_of<std::string>({"0.5", "1.5", "2.5"});
        }
    }

    BoolExpr simple_condition() {
        switch (pick(9)) {
            case 0:
            case 1:
            case 2: return leaf(numeric(1) + " " + op() + " " + numeric(1));
            case 3: return leaf(alias() + ".s " + op() + " " + one_of<std::string>({"'x'", "'y'", "'z'", "''"}));
            case 4: return leaf(alias() + ".f");
            case 5: return leaf((chance(50) ? numeric(0) : alias() + ".s") + (chance(50) ? " IS NULL" : " IS NOT NULL"));
            case 6: return leaf(numeric(0) + (chance(30) ? " NOT" : "") + " BETWEEN " + small_int() + " AND " + small_int());
            case 7: {
                std::string list = small_int();
                for (int i = pick(3); i > 0; --i) list += ", " + small_int();
                return leaf(numeric(0) + (chance(30) ? " NOT" : "") + " IN (" + list + ")");
            }
            default: return leaf(one_of<std::string>({"TRUE", "FALSE", "NULL", alias() + ".f = TRUE", alias() + ".s = " + alias() + ".s"}));
        }
    }

    BoolExpr condition(int depth) {
        if (depth == 0 || chance(40)) return simple_condition();
        int kids = 2 + pick(2);
        std::vector<BoolExpr> parts;
        for (int i = 0; i < kids; ++i) parts.push_back(condition(depth - 1));
        if (chance(20)) return node(BoolExpr::Kind::Not, {node(BoolExpr::Kind::And, std::move(parts))});
        return node(chance(60) ? BoolExpr::Kind::And : BoolExpr::Kind::Or, std::move(parts));
    }

    // Something that relates the newest table to an earlier one.
    BoolExpr join_condition() {
        const std::string& newest = aliases_.back();
        const std::string& older = aliases_[static_cast<std::size_t>(pick(static_cast<int>(aliases_.size()) - 1))];
        auto col = [&] { return one_of<std::string>({"id", "a", "b"}); };
        BoolExpr base = leaf(newest + "." + col() + " " + (chance(75) ? "=" : op()) + " " + older + "." + col());
        if (chance(30)) return node(BoolExpr::Kind::And, {base, simple_condition()});
        return base;
    }

    std::mt19937_64 rng_;
    std::vector<std::string> aliases_;
};

// ---- shrinking ----

std::vector<BoolExpr> reductions(const BoolExpr& e) {
    std::vector<BoolExpr> out;
    if (e.kind == BoolExpr::Kind::Leaf) return out;
    for (std::size_t i = 0; i < e.kids.size(); ++i) out.push_back(e.kids[i]);
    if (e.kind != BoolExpr::Kind::Not && e.kids.size() > 2) {
        for (std::size_t i = 0; i < e.kids.size(); ++i) {
            BoolExpr copy = e;
            copy.kids.erase(copy.kids.begin() + static_cast<std::ptrdiff_t>(i));
            out.push_back(std::move(copy));
        }
    }
    for (std::size_t i = 0; i < e.kids.size(); ++i) {
        for (BoolExpr& r : reductions(e.kids[i])) {
            BoolExpr copy = e;
            copy.kids[i] = std::move(r);
            out.push_back(std::move(copy));
        }
    }
    return out;
}

bool still_fails_in(Database& db, const QuerySpec& spec) {
    try {
        return !check_query(db, to_sql(spec)).ok;
    } catch (const ParseError&) {
        return false;
    }
}

}  // namespace

std::string to_sql(const QuerySpec& spec) {
    std::string out;
    for (const std::string& c : clauses(spec)) out += (out.empty() || c[0] == ',' ? "" : " ") + c;
    return out;
}

std::string to_lines(const QuerySpec& spec) {
    std::string out;
    for (const std::string& c : clauses(spec)) out += (out.empty() ? "" : "\n") + c;
    return out;
}

void create_fuzz_tables(Database& db, uint64_t seed) {
    std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ULL);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); };
    auto maybe_null = [&](std::string value) { return pick(100) < 20 ? std::string("NULL") : std::move(value); };

    const char* texts[] = {"'x'", "'y'", "'z'", "''"};
    const char* reals[] = {"0.5", "1.0", "1.5", "2.5"};
    for (int t = 1; t <= 3; ++t) {
        std::string name = "t" + std::to_string(t);
        db.execute("CREATE TABLE " + name + " (id INT, a INT, b INT, c DOUBLE, s TEXT, f BOOL)");
        int rows = pick(100) < 10 ? 0 : 1 + pick(7);
        if (rows == 0) continue;
        std::string insert = "INSERT INTO " + name + " VALUES ";
        for (int r = 1; r <= rows; ++r) {
            insert += std::string(r > 1 ? ", " : "") + "(" + maybe_null(std::to_string(r)) + ", " +
                      maybe_null(std::to_string(pick(4))) + ", " + maybe_null(std::to_string(pick(4))) + ", " +
                      maybe_null(reals[pick(4)]) + ", " + maybe_null(texts[pick(4)]) + ", " +
                      maybe_null(pick(2) ? "TRUE" : "FALSE") + ")";
        }
        db.execute(insert);
    }
}

QuerySpec generate_query(uint64_t seed, int index) {
    return Generator(seed * 1000003ULL + static_cast<uint64_t>(index)).query();
}

std::vector<QuerySpec> simplifications(const QuerySpec& spec) {
    std::vector<QuerySpec> out;
    auto with = [&](auto change) {
        QuerySpec copy = spec;
        change(copy);
        out.push_back(std::move(copy));
    };

    if (spec.limit) with([](QuerySpec& q) { q.limit.reset(); });
    if (!spec.order_by.empty()) {
        with([](QuerySpec& q) { q.order_by.clear(); });
        if (spec.order_by.size() > 1)
            for (std::size_t i = 0; i < spec.order_by.size(); ++i)
                with([i](QuerySpec& q) { q.order_by.erase(q.order_by.begin() + static_cast<std::ptrdiff_t>(i)); });
    }
    if (spec.where) {
        with([](QuerySpec& q) { q.where.reset(); });
        for (BoolExpr& r : reductions(*spec.where)) with([&r](QuerySpec& q) { q.where = r; });
    }
    if (!spec.joins.empty()) with([](QuerySpec& q) { q.joins.pop_back(); });
    for (std::size_t i = 0; i < spec.joins.size(); ++i) {
        if (!spec.joins[i].on) continue;
        with([i](QuerySpec& q) { q.joins[i].on.reset(); });
        for (BoolExpr& r : reductions(*spec.joins[i].on)) with([i, &r](QuerySpec& q) { q.joins[i].on = r; });
    }
    if (spec.select.size() > 1)
        for (std::size_t i = 0; i < spec.select.size(); ++i)
            with([i](QuerySpec& q) { q.select.erase(q.select.begin() + static_cast<std::ptrdiff_t>(i)); });
    if (spec.select.size() == 1 && spec.select[0] == "*") with([](QuerySpec& q) { q.select = {"q1.id"}; });
    return out;
}

QuerySpec shrink(const QuerySpec& spec, const std::function<bool(const QuerySpec&)>& still_fails) {
    QuerySpec current = spec;
    for (bool progress = true; progress;) {
        progress = false;
        for (QuerySpec& candidate : simplifications(current)) {
            if (still_fails(candidate)) {
                current = std::move(candidate);
                progress = true;
                break;
            }
        }
    }
    return current;
}

FuzzReport fuzz(Database& db, uint64_t seed, int count) {
    FuzzReport report;
    for (int i = 0; i < count; ++i) {
        QuerySpec spec = generate_query(seed, i);
        ++report.queries;
        CheckOutcome outcome = check_query(db, to_sql(spec));
        if (outcome.ok) {
            outcome.detail.empty() ? ++report.compared : ++report.skipped;
            continue;
        }
        QuerySpec small = shrink(spec, [&](const QuerySpec& s) { return still_fails_in(db, s); });
        report.failure = FuzzFailure{seed, i, std::move(spec), small, check_query(db, to_sql(small)).detail};
        return report;
    }
    return report;
}

std::string describe(const FuzzFailure& f) {
    std::string out = "seed " + std::to_string(f.seed) + ", query #" + std::to_string(f.index) + "\n";
    out += "original:\n  " + to_sql(f.original) + "\n";
    out += "shrunk:\n";
    std::string lines = to_lines(f.shrunk);
    out += "  ";
    for (char c : lines) {
        out += c;
        if (c == '\n') out += "  ";
    }
    return out + "\n" + f.detail;
}

}  // namespace cardinal
