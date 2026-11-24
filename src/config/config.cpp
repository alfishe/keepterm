// Loading, validation, migration, layering and the per-project view.
#include "config/config.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>

#include "config/service.h"  // hash_files
#include "config/toml_reader.h"
#include "platform/paths.h"

namespace kt::config {

namespace fs = std::filesystem;
using detail::PNode;
using detail::PTable;

struct Config::Data {
    std::vector<Value> values;
    std::vector<Origin> origins;
    KeysTable keys;
    std::vector<ProjectOverride> projects;
    std::vector<Assignment> cli;
    std::vector<Diagnostic> diags;
    std::vector<fs::path> files;
    bool rejected = false;
    std::uint64_t generation = 0;
    std::string profile;
    std::uint64_t deps_hash = 0;  // LoadOptions::dependencies contents
};

namespace {

using Level = Diagnostic::Level;

constexpr int kMaxIncludeDepth = 4;
constexpr std::uintmax_t kMaxFileBytes = 1u << 20;  // bigger files are rejected

std::atomic<std::uint64_t> g_generation{0};

// ---------------------------------------------------------------- migrations

struct Migration {
    int to_version;
    enum class Kind { Rename, Move, Transform, Retire } kind;
    std::string_view from, to;
    RawValue (*transform)(const RawValue&);
    std::string_view note;
};

RawValue show_on_tabs_to_tab_badge(const RawValue& v) {
    if (const auto* b = std::get_if<bool>(&v.v)) return RawValue{std::string(*b ? "memory" : "off")};
    return v;  // not a bool: validation of the new key reports it
}

constexpr Migration kMigrations[] = {
    {1, Migration::Kind::Rename, "terminal.option_as_alt", "macos.option_as_alt", nullptr, ""},
    {1, Migration::Kind::Rename, "agents.extra", "agents.custom", nullptr, ""},
    {1, Migration::Kind::Rename, "resources.thresholds.session", "resources.warn_session", nullptr, ""},
    {1, Migration::Kind::Rename, "resources.thresholds.project", "resources.warn_project", nullptr, ""},
    {1, Migration::Kind::Transform, "resources.show_on_tabs", "resources.tab_badge", &show_on_tabs_to_tab_badge,
     "true becomes \"memory\", false \"off\""},
    {1, Migration::Kind::Rename, "resources.warn_above", "resources.warn_project", nullptr, ""},
};

const Migration* find_migration(std::string_view path) {
    for (const auto& m : kMigrations)
        if (m.from == path) return &m;
    return nullptr;
}

// ---------------------------------------------------------------- small helpers

std::string quote(std::string_view s) { return "'" + std::string(s) + "'"; }

std::string join_values(const std::vector<std::string_view>& vs) {
    std::string out;
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (i) out += ", ";
        out += vs[i].empty() ? "\"\"" : std::string(vs[i]);
    }
    return out;
}

bool has_prefix_key(std::string_view prefix) {
    for (const auto& k : schema())
        if (k.path.size() > prefix.size() && k.path.starts_with(prefix) && k.path[prefix.size()] == '.') return true;
    return false;
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

std::string fmt_num(double v) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << v;
    return s.str();
}

// ---------------------------------------------------------------- builder

struct Ctx {
    Layer layer;
    std::string file;  // display form
    ProjectOverride* project = nullptr;  // non-null while reading a [[projects]] entry
};

class Builder {
public:
    explicit Builder(Config::Data& d) : d_(d) {
        const auto s = schema();
        d_.values.reserve(s.size());
        for (const auto& k : s) d_.values.push_back(k.default_value);
        d_.origins.assign(s.size(), Origin{});
    }

    void diag(Level lvl, const Ctx& c, const PNode* at, std::string key, std::string msg, std::string hint = {}) {
        Diagnostic dg;
        dg.level = lvl;
        dg.file = c.file;
        if (at) dg.line = at->line, dg.column = at->column;
        dg.key = std::move(key);
        dg.message = std::move(msg);
        dg.hint = std::move(hint);
        d_.diags.push_back(std::move(dg));
    }

    // ---- files

    void load_file(const fs::path& path, Layer layer, int depth, std::set<fs::path>& visited) {
        d_.files.push_back(path);
        const std::string shown = display_path(path);
        Ctx c{layer, shown, nullptr};
        std::error_code ec;
        if (!fs::exists(path, ec)) return;  // a missing file is no input, not a problem
        const auto size = fs::file_size(path, ec);
        if (ec) {
            d_.rejected = true;
            diag(Level::Error, c, nullptr, {}, "cannot read the file: " + ec.message());
            return;
        }
        if (size > kMaxFileBytes) {
            d_.rejected = true;
            diag(Level::Error, c, nullptr, {}, "the file is larger than 1 MB and was not read");
            return;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            d_.rejected = true;
            diag(Level::Error, c, nullptr, {}, "cannot open the file");
            return;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) {
            d_.rejected = true;
            diag(Level::Error, c, nullptr, {}, "cannot read the file");
            return;
        }
        std::error_code cec;
        fs::path canon = fs::weakly_canonical(path, cec);
        if (cec) canon = path;
        visited.insert(canon);
        load_text(text, path, c, depth, visited);
        visited.erase(canon);
    }

