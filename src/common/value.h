#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cardinal {

enum class Type { Int, Double, Text, Bool };

const char* type_name(Type type);

// One SQL value, or NULL. `==` is structural (NULL == NULL is true) so tests
// and containers can use it. SQL's three-valued comparison lives in the
// expression evaluator, not here.
class Value {
public:
    Value() = default;  // NULL
    explicit Value(std::int64_t v) : data_(v) {}
    explicit Value(double v) : data_(v) {}
    explicit Value(std::string v) : data_(std::move(v)) {}
    explicit Value(bool v) : data_(v) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(data_); }

    // Only valid when not NULL.
    Type type() const;

    std::int64_t as_int() const { return std::get<std::int64_t>(data_); }
    double as_double() const { return std::get<double>(data_); }
    const std::string& as_text() const { return std::get<std::string>(data_); }
    bool as_bool() const { return std::get<bool>(data_); }

    // How the shell shows it: NULL, true/false, shortest round-trip doubles, text unquoted.
    std::string to_string() const;

    bool operator==(const Value&) const = default;

private:
    std::variant<std::monostate, std::int64_t, double, std::string, bool> data_;
};

using Row = std::vector<Value>;

}  // namespace cardinal
