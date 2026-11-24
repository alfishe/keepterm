// TOML output of values and of the effective config (`keepterm --config dump`).
#include <cmath>
#include <locale>
#include <sstream>
#include <string>

#include "config/config.h"

namespace kt::config {
namespace {

std::string fmt_double(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    std::string s;
    const bool fixed = v == 0 || (std::fabs(v) >= 1e-5 && std::fabs(v) < 1e15);
    for (int prec = 1; prec <= 17; ++prec) {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        if (fixed) o << std::fixed;
        o.precision(prec);
        o << v;
        s = o.str();
        std::istringstream in(s);
        in.imbue(std::locale::classic());
        double back = 0;
        in >> back;
        if (back == v) break;
    }
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string quote_str(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (u < 0x20 || u == 0x7f) {
                    static constexpr char hex[] = "0123456789ABCDEF";
                    out += "\\u00";
                    out += hex[u >> 4];
                    out += hex[u & 15];
                } else {
                    out += ch;
                }
        }
    }
    return out + "\"";
}

std::string key_str(std::string_view k) {
    if (k.empty()) return "\"\"";
    for (const char c : k) {
        const bool bare = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!bare) return quote_str(k);
    }
    return std::string(k);
}

constexpr std::string_view kRedacted = "<redacted>";

std::string raw_to_toml(const RawValue& v, bool redact_secrets) {
    struct V {
        bool redact;
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(std::int64_t i) const { return std::to_string(i); }
        std::string operator()(double d) const { return fmt_double(d); }
        std::string operator()(const std::string& s) const { return quote_str(s); }
        std::string operator()(const RawValue::Array& a) const {
            std::string out = "[";
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out += ", ";
                out += raw_to_toml(a[i], redact);
            }
            return out + "]";
        }
        std::string operator()(const RawValue::Table& t) const {
            if (t.empty()) return "{}";
            std::string out = "{ ";
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (i) out += ", ";
                out += key_str(t[i].key) + " = ";
                out += (redact && is_secret_name(t[i].key)) ? quote_str(kRedacted) : raw_to_toml(t[i].value, redact);
            }
            return out + " }";
        }
    };
    return std::visit(V{redact_secrets}, v.v);
}

std::string value_to_toml(const Value& v, ValueType type, bool redact_all_table_values) {
    struct V {
        ValueType type;
        bool redact_all;
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(std::int64_t i) const {
            switch (type) {
                case ValueType::Duration: return quote_str(format_duration(i));
                case ValueType::Size: return quote_str(format_size(i));
                case ValueType::SizeOrOff: return i == 0 ? std::string("\"\"") : quote_str(format_size(i));
                default: return std::to_string(i);
            }
        }
        std::string operator()(double d) const { return fmt_double(d); }
        std::string operator()(const std::string& s) const { return quote_str(s); }
        std::string operator()(const std::vector<std::string>& l) const {
            std::string out = "[";
            for (std::size_t i = 0; i < l.size(); ++i) {
                if (i) out += ", ";
                out += quote_str(l[i]);
            }
            return out + "]";
        }
        std::string operator()(const StringTable& t) const {
            if (t.empty()) return "{}";
            std::string out = "{ ";
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (i) out += ", ";
                const bool hide = redact_all || is_secret_name(t[i].first);
                out += key_str(t[i].first) + " = " + quote_str(hide ? kRedacted : std::string_view(t[i].second));
            }
            return out + " }";
        }
        std::string operator()(const TableList& tl) const {
            std::string out = "[";
            for (std::size_t i = 0; i < tl.size(); ++i) {
                if (i) out += ", ";
                out += raw_to_toml(tl[i], true);
            }
            return out + "]";
        }
    };
    return std::visit(V{type, redact_all_table_values}, v);
}

std::string origin_str(const Origin& o) {
    std::string where;
    switch (o.layer) {
        case Layer::Default: return "default";
        case Layer::Cli: where = "--set"; break;
        default:
            where = o.file;
            if (o.line > 0) where += ":" + std::to_string(o.line);
            break;
    }
    if (o.fallback) where += " (invalid value, default used)";
    return where;
}