    void load_text(std::string_view text, const fs::path& path, const Ctx& c, int depth, std::set<fs::path>& visited) {
        auto parsed = detail::parse_toml(text, c.file);
        if (const auto* err = std::get_if<detail::ParseError>(&parsed)) {
            d_.rejected = true;
            Diagnostic dg;
            dg.level = Level::Error;
            dg.file = c.file;
            dg.line = err->line;
            dg.column = err->column;
            dg.message = "syntax error: " + err->message;
            d_.diags.push_back(std::move(dg));
            return;
        }
        const PNode& root = std::get<PNode>(parsed);
        const PTable& top = *root.table();

        // version: a file from a newer keepterm is read as usual.
        for (const auto& [k, v] : top) {
            if (k != "version") continue;
            if (const auto* s = v.scalar(); s && std::holds_alternative<std::int64_t>(s->v) &&
                                             std::get<std::int64_t>(s->v) > kSchemaVersion) {
                diag(Level::Info, c, &v, "version",
                     "this file is from a newer keepterm (version " + std::to_string(std::get<std::int64_t>(s->v)) +
                         "); keys this build does not know are ignored");
            }
        }

        // Includes first, in order; this file then wins over them.
        if (!path.empty()) {
            for (const auto& [k, v] : top) {
                if (k != "include") continue;
                const auto* arr = v.array();
                if (!arr) continue;  // the type error is reported by the schema pass
                for (const auto& item : *arr) {
                    const auto* s = item.scalar();
                    if (!s || !std::holds_alternative<std::string>(s->v)) continue;
                    const std::string& rel = std::get<std::string>(s->v);
                    if (rel.find("://") != std::string::npos) {
                        diag(Level::Warning, c, &item, "include", "include cannot reference a URL: " + rel);
                        continue;
                    }
                    fs::path inc = platform::paths::from_utf8(rel);
                    if (inc.is_relative()) inc = path.parent_path() / inc;
                    std::error_code ec;
                    fs::path canon = fs::weakly_canonical(inc, ec);
                    if (ec) canon = inc;
                    if (visited.contains(canon)) {
                        diag(Level::Warning, c, &item, "include", "include cycle: " + rel + " skipped");
                        continue;
                    }
                    if (depth + 1 > kMaxIncludeDepth) {
                        diag(Level::Warning, c, &item, "include",
                             "include depth above " + std::to_string(kMaxIncludeDepth) + ": " + rel + " skipped");
                        continue;
                    }
                    if (!fs::exists(inc, ec)) {
                        d_.files.push_back(inc);  // watched, so creating it triggers a reload
                        diag(Level::Warning, c, &item, "include", "include not found: " + rel);
                        continue;
                    }
                    load_file(inc, Layer::Include, depth + 1, visited);
                }
            }
        }

        walk(top, "", c);
    }

    void load_cli(const std::vector<std::string>& sets) {
        Ctx c{Layer::Cli, "--set", nullptr};
        for (const auto& s : sets) {
            const auto eq = s.find('=');
            if (eq == std::string::npos || eq == 0) {
                Diagnostic dg;
                dg.file = "--set";
                dg.message = "expected key=value: " + s;
                d_.diags.push_back(std::move(dg));
                continue;
            }
            const std::string key = s.substr(0, eq), val = s.substr(eq + 1);
            auto parsed = detail::parse_toml(key + " = " + val, "--set");
            if (std::holds_alternative<detail::ParseError>(parsed)) {
                // Not a TOML value: take it as a bare string (--set font.family=Menlo).
                std::string esc;
                for (const char ch : val) {
                    if (ch == '"' || ch == '\\') esc += '\\';
                    esc += ch;
                }
                parsed = detail::parse_toml(key + " = \"" + esc + "\"", "--set");
            }
            if (const auto* err = std::get_if<detail::ParseError>(&parsed)) {
                Diagnostic dg;
                dg.file = "--set";
                dg.message = "invalid --set " + s + ": " + err->message;
                d_.diags.push_back(std::move(dg));
                continue;
            }
            walk(*std::get<PNode>(parsed).table(), "", c);
        }
    }

