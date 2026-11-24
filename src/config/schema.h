// The config key registry, generated from schema.inc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/value.h"

namespace kt::config {

enum class Apply : std::uint8_t { Live, NewPanes, Restart };

enum class ValueType : std::uint8_t {
    Bool,
    Int,
    Float,
    String,
    Enum,
    Color,        // "#RRGGBB", "#RRGGBBAA" or "" (from theme)
    Duration,     // "500ms", "10s", "2m", "1h"; stored as milliseconds (Int)
    Size,         // "8MiB", "1GB", or an integer byte count; stored as bytes (Int)
    SizeOrOff,    // Size, or "" (off, stored as 0)
    List,         // list of strings
    EnumList,     // list of strings from `values`
    ColorList,    // list of colors, at most range.hi entries
    StringMap,    // table string -> string (StringTable)
    TableArray,   // array of tables, raw (TableList) (validated by the owner module)
};

enum class KeyId : std::uint16_t {
#define KT_KEY(id, ...) id,
#include "config/schema.inc"
#undef KT_KEY
};

inline constexpr std::size_t kKeyCount = 0
#define KT_KEY(id, ...) +1
#include "config/schema.inc"
#undef KT_KEY
    ;

// Schema version that this build writes and understands.
inline constexpr int kSchemaVersion = 1;

struct Range {
    double lo = 0, hi = 0;  // durations in ms, sizes in bytes
};

struct KeySpec {
    KeyId id;
    std::string_view path;   // "terminal.scrollback_lines"
    ValueType type;
    Value default_value;     // resolved for the running OS
    Value default_windows, default_macos, default_linux;  // per-OS defaults (equal unless PerOs)
    std::optional<Range> range;
    std::vector<std::string_view> values;  // enum values, in table order
    Apply apply;
    std::string_view section;  // Settings section id, "" = file only
    bool project_overridable;
    std::string_view doc;
};

std::span<const KeySpec> schema();
const KeySpec& spec(KeyId k);
const KeySpec* find_key(std::string_view path);

std::string_view to_string(ValueType t);
std::string_view to_string(Apply a);

// Damerau-Levenshtein distance (optimal string alignment).
std::size_t edit_distance(std::string_view a, std::string_view b);

// "did you mean" for an unknown key path: same table first, then globally (distance <= 2).
std::optional<std::string_view> suggest_key(std::string_view path);

// Parsers shared by validation and the CLI. Return nullopt on malformed text.
std::optional<std::int64_t> parse_duration_ms(std::string_view s);  // "500ms", "1.5s", "2m", "1h"
std::optional<std::int64_t> parse_size_bytes(std::string_view s);   // "8MiB", "1GB", "512KB", "100B"
bool is_color(std::string_view s);                                  // "#RRGGBB" / "#RRGGBBAA"
std::string format_duration(std::int64_t ms);
std::string format_size(std::int64_t bytes);

}  // namespace kt::config
