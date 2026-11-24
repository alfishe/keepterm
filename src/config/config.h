// Config snapshots: load, layering, typed getters, per-project view, diff.
//
// Layers, lowest precedence first:
//   1. built-in defaults (schema.inc, per OS)
//   2. user files: on macOS the XDG file first, then the profile's config.toml; each file's
//      `include` list is merged before the file itself (later wins). `--config <file>`
//      replaces this layer.
//   3. per-project overrides ([[projects]]), only through resolve_for_project()
//   4. command line `--set key=value`
//
// Loading never fails: every problem becomes a Diagnostic and the key keeps its default.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/schema.h"
#include "config/value.h"

namespace kt::config {

struct Diagnostic {
    enum class Level : std::uint8_t { Error, Warning, Info } level = Level::Error;
    std::string file;          // display form, home folder abbreviated; "--set" for the command line
    int line = 0, column = 0;  // 1-based; 0 when unknown
    std::string key;           // "font.size" or empty
    std::string message;       // English
    std::string hint;          // "did you mean 'font.size'?"
};

// "file:line:col: warning: message (hint)"
std::string format(const Diagnostic& d);
std::string_view to_string(Diagnostic::Level l);

enum class Layer : std::uint8_t { Default, User, Include, Project, Cli };

struct Origin {
    Layer layer = Layer::Default;
    std::string file;  // display form
    int line = 0, column = 0;
    bool fallback = false;  // the file's value was invalid; the default applies
};

struct Assignment {
    KeyId key;
    Value value;
    Origin origin;
};

// One raw [keys] entry; the command module parses the chord grammar.
struct KeyBinding {
    std::string command;  // "pane.split_right"
    RawValue value;       // string, list of strings, or a table {keys, global, passthrough, when, args}
    Origin origin;
};
using KeysTable = std::vector<KeyBinding>;  // sorted by command

struct ProjectOverride {
    std::string match_name;  // case-insensitive, exact
    std::string match_path;  // folder; '~' and environment variables expand; '*' in the last segment
    std::vector<Assignment> values;  // only keys marked project_overridable
    Origin origin;
};

struct ProjectRef {
    std::string name;
    std::filesystem::path folder;
};

class Config;

struct LoadOptions {
    std::string profile;                       // "" = default profile
    std::optional<std::filesystem::path> file; // --config <file>: replaces the user files
    std::vector<std::string> cli_sets;         // --set key=value, in order
    bool read_user_files = true;               // false: defaults and --set only
    // Extra files the snapshot depends on, existing or not (the theme files of src/theme,
    // the config). Called at the end of every load with the finished snapshot. The files
    // join files(), so the live reload watches them, and a change of their contents sets
    // ConfigDelta::theme_changed. Empty: none.
    std::function<std::vector<std::filesystem::path>(const Config&)> dependencies;
};

class Config {
public:
    Config();  // built-in defaults

    static Config load(const LoadOptions& opts);
    // Same as load, with the user layer from text (tests; `name` is the display name).
    static Config load_text(std::string_view toml_text, std::string_view name = "config.toml",
                            const std::vector<std::string>& cli_sets = {});

    bool get_bool(KeyId k) const;
    std::int64_t get_int(KeyId k) const;     // Int; Duration in ms; Size in bytes
    double get_float(KeyId k) const;
    const std::string& get_string(KeyId k) const;  // String, Enum, Color
    const std::vector<std::string>& get_list(KeyId k) const;
    const StringTable& get_table(KeyId k) const;
    const TableList& get_tables(KeyId k) const;
    const Value& value(KeyId k) const;
    const Origin& origin(KeyId k) const;

    template <class T>
    const T& get(KeyId k) const {
        return std::get<T>(value(k));
    }

    const KeysTable& keys() const;
    std::span<const ProjectOverride> project_overrides() const;
    std::span<const Assignment> cli_assignments() const;
    std::span<const Diagnostic> diagnostics() const;
    bool has_errors() const;
    // Files this snapshot depends on, existing or not (user files and resolved includes).
    std::span<const std::filesystem::path> files() const;
    // True when a file was rejected (syntax error, unreadable, too big).
    bool file_rejected() const;
    std::uint64_t generation() const;
    const std::string& profile() const;
    // Hash of the contents of the LoadOptions::dependencies files (0 without any).
    std::uint64_t dependency_hash() const;

    // Typed accessors, one per key: cfg.font_size(), cfg.terminal_shell(), ...
#define KT_ACCESSOR_Bool(id) bool id() const { return get_bool(KeyId::id); }
#define KT_ACCESSOR_Int(id) std::int64_t id() const { return get_int(KeyId::id); }
#define KT_ACCESSOR_Float(id) double id() const { return get_float(KeyId::id); }
#define KT_ACCESSOR_String(id) const std::string& id() const { return get_string(KeyId::id); }
#define KT_ACCESSOR_Enum(id) KT_ACCESSOR_String(id)
#define KT_ACCESSOR_Color(id) KT_ACCESSOR_String(id)
#define KT_ACCESSOR_Duration(id) KT_ACCESSOR_Int(id)
#define KT_ACCESSOR_Size(id) KT_ACCESSOR_Int(id)
#define KT_ACCESSOR_SizeOrOff(id) KT_ACCESSOR_Int(id)
#define KT_ACCESSOR_List(id) const std::vector<std::string>& id() const { return get_list(KeyId::id); }
#define KT_ACCESSOR_EnumList(id) KT_ACCESSOR_List(id)
#define KT_ACCESSOR_ColorList(id) KT_ACCESSOR_List(id)
#define KT_ACCESSOR_StringMap(id) const StringTable& id() const { return get_table(KeyId::id); }
#define KT_ACCESSOR_TableArray(id) const TableList& id() const { return get_tables(KeyId::id); }
#define KT_KEY(id, path, type, ...) KT_ACCESSOR_##type(id)
#include "config/schema.inc"
#undef KT_KEY

    struct Data;
    explicit Config(std::shared_ptr<const Data> d);
    const Data& data() const { return *d_; }

private:
    std::shared_ptr<const Data> d_;
};

// The global config with one project's overrides on top (first matching entry), and the
// command line still on top of that.
Config resolve_for_project(const Config& base, const ProjectRef& project);
bool project_matches(const ProjectOverride& o, const ProjectRef& project);

struct ConfigDelta {
    std::vector<KeyId> changed;  // keys whose effective value changed, in schema order
    bool theme_changed = false;  // theme.* or colors.*, or the contents of a dependency file
    bool keys_changed = false;   // [keys] bindings
    bool projects_changed = false;
    bool needs_restart = false;  // some changed key is Apply::Restart
    bool empty() const { return changed.empty() && !keys_changed && !projects_changed && !theme_changed; }
    bool any(Apply a) const;
    bool contains(KeyId k) const;
};
ConfigDelta diff(const Config& before, const Config& after);

// The user files of a profile, lowest precedence first (existing or not).
std::vector<std::filesystem::path> user_files(std::string_view profile);

// Path with the home folder abbreviated to '~'.
std::string display_path(const std::filesystem::path& p);

struct DumpOptions {
    bool defaults_only = false;  // the schema defaults with their doc lines
    bool origins = true;         // a comment with the origin after each value
};
// TOML text of the effective config. terminal.env values and secret-like names are redacted.
std::string dump(const Config& cfg, const DumpOptions& opts = {});

// TOML text of a value (inline form).
std::string to_toml(const Value& v, ValueType type);
// A secret-like name: contains "token", "secret" or "password".
bool is_secret_name(std::string_view name);

}  // namespace kt::config