    // ---- tree walk

    void walk(const PTable& t, const std::string& prefix, const Ctx& c) {
        for (const auto& [k, node] : t) {
            const std::string path = prefix.empty() ? k : prefix + "." + k;
            if (!c.project && prefix.empty() && k == "keys" && node.table()) {
                read_keys(*node.table(), "", c);
                continue;
            }
            if (!c.project && prefix.empty() && k == "projects") {
                read_projects(node, c);
                continue;
            }
            assign_path(path, node, c);
        }
    }

    void assign_path(std::string path, const PNode& node, const Ctx& c) {
        PNode migrated;
        const PNode* n = &node;
        if (const Migration* m = find_migration(path)) {
            if (m->kind == Migration::Kind::Retire) {
                diag(Level::Info, c, &node, path, quote(path) + " is retired: " + std::string(m->note));
                return;
            }
            std::string msg = quote(path) + " is now " + quote(m->to);
            if (!m->note.empty()) msg += " (" + std::string(m->note) + ")";
            diag(Level::Info, c, &node, path, msg);
            if (m->kind == Migration::Kind::Transform && m->transform) {
                migrated = node;
                migrated.set(m->transform(detail::to_raw(node)));
                n = &migrated;
            }
            path = std::string(m->to);
        }
        const KeySpec* ks = find_key(path);
        if (!ks) {
            if (n->table() && (has_prefix_key(path) || path_is_migration_prefix(path))) {
                walk(*n->table(), path, c);
                return;
            }
            if (n->table() && !n->inline_table) {
                // An unknown table: report each key inside, with suggestions from their full paths.
                walk(*n->table(), path, c);
                return;
            }
            if (has_prefix_key(path)) {
                diag(Level::Warning, c, n, path, quote(path) + " must be a table");
                return;
            }
            std::string hint;
            if (auto s = suggest_key(path)) hint = "did you mean " + quote(*s) + "?";
            diag(Level::Warning, c, n, path, "unknown key " + quote(path), hint);
            return;
        }
        if (ks->id == KeyId::include && c.layer == Layer::Include) {
            // Nested include lists are resolved in load_text; they do not change the main file's list.
            (void)validate(*ks, *n, c);
            return;
        }
        auto v = validate(*ks, *n, c);
        Origin o{c.layer, c.file, n->line, n->column, false};
        Value val;
        if (v) {
            val = std::move(*v);
        } else {
            val = ks->default_value;
            o.fallback = true;
        }
        if (c.project) {
            if (!ks->project_overridable) {
                diag(Level::Warning, c, n, path, quote(path) + " is not allowed in [[projects]]");
                return;
            }
            o.layer = Layer::Project;
            c.project->values.push_back({ks->id, std::move(val), std::move(o)});
            return;
        }
        if (c.layer == Layer::Cli) d_.cli.push_back({ks->id, val, o});
        const auto i = static_cast<std::size_t>(ks->id);
        d_.values[i] = std::move(val);
        d_.origins[i] = std::move(o);
    }

    static bool path_is_migration_prefix(std::string_view path) {
        for (const auto& m : kMigrations)
            if (m.from.size() > path.size() && m.from.starts_with(path) && m.from[path.size()] == '.') return true;
        return false;
    }

    // ---- validation. nullopt: the default applies.

