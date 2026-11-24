// Schema table construction and value-text helpers.
#include "config/schema.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace kt::config {
namespace {

// ---- helpers used by the rows of schema.inc

struct PerOsDefault {
    Value windows, macos, linux_;
};

struct ListDefault {
    std::vector<std::string> items;
};
template <class... A>
ListDefault List(A... a) {
    return ListDefault{{std::string(a)...}};
}
struct NoTableT {};
struct NoTablesT {};
constexpr NoTableT NoTable{};
constexpr NoTablesT NoTables{};

struct RangeSpec {
    std::optional<Range> num;
    const char* lo_text = nullptr;  // durations and sizes, parsed per type
    const char* hi_text = nullptr;
    const char* values = nullptr;
};
constexpr RangeSpec NoRange{};
RangeSpec Range_(double lo, double hi) { return RangeSpec{config::Range{lo, hi}, nullptr, nullptr, nullptr}; }
RangeSpec Range_(const char* lo, const char* hi) { return RangeSpec{std::nullopt, lo, hi, nullptr}; }
RangeSpec Values(const char* v) { return RangeSpec{std::nullopt, nullptr, nullptr, v}; }

Value to_value(ValueType t, bool v) {
    (void)t;
    return v;
}
Value to_value(ValueType t, int v) {
    if (t == ValueType::Float) return static_cast<double>(v);
    return static_cast<std::int64_t>(v);
}
Value to_value(ValueType, double v) { return v; }
Value to_value(ValueType t, const char* s) {
    switch (t) {
        case ValueType::Duration: {
            const auto ms = parse_duration_ms(s);
            assert(ms);
            return ms.value_or(0);
        }
        case ValueType::Size:
        case ValueType::SizeOrOff: {
            if (!*s) return std::int64_t{0};
            const auto b = parse_size_bytes(s);
            assert(b);
            return b.value_or(0);
        }
        default: return std::string(s);
    }
}
Value to_value(ValueType, const ListDefault& l) { return l.items; }
Value to_value(ValueType, NoTableT) { return StringTable{}; }
Value to_value(ValueType, NoTablesT) { return TableList{}; }

[[maybe_unused]] Value raw(bool v) { return v; }
[[maybe_unused]] Value raw(int v) { return static_cast<std::int64_t>(v); }
[[maybe_unused]] Value raw(double v) { return v; }
[[maybe_unused]] Value raw(const char* v) { return std::string(v); }
template <class W, class M, class L>
PerOsDefault PerOs(W w, M m, L l) {
    return PerOsDefault{raw(w), raw(m), raw(l)};
}
// PerOs values need the key type for conversion; resolved in make_spec.
Value convert(ValueType t, const Value& v) {
    return std::visit(
        [&](const auto& x) -> Value {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::string>) return to_value(t, x.c_str());
            else if constexpr (std::is_same_v<X, double>) return to_value(t, x);
            else if constexpr (std::is_same_v<X, std::int64_t>)
                return t == ValueType::Float ? Value{static_cast<double>(x)} : Value{x};
            else return x;
        },
        v);
}

std::vector<std::string_view> split_values(const char* v) {
    std::vector<std::string_view> out;
    if (!v) return out;
    std::string_view s(v);
    for (;;) {
        const auto bar = s.find('|');
        out.push_back(s.substr(0, bar));
        if (bar == std::string_view::npos) break;
        s.remove_prefix(bar + 1);
    }
    return out;
}

template <class D>
KeySpec make_spec(KeyId id, std::string_view path, ValueType type, const D& def, const RangeSpec& r, Apply apply,
                  std::string_view section, bool p, std::string_view doc) {
    KeySpec k{id, path, type, {}, {}, {}, {}, std::nullopt, {}, apply, section, p, doc};
    if constexpr (std::is_same_v<D, PerOsDefault>) {
        k.default_windows = convert(type, def.windows);
        k.default_macos = convert(type, def.macos);
        k.default_linux = convert(type, def.linux_);
    } else {
        k.default_windows = k.default_macos = k.default_linux = to_value(type, def);
    }
#if defined(_WIN32)
    k.default_value = k.default_windows;
#elif defined(__APPLE__)
    k.default_value = k.default_macos;
#else
    k.default_value = k.default_linux;
#endif
    if (r.num) {
        k.range = r.num;
    } else if (r.lo_text) {
        const bool dur = type == ValueType::Duration;
        const auto lo = dur ? parse_duration_ms(r.lo_text) : parse_size_bytes(r.lo_text);
        const auto hi = dur ? parse_duration_ms(r.hi_text) : parse_size_bytes(r.hi_text);
        assert(lo && hi);
        k.range = config::Range{static_cast<double>(lo.value_or(0)), static_cast<double>(hi.value_or(0))};
    }
    k.values = split_values(r.values);
    return k;
}

#define Range Range_
const std::vector<KeySpec>& table() {
    static const std::vector<KeySpec> t = [] {
        std::vector<KeySpec> v;
        v.reserve(kKeyCount);
#define KT_KEY(id, path, type, def, range, apply, section, p, doc) \
    v.push_back(make_spec(KeyId::id, path, ValueType::type, def, range, Apply::apply, section, p, doc));
#include "config/schema.inc"
#undef KT_KEY
        return v;
    }();
    return t;
}
#undef Range

const std::unordered_map<std::string_view, std::size_t>& index() {
    static const auto m = [] {
        std::unordered_map<std::string_view, std::size_t> out;
        const auto& t = table();
        for (std::size_t i = 0; i < t.size(); ++i) out.emplace(t[i].path, i);
        return out;
    }();
    return m;
}

