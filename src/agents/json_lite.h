// A minimal JSON reader and string writer for the agents module: the agents' session files and the
// AgentResume record. Strict, bounded (depth 32), never throws. Core code.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kt::agents::json {

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;                                     // String
    std::vector<Value> items;                             // Array
    std::vector<std::pair<std::string, Value>> members;   // Object, in file order

    // Declared here, defaulted in the .cpp: inline special members would instantiate the
    // pair<string, Value> of the member list while Value is incomplete (clang with libstdc++ 15).
    Value();
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;
    ~Value();

    // nullptr when absent or when this is not an object. The last duplicate key wins.
    const Value* get(std::string_view key) const;
    // The string member `key`, nullopt when absent or of another kind.
    std::optional<std::string> string_at(std::string_view key) const;
};

// nullopt on any syntax error, trailing garbage, or nesting deeper than 32.
std::optional<Value> parse(std::string_view text);

// Appends `s` as a JSON string literal (with quotes); control characters are \u-escaped.
void write_string(std::string& out, std::string_view s);

}  // namespace kt::agents::json