    std::optional<Value> validate(const KeySpec& ks, const PNode& n, const Ctx& c) {
        const std::string path(ks.path);
        auto type_error = [&](std::string_view got) -> std::optional<Value> {
            diag(Level::Warning, c, &n, path,
                 quote(path) + " expects " + std::string(to_string(ks.type)) + ", got " + std::string(got) +
                     "; using the default");
            return std::nullopt;
        };
        const RawValue* s = n.scalar();
        auto kind_name = [&]() -> std::string {
            if (n.table()) return "a table";
            if (n.array()) return "an array";
            switch (s->v.index()) {
                case 0: return "a bool";
                case 1: return "an integer";
                case 2: return "a float";
                case 3: return "a string";
                default: return "a value";
            }
        };
        auto clamp = [&](double v) -> double {
            if (!ks.range) return v;
            if (v < ks.range->lo || v > ks.range->hi) {
                const double cl = std::clamp(v, ks.range->lo, ks.range->hi);
                std::string shown_lo = fmt_num(ks.range->lo), shown_hi = fmt_num(ks.range->hi);
                if (ks.type == ValueType::Duration) {
                    shown_lo = format_duration(static_cast<std::int64_t>(ks.range->lo));
                    shown_hi = format_duration(static_cast<std::int64_t>(ks.range->hi));
                } else if (ks.type == ValueType::Size || ks.type == ValueType::SizeOrOff) {
                    shown_lo = format_size(static_cast<std::int64_t>(ks.range->lo));
                    shown_hi = format_size(static_cast<std::int64_t>(ks.range->hi));
                }
                diag(Level::Warning, c, &n, path,
                     quote(path) + " is out of range " + shown_lo + ".." + shown_hi + "; clamped");
                return cl;
            }
            return v;
        };
        auto string_list = [&](std::vector<std::string>& out) -> bool {
            const auto* a = n.array();
            if (!a) return false;
            for (const auto& e : *a) {
                const auto* es = e.scalar();
                if (!es || !std::holds_alternative<std::string>(es->v)) return false;
                out.push_back(std::get<std::string>(es->v));
            }
            return true;
        };

        switch (ks.type) {
            case ValueType::Bool:
                if (s && std::holds_alternative<bool>(s->v)) return Value{std::get<bool>(s->v)};
                return type_error(kind_name());
            case ValueType::Int: {
                double d = 0;
                if (s && std::holds_alternative<std::int64_t>(s->v)) {
                    d = static_cast<double>(std::get<std::int64_t>(s->v));
                } else if (s && std::holds_alternative<double>(s->v) &&
                           std::get<double>(s->v) == static_cast<double>(static_cast<std::int64_t>(std::get<double>(s->v)))) {
                    d = std::get<double>(s->v);
                } else {
                    return type_error(kind_name());
                }
                if (!ks.range) return Value{static_cast<std::int64_t>(d)};
                return Value{static_cast<std::int64_t>(clamp(d))};
            }
            case ValueType::Float: {
                double d = 0;
                if (s && std::holds_alternative<double>(s->v)) d = std::get<double>(s->v);
                else if (s && std::holds_alternative<std::int64_t>(s->v)) d = static_cast<double>(std::get<std::int64_t>(s->v));
                else return type_error(kind_name());
                if (d != d) return type_error("nan");
                return Value{clamp(d)};
            }
            case ValueType::String:
                if (s && std::holds_alternative<std::string>(s->v)) return Value{std::get<std::string>(s->v)};
                return type_error(kind_name());
            case ValueType::Enum: {
                if (!s || !std::holds_alternative<std::string>(s->v)) return type_error(kind_name());
                const auto& str = std::get<std::string>(s->v);
                if (std::find(ks.values.begin(), ks.values.end(), str) != ks.values.end()) return Value{str};
                diag(Level::Warning, c, &n, path,
                     "unknown value \"" + str + "\" for " + quote(path) + "; using the default",
                     "allowed: " + join_values(ks.values));
                return std::nullopt;
            }
            case ValueType::Color: {
                if (!s || !std::holds_alternative<std::string>(s->v)) return type_error(kind_name());
                const auto& str = std::get<std::string>(s->v);
                if (str.empty() || is_color(str)) return Value{str};
                diag(Level::Warning, c, &n, path,
                     quote(path) + ": \"" + str + "\" is not a color; using the default", "use \"#RRGGBB\" or \"#RRGGBBAA\"");
                return std::nullopt;
            }
            case ValueType::Duration: {
                if (!s || !std::holds_alternative<std::string>(s->v)) return type_error(kind_name());
                const auto ms = parse_duration_ms(std::get<std::string>(s->v));
                if (!ms) {
                    diag(Level::Warning, c, &n, path,
                         quote(path) + ": \"" + std::get<std::string>(s->v) + "\" is not a duration; using the default",
                         "examples: \"500ms\", \"10s\", \"2m\", \"1h\"");
                    return std::nullopt;
                }
                return Value{static_cast<std::int64_t>(clamp(static_cast<double>(*ms)))};
            }
            case ValueType::Size:
            case ValueType::SizeOrOff: {
                std::optional<std::int64_t> b;
                if (s && std::holds_alternative<std::int64_t>(s->v) && std::get<std::int64_t>(s->v) >= 0) {
                    b = std::get<std::int64_t>(s->v);
                } else if (s && std::holds_alternative<std::string>(s->v)) {
                    const auto& str = std::get<std::string>(s->v);
                    if (str.empty() && ks.type == ValueType::SizeOrOff) return Value{std::int64_t{0}};
                    b = parse_size_bytes(str);
                    if (!b) {
                        diag(Level::Warning, c, &n, path,
                             quote(path) + ": \"" + str + "\" is not a size; using the default",
                             "examples: \"512KiB\", \"8MiB\", \"8GB\"");
                        return std::nullopt;
                    }
                } else {
                    return type_error(kind_name());
                }
                return Value{static_cast<std::int64_t>(clamp(static_cast<double>(*b)))};
            }
            case ValueType::List: {
                std::vector<std::string> out;
                if (!string_list(out)) return type_error(n.array() ? "an array with non-string items" : kind_name());
                return Value{std::move(out)};
            }
            case ValueType::EnumList: {
                std::vector<std::string> out;
                if (!string_list(out)) return type_error(n.array() ? "an array with non-string items" : kind_name());
                for (const auto& e : out) {
                    if (std::find(ks.values.begin(), ks.values.end(), e) == ks.values.end()) {
                        diag(Level::Warning, c, &n, path,
                             "unknown value \"" + e + "\" in " + quote(path) + "; using the default",
                             "allowed: " + join_values(ks.values));
                        return std::nullopt;
                    }
                }
                return Value{std::move(out)};
            }
            case ValueType::ColorList: {
                std::vector<std::string> out;
                if (!string_list(out)) return type_error(n.array() ? "an array with non-string items" : kind_name());
                const auto want = ks.range ? static_cast<std::size_t>(ks.range->hi) : out.size();
                if (!out.empty() && out.size() != want) {
                    diag(Level::Warning, c, &n, path,
                         quote(path) + " needs " + std::to_string(want) + " colors, got " + std::to_string(out.size()) +
                             "; using the default");
                    return std::nullopt;
                }
                for (const auto& e : out) {
                    if (!is_color(e)) {
                        diag(Level::Warning, c, &n, path,
                             quote(path) + ": \"" + e + "\" is not a color; using the default",
                             "use \"#RRGGBB\" or \"#RRGGBBAA\"");
                        return std::nullopt;
                    }
                }
                return Value{std::move(out)};
            }
            case ValueType::StringMap: {
                const auto* t = n.table();
                if (!t) return type_error(kind_name());
                StringTable out;
                for (const auto& [k, v] : *t) {
                    const auto* vs = v.scalar();
                    if (!vs || !std::holds_alternative<std::string>(vs->v)) {
                        diag(Level::Warning, c, &v, path + "." + k,
                             quote(path + "." + k) + " must be a string; entry ignored");
                        continue;
                    }
                    out.emplace_back(k, std::get<std::string>(vs->v));
                }
                std::sort(out.begin(), out.end());
                return Value{std::move(out)};
            }
            case ValueType::TableArray: {
                const auto* a = n.array();
                if (!a) return type_error(kind_name());
                TableList out;
                for (const auto& e : *a) {
                    if (!e.table()) return type_error("an array with non-table items");
                    out.push_back(detail::to_raw(e));
                }
                return Value{std::move(out)};
            }
        }
        return std::nullopt;
    }

