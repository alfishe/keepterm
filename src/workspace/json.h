// The JSON value of the workspace state files. Core code, never throws.
//
// Objects keep their keys sorted, so dump() is byte-stable ("sorted keys"). Numbers keep
// the integer/double distinction; doubles are written in the shortest form that reads back to the
// same value. The parser is bounded: depth 128 (split trees nest), input 16 MiB.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kt::ws::json {

class Value {
public:
    enum class Kind : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };

    Value() = default;
    static Value boolean(bool b);
    static Value integer(std::int64_t i);
    static Value number(double d);
    static Value string(std::string s);
    static Value array();
    static Value object();

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_object() const { return kind_ == Kind::Object; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_number() const { return kind_ == Kind::Int || kind_ == Kind::Double; }

    std::optional<bool> as_bool() const;
    std::optional<double> as_double() const;        // Int or Double
    std::optional<std::int64_t> as_int() const;     // Int, or a Double with no fraction
    const std::string* as_string() const;
    const std::vector<Value>* as_array() const;
    std::vector<Value>* as_array();

    // Objects. get() returns nullptr when absent or when this is not an object.
    const Value* get(std::string_view key) const;
    Value* get(std::string_view key);
    Value& set(std::string key, Value v);   // inserts or replaces; turns Null into an object
    bool erase(std::string_view key);
    const std::vector<std::pair<std::string, Value>>& members() const { return members_; }
    std::size_t size() const;               // members or items

    // Arrays.
    Value& push(Value v);                   // turns Null into an array

    std::string dump(bool pretty = true) const;

    bool operator==(const Value& o) const;

private:
    void dump_to(std::string& out, bool pretty, int indent) const;

    Kind kind_ = Kind::Null;
    bool b_ = false;
    std::int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;   // sorted by key
};

struct ParseError {
    std::size_t offset = 0;
    std::string message;
};

// Strict JSON (RFC 8259). Duplicate keys: the last one wins.
bool parse(std::string_view text, Value& out, ParseError* err = nullptr);

// Appends `s` as a JSON string literal.
void write_string(std::string& out, std::string_view s);
// The shortest decimal text of `d` that parses back to `d` (no exponent for ordinary values).
std::string format_double(double d);

}  // namespace kt::ws::json
