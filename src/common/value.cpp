#include "common/value.h"

#include <charconv>

namespace cardinal {

const char* type_name(Type type) {
    switch (type) {
        case Type::Int: return "INT";
        case Type::Double: return "DOUBLE";
        case Type::Text: return "TEXT";
        case Type::Bool: return "BOOL";
    }
    return "?";
}

Type Value::type() const {
    switch (data_.index()) {
        case 1: return Type::Int;
        case 2: return Type::Double;
        case 3: return Type::Text;
        default: return Type::Bool;
    }
}

namespace {

std::string double_to_string(double v) {
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, end);
    if (s.find_first_of(".einfa") == std::string::npos) s += ".0";
    return s;
}

}  // namespace

std::string Value::to_string() const {
    switch (data_.index()) {
        case 0: return "NULL";
        case 1: return std::to_string(as_int());
        case 2: return double_to_string(as_double());
        case 3: return as_text();
        default: return as_bool() ? "true" : "false";
    }
}

}  // namespace cardinal