    // ---- [keys] (the command module parses the chord grammar; here only the shape is checked)

    void read_keys(const PTable& t, const std::string& prefix, const Ctx& c) {
        for (const auto& [k, node] : t) {
            const std::string cmd = prefix.empty() ? k : prefix + "." + k;
            if (cmd == "passthrough_programs") {
                assign_path("keys.passthrough_programs", node, c);
                continue;
            }
            if (node.table() && !node.inline_table) {  // dotted command id written without quotes
                read_keys(*node.table(), cmd, c);
                continue;
            }
            if (!binding_shape_ok(node, c, cmd)) continue;
            KeyBinding b{cmd, detail::to_raw(node), Origin{c.layer, c.file, node.line, node.column, false}};
            auto it = std::lower_bound(d_.keys.begin(), d_.keys.end(), cmd,
                                       [](const KeyBinding& x, const std::string& y) { return x.command < y; });
            if (it != d_.keys.end() && it->command == cmd) *it = std::move(b);
            else d_.keys.insert(it, std::move(b));
        }
    }

    bool binding_shape_ok(const PNode& n, const Ctx& c, const std::string& cmd) {
        const std::string key = "keys." + cmd;
        auto bad = [&](std::string msg) {
            diag(Level::Warning, c, &n, key, std::move(msg) + "; binding ignored",
                 "use a chord string, a list of chords, \"none\", or { keys = [...], global = bool, "
                 "passthrough = bool, when = [...], args = {...} }");
            return false;
        };
        auto is_str = [](const PNode& x) { return x.scalar() && std::holds_alternative<std::string>(x.scalar()->v); };
        auto str_list = [&](const PNode& x) {
            if (!x.array()) return false;
            for (const auto& e : *x.array())
                if (!is_str(e)) return false;
            return true;
        };
        if (is_str(n) || str_list(n)) return true;
        if (const auto* t = n.table()) {
            for (const auto& [f, v] : *t) {
                if (f == "keys") {
                    if (!is_str(v) && !str_list(v)) return bad(quote(key + ".keys") + " must be a chord or a list of chords");
                } else if (f == "global" || f == "passthrough") {
                    if (!v.scalar() || !std::holds_alternative<bool>(v.scalar()->v))
                        return bad(quote(key + "." + f) + " must be a bool");
                } else if (f == "args") {
                    if (!v.table()) return bad(quote(key + ".args") + " must be a table");
                } else if (f == "when") {  // context names are checked by the command module
                    if (!is_str(v) && !str_list(v))
                        return bad(quote(key + ".when") + " must be a context or a list of contexts");
                } else {
                    return bad("unknown field " + quote(f) + " in " + quote(key));
                }
            }
            return true;
        }
        return bad(quote(key) + " has the wrong type");
    }

