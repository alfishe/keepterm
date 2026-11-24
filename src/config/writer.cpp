// Settings writes (see writer.h).
#include "config/writer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <locale>
#include <sstream>

#include "config/toml_edit.h"
#include "platform/paths.h"

namespace kt::config {
namespace {

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> split_list(std::string_view s, bool newlines_too) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || (newlines_too && c == '\n')) {
            if (std::string t = trim(cur); !t.empty()) out.push_back(std::move(t));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (std::string t = trim(cur); !t.empty()) out.push_back(std::move(t));
    return out;
}

std::string quote(std::string_view s) {
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

std::string number_text(double d) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(15);
    o << d;
    return o.str();
}

Result<std::string> edit_file(const ConfigWriter& w) { return w.read(); }

// The path of a [keys] entry as the file writes it: a quoted id ("pane.split_right" = ...) or
// the same id as dotted keys (pane.split_right = ... inside [keys]). New entries are quoted.
toml_edit::Path binding_path(std::string_view doc, std::string_view command) {
    toml_edit::Path quoted{"keys", std::string(command)};
    if (toml_edit::find(doc, quoted)) return quoted;
    toml_edit::Path dotted{"keys"};
    for (auto& seg : toml_edit::split_path(command)) dotted.push_back(std::move(seg));
    if (toml_edit::find(doc, dotted)) return dotted;
    return quoted;
}

}  // namespace

std::string value_text(const Value& v, ValueType type) {
    if (const auto* t = std::get_if<StringTable>(&v)) {
        if (t->empty()) return "{}";
        std::string out = "{ ";
        for (size_t i = 0; i < t->size(); ++i) {
            if (i) out += ", ";
            out += toml_edit::format_key((*t)[i].first) + " = " + quote((*t)[i].second);
        }
        return out + " }";
    }
    if (const auto* s = std::get_if<std::string>(&v)) return quote(*s);
    if (const auto* l = std::get_if<std::vector<std::string>>(&v)) {
        std::string out = "[";
        for (size_t i = 0; i < l->size(); ++i) {
            if (i) out += ", ";
            out += quote((*l)[i]);
        }
        return out + "]";
    }
    return to_toml(v, type);
}

Result<Value> parse_input(const KeySpec& spec, std::string_view raw_text) {
    const std::string text = trim(raw_text);
    auto bad = [](std::string m) -> Result<Value> { return make_error(Errc::invalid_argument, std::move(m)); };
    switch (spec.type) {
        case ValueType::Bool:
            if (text == "true" || text == "on" || text == "yes") return Value{true};
            if (text == "false" || text == "off" || text == "no") return Value{false};
            return bad("Enter true or false");
        case ValueType::Int: {
            if (text.empty()) return bad("Enter a whole number");
            std::istringstream in(text);
            in.imbue(std::locale::classic());
            long long n = 0;
            in >> n;
            if (in.fail() || !in.eof()) return bad("Enter a whole number");
            return Value{static_cast<std::int64_t>(n)};
        }
        case ValueType::Float: {
            if (text.empty()) return bad("Enter a number");
            std::istringstream in(text);
            in.imbue(std::locale::classic());
            double d = 0;
            in >> d;
            if (in.fail() || !in.eof() || !std::isfinite(d)) return bad("Enter a number");
            return Value{d};
        }
        case ValueType::String: return Value{text};
        case ValueType::Enum: {
            for (std::string_view v : spec.values) {
                if (v == text) return Value{text};
            }
            std::string all;
            for (std::string_view v : spec.values) all += (all.empty() ? "" : ", ") + std::string(v);
            return bad("Choose one of " + all);
        }
        case ValueType::Color:
            if (text.empty() || is_color(text)) return Value{text};
            return bad("Enter a color as #RRGGBB or #RRGGBBAA");
        case ValueType::Duration: {
            auto ms = parse_duration_ms(text);
            if (!ms) return bad("Enter a duration such as 500ms, 10s or 2m");
            return Value{*ms};
        }
        case ValueType::Size: {
            auto b = parse_size_bytes(text);
            if (!b) return bad("Enter a size such as 512KiB, 8MiB or 1GiB");
            return Value{*b};
        }
        case ValueType::SizeOrOff: {
            if (text.empty() || text == "off") return Value{std::int64_t{0}};
            auto b = parse_size_bytes(text);
            if (!b) return bad("Enter a size such as 512MiB or 2GiB, or leave it empty for off");
            return Value{*b};
        }
        case ValueType::List:
        case ValueType::EnumList:
        case ValueType::ColorList: {
            std::vector<std::string> items = split_list(text, true);
            if (spec.type == ValueType::ColorList) {
                for (const auto& c : items) {
                    if (!is_color(c)) return bad("Not a color: " + c);
                }
            }
            if (spec.type == ValueType::EnumList) {
                for (const auto& c : items) {
                    bool ok = false;
                    for (std::string_view v : spec.values) ok = ok || v == c;
                    if (!ok) return bad("Unknown value: " + c);
                }
            }
            return Value{std::move(items)};
        }
        case ValueType::StringMap: {
            StringTable t;
            for (const std::string& item : split_list(text, true)) {
                const size_t eq = item.find('=');
                if (eq == std::string::npos || eq == 0) return bad("Write each entry as NAME=value");
                t.emplace_back(trim(std::string_view(item).substr(0, eq)), trim(std::string_view(item).substr(eq + 1)));
            }
            std::sort(t.begin(), t.end());
            return Value{std::move(t)};
        }
        case ValueType::TableArray: return bad("Edit this key in the config file");
    }
    return bad("Unsupported value");
}

