// Config values. Core: no OS headers, no toml++ types in the interface.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kt::config {

// A raw TOML value for tables that other modules validate ([keys] entries for the command module,
// [[agents.custom]] for the agent module). Dates and times arrive as strings.
struct RawEntry;
struct RawValue {
    using Array = std::vector<RawValue>;
    using Table = std::vector<RawEntry>;  // in file order
    std::variant<bool, std::int64_t, double, std::string, Array, Table> v;

    bool is_table() const { return v.index() == 5; }
    const Table* table() const { return std::get_if<Table>(&v); }
    const RawValue* find(std::string_view key) const;
};
struct RawEntry {
    std::string key;
    RawValue value;
};
bool operator==(const RawValue& a, const RawValue& b);
inline bool operator==(const RawEntry& a, const RawEntry& b) { return a.key == b.key && a.value == b.value; }

// TOML text of a raw value (inline form), for dumps and diagnostics.
std::string to_toml(const RawValue& v);

using StringTable = std::vector<std::pair<std::string, std::string>>;  // sorted by key
using TableList = std::vector<RawValue>;                               // each a RawValue::Table

// The typed value of a schema key. Durations are milliseconds and sizes bytes (Int);
// enums and colors are strings; lists of strings, lists of enums and lists of colors are
// string lists.
using Value = std::variant<bool, std::int64_t, double, std::string, std::vector<std::string>, StringTable, TableList>;

}  // namespace kt::config