    // ---- [[projects]]

    void read_projects(const PNode& node, const Ctx& c) {
        const auto* arr = node.array();
        if (!arr) {
            diag(Level::Warning, c, &node, "projects", "'projects' must be an array of tables ([[projects]])");
            return;
        }
        for (const auto& e : *arr) {
            const auto* t = e.table();
            if (!t) {
                diag(Level::Warning, c, &e, "projects", "a [[projects]] entry must be a table");
                continue;
            }
            ProjectOverride po;
            po.origin = Origin{Layer::Project, c.file, e.line, e.column, false};
            PTable rest;
            for (const auto& [k, v] : *t) {
                if (k == "match_name" || k == "match_path") {
                    const auto* s = v.scalar();
                    if (!s || !std::holds_alternative<std::string>(s->v)) {
                        diag(Level::Warning, c, &v, "projects." + k, quote(k) + " must be a string");
                        continue;
                    }
                    (k == "match_name" ? po.match_name : po.match_path) = std::get<std::string>(s->v);
                } else {
                    rest.emplace_back(k, v);
                }
            }
            if (po.match_name.empty() && po.match_path.empty()) {
                diag(Level::Warning, c, &e, "projects",
                     "a [[projects]] entry needs match_name or match_path; entry ignored");
                continue;
            }
            Ctx pc{c.layer, c.file, &po};
            walk(rest, "", pc);
            d_.projects.push_back(std::move(po));
        }
    }

    // ---- checks across keys, after all layers

    void finish() {
        auto& pty = std::get<std::string>(d_.values[static_cast<std::size_t>(KeyId::terminal_pty)]);
        if (pty == "openconsole" && !std::get<bool>(d_.values[static_cast<std::size_t>(KeyId::extended_openconsole)])) {
            const Origin& o = d_.origins[static_cast<std::size_t>(KeyId::terminal_pty)];
            Diagnostic dg;
            dg.level = Level::Warning;
            dg.file = o.file;
            dg.line = o.line;
            dg.column = o.column;
            dg.key = "terminal.pty";
            dg.message = "openconsole needs extended.openconsole = true; using auto";
            d_.diags.push_back(std::move(dg));
            pty = "auto";
        }
#ifndef _WIN32
        for (const KeyId k : {KeyId::extended_openconsole, KeyId::extended_process_cwd, KeyId::extended_nt_process_info}) {
            const auto i = static_cast<std::size_t>(k);
            if (!std::get<bool>(d_.values[i])) continue;
            const Origin& o = d_.origins[i];
            Diagnostic dg;
            dg.level = Level::Info;
            dg.file = o.file;
            dg.line = o.line;
            dg.column = o.column;
            dg.key = std::string(spec(k).path);
            dg.message = quote(dg.key) + " is read on Windows only and is ignored here";
            d_.diags.push_back(std::move(dg));
        }
#endif
    }

private:
    Config::Data& d_;
};

std::shared_ptr<Config::Data> new_data() {
    auto d = std::make_shared<Config::Data>();
    d->generation = ++g_generation;
    return d;
}

}  // namespace

// ---------------------------------------------------------------- Diagnostic

std::string_view to_string(Diagnostic::Level l) {
    switch (l) {
        case Level::Error: return "error";
        case Level::Warning: return "warning";
        case Level::Info: return "info";
    }
    return "?";
}

std::string format(const Diagnostic& d) {
    std::string out = d.file.empty() ? std::string("config") : d.file;
    if (d.line > 0) {
        out += ":" + std::to_string(d.line);
        if (d.column > 0) out += ":" + std::to_string(d.column);
    }
    out += ": ";
    out += to_string(d.level);
    out += ": ";
    out += d.message;
    if (!d.hint.empty()) out += " (" + d.hint + ")";
    return out;
}

// ---------------------------------------------------------------- Config

Config::Config() {
    auto d = new_data();
    Builder b(*d);
    d_ = std::move(d);
}

Config::Config(std::shared_ptr<const Data> d) : d_(std::move(d)) {}

