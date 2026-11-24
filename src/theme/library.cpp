#include "theme/library.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>

#include "platform/paths.h"
#include "theme/builtin_data.h"
#include "theme/theme_set.h"

namespace kt::theme {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxThemeFile = 64 * 1024;

struct Alias {
    std::string_view from, to;
};
constexpr Alias kAliases[] = {
    {"Solarized Dark", "iTerm2 Solarized Dark"},   {"Solarized Light", "iTerm2 Solarized Light"},
    {"Tango Dark", "Builtin Tango Dark"},          {"Tango Light", "Builtin Tango Light"},
    {"GitHub Dark", "GitHub Dark Default"},        {"GitHub Light", "GitHub Light Default"},
    {"One Dark", "Atom One Dark"},                 {"One Light", "Atom One Light"},
    {"Everforest Dark", "Everforest Dark Med"},    {"Everforest Light", "Everforest Light Med"},
    {"Everforest", "Everforest Dark Med"},         {"Kanagawa", "Kanagawa Wave"},
    {"Rosé Pine", "Rose Pine"},                    {"Rosé Pine Dawn", "Rose Pine Dawn"},
    {"Ayu Dark", "Ayu"},                           {"Tokyo Night Light", "TokyoNight Day"},
};

bool iless(std::string_view a, std::string_view b) {
    const std::string na = normalize_name(a), nb = normalize_name(b);
    return na != nb ? na < nb : a < b;
}

std::vector<Theme> parse_builtins() {
    std::vector<Theme> out;
    for (std::size_t i = 0; i < detail::kBuiltinFileCount; ++i) {
        const detail::BuiltinFile& f = detail::kBuiltinFiles[i];
        const std::string_view text(reinterpret_cast<const char*>(f.data), f.size);
        ParseResult r = parse_theme_file(f.filename, text, std::string("built-in:") + f.filename);
        if (!r.theme) continue;   // the tests make sure every built-in parses
        r.theme->builtin = true;
        r.theme->source = "built-in";
        out.push_back(std::move(*r.theme));
    }
    std::sort(out.begin(), out.end(), [](const Theme& a, const Theme& b) { return iless(a.name, b.name); });
    return out;
}

// Exact, then normalized, then aliases.
template <class Range>
const Theme* lookup(const Range& themes, std::string_view name, bool aliases) {
    for (const Theme& t : themes) {
        if (t.name == name) return &t;
    }
    const std::string n = normalize_name(name);
    if (n.empty()) return nullptr;
    for (const Theme& t : themes) {
        if (normalize_name(t.name) == n) return &t;
    }
    if (!aliases) return nullptr;
    for (const Alias& a : kAliases) {
        if (normalize_name(a.from) == n) return lookup(themes, a.to, false);
    }
    return nullptr;
}

std::string ext_of(std::string_view filename) {
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    std::string e(filename.substr(dot));
    for (char& c : e) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return e;
}

std::optional<std::string> read_small(const fs::path& p, std::string* why) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) {
        *why = "cannot read the file";
        return std::nullopt;
    }
    if (size > kMaxThemeFile) {
        *why = "the file is larger than 64 KiB";
        return std::nullopt;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        *why = "cannot read the file";
        return std::nullopt;
    }
    std::string s(static_cast<std::size_t>(size), '\0');
    in.read(s.data(), static_cast<std::streamsize>(size));
    s.resize(static_cast<std::size_t>(in.gcount()));
    return s;
}

// Theme files of one folder, sorted by file name.
std::vector<fs::path> theme_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        if (is_theme_file_name(platform::paths::to_utf8(it->path().filename()))) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::span<const Theme> builtin_themes() {
    static const std::vector<Theme> themes = parse_builtins();
    return themes;
}

const Theme& builtin_default(Appearance a) {
    const Theme* t = find_builtin(a == Appearance::light ? "Keepterm Light" : "Keepterm Dark");
    if (t) return *t;
    static const Theme fallback;   // never used: the built-ins always include both
    return fallback;
}

const Theme* find_builtin(std::string_view name) { return lookup(builtin_themes(), name, true); }