std::string_view group_of(std::string_view path) {
    const auto dot = path.find('.');
    return dot == std::string_view::npos ? std::string_view{} : path.substr(0, dot);
}
std::string_view name_in_group(std::string_view path) {
    const auto dot = path.find('.');
    return dot == std::string_view::npos ? path : path.substr(dot + 1);
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

}  // namespace

bool operator==(const RawValue& a, const RawValue& b) { return a.v == b.v; }

const RawValue* RawValue::find(std::string_view key) const {
    if (const auto* t = table())
        for (const auto& e : *t)
            if (e.key == key) return &e.value;
    return nullptr;
}

std::string to_toml(const RawValue& v) { return raw_to_toml(v, false); }
std::string to_toml(const Value& v, ValueType type) { return value_to_toml(v, type, false); }

bool is_secret_name(std::string_view name) {
    std::string l(name);
    for (char& c : l)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return l.find("token") != std::string::npos || l.find("secret") != std::string::npos ||
           l.find("password") != std::string::npos;
}

std::string dump(const Config& cfg, const DumpOptions& opts) {
    std::string out;
    if (opts.defaults_only) {
        out += "# keepterm built-in defaults (schema version " + std::to_string(kSchemaVersion) + ")\n";
    } else {
        out += "# keepterm effective config";
        if (!cfg.profile().empty()) out += " (profile " + cfg.profile() + ")";
        out += "\n";
        for (const auto& f : cfg.files()) out += "# file: " + display_path(f) + "\n";
        if (!cfg.diagnostics().empty())
            out += "# " + std::to_string(cfg.diagnostics().size()) + " problem(s); run --config check\n";
    }

    std::string_view current_group = "\x01";
    auto open_group = [&](std::string_view g) {
        if (g == current_group) return;
        current_group = g;
        if (!g.empty()) out += "\n[" + std::string(g) + "]\n";
        else out += "\n";
    };

    for (const auto& ks : schema()) {
        open_group(group_of(ks.path));
        const std::string name = std::string(name_in_group(ks.path));
        if (opts.defaults_only) {
            out += "# " + std::string(ks.doc);
            if (!ks.values.empty() && ks.type == ValueType::Enum) {
                out += " One of:";
                for (const auto v : ks.values) out += " " + (v.empty() ? std::string("\"\"") : std::string(v));
                out += ".";
            }
            out += " Applies: " + std::string(to_string(ks.apply)) + ".";
            if (!(ks.default_windows == ks.default_macos && ks.default_macos == ks.default_linux)) {
                out += " Default: Windows " + to_toml(ks.default_windows, ks.type) + ", macOS " +
                       to_toml(ks.default_macos, ks.type) + ", Linux " + to_toml(ks.default_linux, ks.type) + ".";
            }
            out += "\n" + name + " = " + to_toml(ks.default_value, ks.type) + "\n";
            continue;
        }
        const bool env = ks.id == KeyId::terminal_env;
        std::string line = name + " = " + value_to_toml(cfg.value(ks.id), ks.type, env);
        if (opts.origins) line = pad(std::move(line), 44) + "  # " + origin_str(cfg.origin(ks.id));
        out += line + "\n";
        if (ks.id == KeyId::keys_passthrough_programs) {
            for (const auto& b : cfg.keys()) {
                std::string l = quote_str(b.command) + " = " + raw_to_toml(b.value, true);
                if (opts.origins) l = pad(std::move(l), 44) + "  # " + origin_str(b.origin);
                out += l + "\n";
            }
        }
    }

    if (!opts.defaults_only) {
        for (const auto& p : cfg.project_overrides()) {
            out += "\n[[projects]]";
            if (opts.origins) out += "  # " + origin_str(p.origin);
            out += "\n";
            if (!p.match_name.empty()) out += "match_name = " + quote_str(p.match_name) + "\n";
            if (!p.match_path.empty()) out += "match_path = " + quote_str(p.match_path) + "\n";
            for (const auto& a : p.values) {
                const auto& ks = spec(a.key);
                std::string l = std::string(ks.path) + " = " + value_to_toml(a.value, ks.type, a.key == KeyId::terminal_env);
                if (opts.origins) l = pad(std::move(l), 44) + "  # " + origin_str(a.origin);
                out += l + "\n";
            }
        }
    }
    return out;
}

}  // namespace kt::config