Config Config::load(const LoadOptions& opts) {
    auto d = new_data();
    d->profile = opts.profile;
    Builder b(*d);
    std::set<fs::path> visited;
    if (opts.file) {
        b.load_file(*opts.file, Layer::User, 0, visited);
    } else if (opts.read_user_files) {
        for (const auto& f : user_files(opts.profile)) b.load_file(f, Layer::User, 0, visited);
    }
    b.load_cli(opts.cli_sets);
    b.finish();
    if (opts.dependencies) {
        std::vector<fs::path> deps = opts.dependencies(Config(d));
        std::erase_if(deps, [&](const fs::path& p) { return std::find(d->files.begin(), d->files.end(), p) != d->files.end(); });
        d->deps_hash = deps.empty() ? 0 : hash_files(deps);
        d->files.insert(d->files.end(), deps.begin(), deps.end());
    }
    return Config(std::move(d));
}

Config Config::load_text(std::string_view text, std::string_view name, const std::vector<std::string>& cli_sets) {
    auto d = new_data();
    Builder b(*d);
    std::set<fs::path> visited;
    b.load_text(text, {}, Ctx{Layer::User, std::string(name), nullptr}, 0, visited);
    b.load_cli(cli_sets);
    b.finish();
    return Config(std::move(d));
}

const Value& Config::value(KeyId k) const { return d_->values[static_cast<std::size_t>(k)]; }
const Origin& Config::origin(KeyId k) const { return d_->origins[static_cast<std::size_t>(k)]; }
bool Config::get_bool(KeyId k) const { return std::get<bool>(value(k)); }
std::int64_t Config::get_int(KeyId k) const { return std::get<std::int64_t>(value(k)); }
double Config::get_float(KeyId k) const { return std::get<double>(value(k)); }
const std::string& Config::get_string(KeyId k) const { return std::get<std::string>(value(k)); }
const std::vector<std::string>& Config::get_list(KeyId k) const { return std::get<std::vector<std::string>>(value(k)); }
const StringTable& Config::get_table(KeyId k) const { return std::get<StringTable>(value(k)); }
const TableList& Config::get_tables(KeyId k) const { return std::get<TableList>(value(k)); }
const KeysTable& Config::keys() const { return d_->keys; }
std::span<const ProjectOverride> Config::project_overrides() const { return d_->projects; }
std::span<const Assignment> Config::cli_assignments() const { return d_->cli; }
std::span<const Diagnostic> Config::diagnostics() const { return d_->diags; }
std::span<const fs::path> Config::files() const { return d_->files; }
bool Config::file_rejected() const { return d_->rejected; }
std::uint64_t Config::generation() const { return d_->generation; }
const std::string& Config::profile() const { return d_->profile; }
std::uint64_t Config::dependency_hash() const { return d_->deps_hash; }

bool Config::has_errors() const {
    return std::any_of(d_->diags.begin(), d_->diags.end(), [](const Diagnostic& x) { return x.level == Level::Error; });
}

// ---------------------------------------------------------------- per-project view