std::string_view table_of(std::string_view path) {
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : path.substr(0, dot);
}
std::string_view leaf_of(std::string_view path) {
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? path : path.substr(dot + 1);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Splits "1.5s" into 1.5 and "s". Numbers are ASCII and locale-independent.
bool split_number(std::string_view s, double& num, std::string_view& unit) {
    s = trim(s);
    std::size_t i = 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
    if (i == 0) return false;
    // Locale-independent decimal parse (from_chars for double needs macOS 26 in libc++).
    double whole = 0, frac = 0, scale = 1;
    bool dot = false, any_digit = false;
    for (std::size_t k = 0; k < i; ++k) {
        const char c = s[k];
        if (c == '.') {
            if (dot) return false;
            dot = true;
            continue;
        }
        any_digit = true;
        if (!dot) {
            whole = whole * 10 + (c - '0');
        } else {
            scale /= 10;
            frac += (c - '0') * scale;
        }
    }
    if (!any_digit) return false;
    num = whole + frac;
    unit = trim(s.substr(i));
    return std::isfinite(num);
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

}  // namespace

std::span<const KeySpec> schema() { return table(); }

const KeySpec& spec(KeyId k) { return table()[static_cast<std::size_t>(k)]; }

const KeySpec* find_key(std::string_view path) {
    const auto& m = index();
    const auto it = m.find(path);
    return it == m.end() ? nullptr : &table()[it->second];
}

std::string_view to_string(ValueType t) {
    switch (t) {
        case ValueType::Bool: return "bool";
        case ValueType::Int: return "integer";
        case ValueType::Float: return "number";
        case ValueType::String: return "string";
        case ValueType::Enum: return "enum";
        case ValueType::Color: return "color";
        case ValueType::Duration: return "duration";
        case ValueType::Size: return "size";
        case ValueType::SizeOrOff: return "size or \"\"";
        case ValueType::List: return "list of strings";
        case ValueType::EnumList: return "list of enum values";
        case ValueType::ColorList: return "list of colors";
        case ValueType::StringMap: return "table of strings";
        case ValueType::TableArray: return "array of tables";
    }
    return "?";
}

std::string_view to_string(Apply a) {
    switch (a) {
        case Apply::Live: return "live";
        case Apply::NewPanes: return "new panes";
        case Apply::Restart: return "restart";
    }
    return "?";
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<std::size_t> d((n + 1) * (m + 1));
    auto at = [&](std::size_t i, std::size_t j) -> std::size_t& { return d[i * (m + 1) + j]; };
    for (std::size_t i = 0; i <= n; ++i) at(i, 0) = i;
    for (std::size_t j = 0; j <= m; ++j) at(0, j) = j;
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            at(i, j) = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1, at(i - 1, j - 1) + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                at(i, j) = std::min(at(i, j), at(i - 2, j - 2) + 1);
        }
    }
    return at(n, m);
}

std::optional<std::string_view> suggest_key(std::string_view path) {
    const std::string_view tbl = table_of(path), leaf = leaf_of(path);
    std::optional<std::string_view> best;
    std::size_t best_d = 3;
    for (const auto& k : table()) {
        if (table_of(k.path) != tbl) continue;
        const std::size_t d = edit_distance(leaf, leaf_of(k.path));
        if (d < best_d) best_d = d, best = k.path;
    }
    if (best) return best;
    best_d = 3;
    for (const auto& k : table()) {
        const std::size_t d = edit_distance(path, k.path);
        if (d < best_d) best_d = d, best = k.path;
    }
    return best;
}

std::optional<std::int64_t> parse_duration_ms(std::string_view s) {
    double num = 0;
    std::string_view unit;
    if (!split_number(s, num, unit)) return std::nullopt;
    double mul = 0;
    if (unit == "ms") mul = 1;
    else if (unit == "s") mul = 1000;
    else if (unit == "m" || unit == "min") mul = 60'000;
    else if (unit == "h") mul = 3'600'000;
    else return std::nullopt;
    const double ms = num * mul;
    if (ms > 9.0e15) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(ms));
}

std::optional<std::int64_t> parse_size_bytes(std::string_view s) {
    double num = 0;
    std::string_view unit;
    if (!split_number(s, num, unit)) return std::nullopt;
    struct U {
        std::string_view name;
        double mul;
    };
    static constexpr U units[] = {
        {"", 1},           {"B", 1},
        {"KB", 1e3},       {"MB", 1e6},          {"GB", 1e9},           {"TB", 1e12},
        {"K", 1024.0},     {"M", 1048576.0},     {"G", 1073741824.0},
        {"KiB", 1024.0},   {"MiB", 1048576.0},   {"GiB", 1073741824.0}, {"TiB", 1099511627776.0},
    };
    for (const auto& u : units) {
        if (iequals(unit, u.name)) {
            const double b = num * u.mul;
            if (b > 9.0e18) return std::nullopt;
            return static_cast<std::int64_t>(std::llround(b));
        }
    }
    return std::nullopt;
}

bool is_color(std::string_view s) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

std::string format_duration(std::int64_t ms) {
    if (ms != 0 && ms % 3'600'000 == 0) return std::to_string(ms / 3'600'000) + "h";
    if (ms != 0 && ms % 60'000 == 0) return std::to_string(ms / 60'000) + "m";
    if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
    return std::to_string(ms) + "ms";
}

std::string format_size(std::int64_t b) {
    struct U {
        const char* name;
        std::int64_t mul;
    };
    static constexpr U units[] = {{"GiB", 1LL << 30}, {"MiB", 1LL << 20}, {"KiB", 1LL << 10},
                                  {"GB", 1'000'000'000}, {"MB", 1'000'000}, {"KB", 1000}};
    for (const auto& u : units)
        if (b != 0 && b % u.mul == 0) return std::to_string(b / u.mul) + u.name;
    return std::to_string(b) + "B";
}

}  // namespace kt::config
