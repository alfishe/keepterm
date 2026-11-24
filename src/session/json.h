// A small JSON value for the session's metadata files (registry records, record.json, the GUI
// state file). Core code. The parser is strict, bounded (depth 32, 1 MiB input) and never throws.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"

namespace kt::session::json {

class Value {
public:
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value null() { return Value(); }
    static Value boolean(bool b);
    static Value number(double d);
    static Value integer(int64_t i);
    static Value string(std::string s);
    static Value array();
    static Value object();

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }

    // Accessors return nullopt / defaults for the wrong kind.
    std::optional<bool> as_bool() const;
    std::optional<double> as_number() const;
    std::optional<int64_t> as_int() const;
    std::optional<uint64_t> as_uint() const;
    const std::string* as_string() const;
    const std::vector<Value>* as_array() const;

    // Object access. get() returns nullptr when absent or not an object.
    const Value* get(std::string_view key) const;
    Value& set(std::string key, Value v);   // object only
    Value& push(Value v);                    // array only
    const std::vector<std::pair<std::string, Value>>& members() const { return members_; }

    std::string str_or(std::string_view key, std::string_view fallback = {}) const;
    int64_t int_or(std::string_view key, int64_t fallback) const;
    uint64_t uint_or(std::string_view key, uint64_t fallback) const;
    bool bool_or(std::string_view key, bool fallback) const;

    std::string dump(bool pretty = true) const;

private:
    void dump_to(std::string& out, bool pretty, int indent) const;

    Kind kind_ = Kind::Null;
    bool b_ = false;
    double num_ = 0;
    bool is_int_ = false;
    int64_t int_ = 0;
    std::string s_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;  // insertion order
};

Result<Value> parse(std::string_view text);

}  // namespace kt::session::json