namespace {

std::string expand_path(std::string_view in) {
    std::string out;
    std::size_t i = 0;
    if (in.starts_with("~")) {
        out = platform::paths::to_utf8(platform::paths::home());
        i = 1;
    }
    while (i < in.size()) {
        const char ch = in[i];
        if (ch == '$') {
            std::size_t j = i + 1;
            const bool braced = j < in.size() && in[j] == '{';
            if (braced) ++j;
            std::size_t k = j;
            while (k < in.size() && (std::isalnum(static_cast<unsigned char>(in[k])) || in[k] == '_')) ++k;
            if (k > j && (!braced || (k < in.size() && in[k] == '}'))) {
                if (auto v = platform::paths::env_utf8(in.substr(j, k - j))) out += *v;
                i = braced ? k + 1 : k;
                continue;
            }
        } else if (ch == '%') {
            const auto end = in.find('%', i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                if (auto v = platform::paths::env_utf8(in.substr(i + 1, end - i - 1))) {
                    out += *v;
                    i = end + 1;
                    continue;
                }
            }
        }
        out += ch;
        ++i;
    }
    return out;
}

bool glob_match(std::string_view pat, std::string_view s, bool icase) {
    auto eq = [&](char a, char b) {
        if (!icase) return a == b;
        auto lw = [](char x) { return (x >= 'A' && x <= 'Z') ? static_cast<char>(x - 'A' + 'a') : x; };
        return lw(a) == lw(b);
    };
    std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (p < pat.size() && eq(pat[p], s[i])) {
            ++p, ++i;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::string normal_dir(const fs::path& p) {
    std::string s = platform::paths::to_utf8(p.lexically_normal());
#ifdef _WIN32
    for (char& c : s)
        if (c == '\\') c = '/';
#endif
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

bool project_matches(const ProjectOverride& o, const ProjectRef& project) {
    if (!o.match_name.empty()) return lower(o.match_name) == lower(project.name);
    if (o.match_path.empty() || project.folder.empty()) return false;
#ifdef _WIN32
    constexpr bool icase = true;
#else
    constexpr bool icase = false;
#endif
    const std::string pat = normal_dir(platform::paths::from_utf8(expand_path(o.match_path)));
    const std::string dir = normal_dir(project.folder);
    const auto ps = pat.rfind('/'), ds = dir.rfind('/');
    if (ps == std::string::npos || ds == std::string::npos) return glob_match(pat, dir, icase);
    // '*' is allowed in the last segment only.
    const std::string_view pparent(pat.data(), ps), dparent(dir.data(), ds);
    if (pparent.find('*') != std::string_view::npos) return false;
    if (icase ? lower(pparent) != lower(dparent) : pparent != dparent) return false;
    return glob_match(std::string_view(pat).substr(ps + 1), std::string_view(dir).substr(ds + 1), icase);
}

Config resolve_for_project(const Config& base, const ProjectRef& project) {
    const auto& b = base.data();
    const ProjectOverride* hit = nullptr;
    for (const auto& o : b.projects) {
        if (project_matches(o, project)) {
            hit = &o;
            break;
        }
    }
    if (!hit) return base;
    auto d = std::make_shared<Config::Data>(b);
    d->generation = ++g_generation;
    for (const auto& a : hit->values) {
        const auto i = static_cast<std::size_t>(a.key);
        d->values[i] = a.value;
        d->origins[i] = a.origin;
    }
    for (const auto& a : b.cli) {
        const auto i = static_cast<std::size_t>(a.key);
        d->values[i] = a.value;
        d->origins[i] = a.origin;
    }
    return Config(std::move(d));
}

// ---------------------------------------------------------------- diff

bool ConfigDelta::any(Apply a) const {
    if (theme_changed && a == Apply::Live) return true;  // theme files apply live
    return std::any_of(changed.begin(), changed.end(), [a](KeyId k) { return spec(k).apply == a; });
}
bool ConfigDelta::contains(KeyId k) const { return std::find(changed.begin(), changed.end(), k) != changed.end(); }

ConfigDelta diff(const Config& before, const Config& after) {
    ConfigDelta out;
    const auto& a = before.data();
    const auto& b = after.data();
    for (const auto& ks : schema()) {
        const auto i = static_cast<std::size_t>(ks.id);
        if (a.values[i] == b.values[i]) continue;
        out.changed.push_back(ks.id);
        if (ks.path.starts_with("theme.") || ks.path.starts_with("colors.")) out.theme_changed = true;
        if (ks.apply == Apply::Restart) out.needs_restart = true;
    }
    auto same_keys = [](const KeysTable& x, const KeysTable& y) {
        return std::equal(x.begin(), x.end(), y.begin(), y.end(),
                          [](const KeyBinding& p, const KeyBinding& q) { return p.command == q.command && p.value == q.value; });
    };
    if (a.deps_hash != b.deps_hash) out.theme_changed = true;
    out.keys_changed = !same_keys(a.keys, b.keys);
    auto same_projects = [](const std::vector<ProjectOverride>& x, const std::vector<ProjectOverride>& y) {
        return std::equal(x.begin(), x.end(), y.begin(), y.end(), [](const ProjectOverride& p, const ProjectOverride& q) {
            if (p.match_name != q.match_name || p.match_path != q.match_path || p.values.size() != q.values.size())
                return false;
            for (std::size_t i = 0; i < p.values.size(); ++i)
                if (p.values[i].key != q.values[i].key || !(p.values[i].value == q.values[i].value)) return false;
            return true;
        });
    };
    out.projects_changed = !same_projects(a.projects, b.projects);
    return out;
}

// ---------------------------------------------------------------- files

std::vector<fs::path> user_files(std::string_view profile) {
    std::vector<fs::path> out;
    for (const auto& d : platform::paths::extra_config_dirs(profile)) out.push_back(d / "config.toml");
    if (auto d = platform::paths::get(platform::paths::Kind::config, profile)) out.push_back(*d / "config.toml");
    return out;
}

std::string display_path(const fs::path& p) {
    const std::string s = platform::paths::to_utf8(p);
    const std::string h = platform::paths::to_utf8(platform::paths::home());
    if (h.size() > 1 && s.size() > h.size() && s.starts_with(h) && (s[h.size()] == '/' || s[h.size()] == '\\'))
        return "~" + s.substr(h.size());
    return s;
}

}  // namespace kt::config