std::string input_text(const Value& v, ValueType type) {
    struct V {
        ValueType type;
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(std::int64_t i) const {
            switch (type) {
                case ValueType::Duration: return format_duration(i);
                case ValueType::Size: return format_size(i);
                case ValueType::SizeOrOff: return i == 0 ? std::string() : format_size(i);
                default: return std::to_string(i);
            }
        }
        std::string operator()(double d) const { return number_text(d); }
        std::string operator()(const std::string& s) const { return s; }
        std::string operator()(const std::vector<std::string>& l) const {
            std::string out;
            for (const auto& s : l) out += (out.empty() ? "" : ", ") + s;
            return out;
        }
        std::string operator()(const StringTable& t) const {
            std::string out;
            for (const auto& [k, val] : t) out += (out.empty() ? "" : ", ") + k + "=" + val;
            return out;
        }
        std::string operator()(const TableList& t) const { return std::to_string(t.size()) + " entries"; }
    };
    return std::visit(V{type}, v);
}

std::filesystem::path ConfigWriter::target_file(const LoadOptions& opts) {
    if (opts.file) return *opts.file;
    auto files = user_files(opts.profile);
    return files.empty() ? std::filesystem::path("config.toml") : files.back();
}

Result<std::string> ConfigWriter::read() const {
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) return std::string();
    std::ifstream in(file_, std::ios::binary);
    if (!in) return make_error(Errc::os_error, "cannot read " + display_path(file_));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Result<void> ConfigWriter::writable() const {
    auto text = read();
    if (!text) return Unexpected(text.error());
    if (auto ok = toml_edit::check(*text); !ok)
        return make_error(Errc::invalid_argument, display_path(file_) + " has a syntax error (" + ok.error().what +
                                                      "); Settings is read-only until it is fixed");
    return {};
}

Result<void> ConfigWriter::commit(const std::string& before, const std::string& after) {
    if (after == before) return {};
    if (auto ok = write_file_atomic(file_, after); !ok) return ok;
    if (on_written) on_written();
    return {};
}

Result<void> ConfigWriter::set(KeyId k, const Value& v) {
    const KeySpec& s = spec(k);
    if (s.type == ValueType::TableArray) return make_error(Errc::not_supported, "edit " + std::string(s.path) + " in the file");
    if (auto ok = writable(); !ok) return ok;
    auto before = edit_file(*this);
    if (!before) return Unexpected(before.error());
    auto after = toml_edit::set(*before, toml_edit::split_path(s.path), value_text(v, s.type));
    if (!after) return Unexpected(after.error());
    // The loader's verdict on the edited file: no problem with this key, and the value reads back.
    const Config check = Config::load_text(*after, display_path(file_));
    for (const Diagnostic& d : check.diagnostics()) {
        if (d.key == s.path && d.level != Diagnostic::Level::Info) return make_error(Errc::invalid_argument, d.message);
    }
    if (!(check.value(k) == v)) return make_error(Errc::invalid_argument, "the value does not read back as written");
    return commit(*before, *after);
}

Result<void> ConfigWriter::reset(KeyId k) {
    if (auto ok = writable(); !ok) return ok;
    auto before = edit_file(*this);
    if (!before) return Unexpected(before.error());
    auto after = toml_edit::remove(*before, toml_edit::split_path(spec(k).path));
    if (!after) return Unexpected(after.error());
    return commit(*before, *after);
}

Result<void> ConfigWriter::set_binding(std::string_view command, std::string_view toml_value) {
    if (auto ok = writable(); !ok) return ok;
    auto before = edit_file(*this);
    if (!before) return Unexpected(before.error());
    auto after = toml_edit::set(*before, binding_path(*before, command), toml_value);
    if (!after) return Unexpected(after.error());
    return commit(*before, *after);
}

Result<void> ConfigWriter::reset_binding(std::string_view command) {
    if (auto ok = writable(); !ok) return ok;
    auto before = edit_file(*this);
    if (!before) return Unexpected(before.error());
    auto after = toml_edit::remove(*before, binding_path(*before, command));
    if (!after) return Unexpected(after.error());
    return commit(*before, *after);
}

}  // namespace kt::config