bool is_theme_file_name(std::string_view filename) {
    if (filename.empty() || filename.front() == '.' || filename.back() == '~') return false;
    const std::string e = ext_of(filename);
    return e.empty() || e == ".toml" || e == ".conf" || e == ".ghostty";
}

ThemeLibrary ThemeLibrary::load(std::span<const fs::path> dirs) {
    ThemeLibrary lib;
    lib.dirs_.assign(dirs.begin(), dirs.end());
    std::set<std::string> seen;   // normalized names: the first folder wins
    for (const fs::path& dir : dirs) {
        for (const fs::path& file : theme_files(dir)) {
            const std::string display = config::display_path(file);
            std::string why;
            auto text = read_small(file, &why);
            if (!text) {
                config::Diagnostic d;
                d.level = config::Diagnostic::Level::Warning;
                d.file = display;
                d.message = why + "; theme skipped";
                lib.diags_.push_back(std::move(d));
                continue;
            }
            std::string fname = platform::paths::to_utf8(file.filename());
            const std::string e = ext_of(fname);
            ParseResult r;
            if (e == ".conf" || e == ".ghostty") {
                r = parse_ghostty_theme(*text, fname.substr(0, fname.size() - e.size()), display);
            } else {
                r = parse_theme_file(fname, *text, display);
            }
            lib.diags_.insert(lib.diags_.end(), r.diagnostics.begin(), r.diagnostics.end());
            if (!r.theme) continue;
            if (!seen.insert(normalize_name(r.theme->name)).second) continue;
            lib.user_.push_back(std::move(*r.theme));
        }
    }
    return lib;
}

const Theme* ThemeLibrary::find(std::string_view name) const {
    if (const Theme* t = lookup(user_, name, false)) return t;
    if (const Theme* t = find_builtin(name)) {
        // An alias resolves to a built-in that a user theme may shadow.
        if (const Theme* u = lookup(user_, t->name, false)) return u;
        return t;
    }
    return lookup(user_, name, true);
}

std::vector<ThemeInfo> ThemeLibrary::list() const {
    std::vector<ThemeInfo> out;
    std::set<std::string> user_names;
    for (const Theme& t : user_) {
        ThemeInfo i{t.name, t.appearance, false, find_builtin(t.name) != nullptr, t.source, t.format};
        user_names.insert(normalize_name(t.name));
        out.push_back(std::move(i));
    }
    for (const Theme& t : builtin_themes()) {
        if (user_names.contains(normalize_name(t.name))) continue;
        out.push_back(ThemeInfo{t.name, t.appearance, true, false, t.source, t.format});
    }
    std::sort(out.begin(), out.end(), [](const ThemeInfo& a, const ThemeInfo& b) { return iless(a.name, b.name); });
    return out;
}

std::vector<fs::path> user_theme_dirs(std::string_view profile) {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& p) {
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    if (auto d = platform::paths::get(platform::paths::Kind::config, profile)) add(*d / "themes");
    if (!profile.empty()) {
        if (auto d = platform::paths::get(platform::paths::Kind::config)) add(*d / "themes");
    }
    for (const fs::path& d : platform::paths::extra_config_dirs(profile)) add(d / "themes");
    return out;
}

std::vector<fs::path> config_dependencies(const config::Config& cfg) {
    std::vector<fs::path> out;
    const Selection sel = selection(cfg);
    for (const fs::path& dir : user_theme_dirs(cfg.profile())) {
        for (fs::path& f : theme_files(dir)) out.push_back(std::move(f));
        for (const std::string* name : {&sel.light, &sel.dark}) {
            if (name->empty() || name->find_first_of("/\\") != std::string::npos) continue;
            for (const fs::path& cand : {dir / platform::paths::from_utf8(*name + ".toml"), dir / platform::paths::from_utf8(*name)}) {
                if (std::find(out.begin(), out.end(), cand) == out.end()) out.push_back(cand);
            }
        }
    }
    return out;
}

void add_config_dependencies(config::LoadOptions& opts) { opts.dependencies = config_dependencies; }

}  // namespace kt::theme
