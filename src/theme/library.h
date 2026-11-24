// The themes keepterm knows: built-ins embedded in the binary and the user's
// theme folders, with name lookup and the config hook for live reload.
//
// Built-ins: keepterm's own (src/theme/builtin/*.toml) and a curated set of Ghostty theme
// files from iTerm2-Color-Schemes (third_party/iterm2-color-schemes, MIT collection; each
// scheme's own license is in third_party/MANIFEST.toml). They are parsed once, on first use.
//
// User folders (user_theme_dirs): `<config folder>/themes/` of the profile, then of the default
// profile, then the extra config folders (macOS: $XDG_CONFIG_HOME/keepterm).
// `*.toml` files are keepterm themes; files without an extension, `*.conf` and `*.ghostty` are
// Ghostty themes named after the file, so any iTerm2-Color-Schemes `ghostty/` file can be
// dropped in. The first folder wins; a user theme shadows a built-in of the same name.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "theme/theme.h"

namespace kt::theme {

// Every built-in theme, sorted by name.
std::span<const Theme> builtin_themes();
// "Keepterm Light" or "Keepterm Dark".
const Theme& builtin_default(Appearance);
// The built-in of that name (exact, then normalize_name, then the aliases below), or null.
// Aliases: "Solarized Dark" and "Tango Dark" (the config names) for the iTerm2 file names, "GitHub
// Dark", "One Dark", "Everforest Dark", "Kanagawa", "Rosé Pine" and their light sides.
const Theme* find_builtin(std::string_view name);

struct ThemeInfo {
    std::string name;
    Appearance appearance = Appearance::dark;
    bool builtin = false;
    bool shadows_builtin = false;   // a user theme with the name of a built-in
    std::string source;             // "built-in" or the display path
    ThemeFormat format = ThemeFormat::keepterm;
};

class ThemeLibrary {
public:
    ThemeLibrary() = default;   // built-ins only
    // Built-ins plus the theme files of `dirs` (missing folders are fine).
    static ThemeLibrary load(std::span<const std::filesystem::path> dirs);

    // User themes first (in folder order), then the built-ins. Null when none matches.
    const Theme* find(std::string_view name) const;
    // Every theme by name (case-insensitive), user themes replacing built-ins of the same name.
    std::vector<ThemeInfo> list() const;

    std::span<const Theme> user_themes() const { return user_; }
    std::span<const config::Diagnostic> diagnostics() const { return diags_; }
    std::span<const std::filesystem::path> dirs() const { return dirs_; }

private:
    std::vector<Theme> user_;
    std::vector<config::Diagnostic> diags_;
    std::vector<std::filesystem::path> dirs_;
};

// The user theme folders of a profile (see the header comment).
std::vector<std::filesystem::path> user_theme_dirs(std::string_view profile);

// True for a file name the library reads as a theme.
bool is_theme_file_name(std::string_view filename);

// LoadOptions::dependencies for kt_config: the theme files of the user folders, plus the
// candidate files of the selected names (`<dir>/<name>.toml`, `<dir>/<name>`), so editing,
// adding or removing an active theme reloads the config and sets ConfigDelta::theme_changed.
std::vector<std::filesystem::path> config_dependencies(const config::Config&);
// Sets opts.dependencies = config_dependencies. Call before ConfigService::open.
void add_config_dependencies(config::LoadOptions& opts);

}  // namespace kt::theme
