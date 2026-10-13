#include "exec/aggregate.h"

#include <functional>

#include "common/error.h"
#include "expr/evaluator.h"

namespace cardinal {

namespace {

std::size_t hash_value(const Value& v) {
    if (v.is_null()) return 0x9e3779b9u;
    switch (v.type()) {
        case Type::Int:
        case Type::Double: {
            // 1 and 1.0 are the same group, so they must hash alike.
            double d = v.type() == Type::Int ? static_cast<double>(v.as_int()) : v.as_double();
            if (d == 0) d = 0;  // -0.0 equals 0.0
            return std::hash<double>{}(d);
        }
        case Type::Text: return std::hash<std::string>{}(v.as_text());
        case Type::Bool: return std::hash<bool>{}(v.as_bool());
    }
    return 0;
}

struct KeyHash {
    std::size_t operator()(const std::vector<Value>& key) const {
        std::size_t h = 0;
        for (const Value& v : key) h = h * 1000003u ^ hash_value(v);
        return h;
    }
};

struct KeyEqual {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].is_null() || b[i].is_null()) {
                if (a[i].is_null() != b[i].is_null()) return false;
            } else if (compare_values(a[i], b[i]) != 0) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace

void AggregateState::add(AggFunc func, const Value& value) {
    if (func == AggFunc::CountStar) {
        ++count;
        return;
    }
    if (value.is_null()) return;
    ++count;
    switch (func) {
        case AggFunc::Sum:
        case AggFunc::Avg:
            if (value.type() == Type::Int) {
                if (__builtin_add_overflow(int_sum, value.as_int(), &int_sum)) {
                    // Only a SUM has to stay whole; an AVG falls back to the double total.
                    if (func == AggFunc::Sum) throw DbError("integer overflow");
                    all_int = false;
                }
                double_sum += static_cast<double>(value.as_int());
            } else {
                all_int = false;
                double_sum += value.as_double();
            }
            break;
        case AggFunc::Min:
        case AggFunc::Max:
            if (best.is_null()) {
                best = value;
            } else {
                int c = compare_values(value, best);
                if (func == AggFunc::Min ? c < 0 : c > 0) best = value;
            }
            break;
        default: break;
    }
}

Value AggregateState::result(AggFunc func, const BoundExprPtr& arg) const {
    (void)arg;
    switch (func) {
        case AggFunc::CountStar:
        case AggFunc::Count: return Value(count);
        case AggFunc::Sum:
            if (count == 0) return Value();
            return all_int ? Value(int_sum) : Value(double_sum);
        case AggFunc::Avg:
            if (count == 0) return Value();
            return Value(double_sum / static_cast<double>(count));
        case AggFunc::Min:
        case AggFunc::Max: return best;
    }
    return Value();
}

HashAggregate::HashAggregate(std::unique_ptr<Operator> input, std::vector<BoundExprPtr> keys,
                             std::vector<PhysicalAggregateCall> calls)
    : input_(std::move(input)), keys_(std::move(keys)), calls_(std::move(calls)) {}

std::optional<Row> HashAggregate::produce() {
    if (!loaded_) {
        loaded_ = true;
        struct Group {
            std::vector<Value> key;
            std::vector<AggregateState> states;
        };
        std::vector<Group> groups;
        std::unordered_map<std::vector<Value>, std::size_t, KeyHash, KeyEqual> index;

        if (keys_.empty()) groups.push_back({{}, std::vector<AggregateState>(calls_.size())});

        while (std::optional<Row> row = input_->next()) {
            count_scanned();
            Group* group;
            if (keys_.empty()) {
                group = &groups.front();
            } else {
                std::vector<Value> key;
                key.reserve(keys_.size());
                for (const BoundExprPtr& k : keys_) key.push_back(evaluate(*k, *row));
                auto [it, inserted] = index.try_emplace(key, groups.size());
                if (inserted) groups.push_back({std::move(key), std::vector<AggregateState>(calls_.size())});
                group = &groups[it->second];
            }
            for (std::size_t i = 0; i < calls_.size(); ++i) {
                Value v = calls_[i].arg ? evaluate(*calls_[i].arg, *row) : Value();
                group->states[i].add(calls_[i].func, v);
            }
        }

        output_.reserve(groups.size());
        for (Group& group : groups) {
            Row out = std::move(group.key);
            for (std::size_t i = 0; i < calls_.size(); ++i) out.push_back(group.states[i].result(calls_[i].func, calls_[i].arg));
            output_.push_back(std::move(out));
        }
    }
    if (next_ >= output_.size()) return std::nullopt;
    return std::move(output_[next_++]);
}

}  // namespace cardinal
