// Theme files: the keepterm TOML format and the Ghostty format of the
// iTerm2-Color-Schemes collection, parsed into one Theme. Core code: no OS calls.
//
// keepterm TOML (`*.toml`):
//
//   name = "Keepterm Dark"
//   appearance = "dark"                 # light | dark; missing: from the background luminance
//   [colors]
//   background = "#302F3A"              # foreground, cursor, cursor_text, selection_background,
//   selection_foreground = ""           #   selection_foreground ("" keeps the cell colors),
//   bold = "#FFFFFF"                    #   bold, link, accent
//   palette = ["#3A3946", ...]          # ANSI 0-15
//   [ui]                                # optional UI color tokens, "#RRGGBB" or "#RRGGBBAA"
//   surface = "#1E2128"
//
// Ghostty (any other file, the name is the file name, as in Ghostty and Macterm):
//
//   palette = 0=#21222c                 # ... 15
//   background = #282a36                # foreground, cursor-color, cursor-text,
//   selection-background = #44475a      #   selection-foreground, bold-color
//
// Missing colors get defaults (xterm palette; cursor = foreground; cursor text = background;
// selection = background mixed 25 % toward the foreground). Problems are diagnostics with
// file:line; only a TOML syntax error or a file without any color rejects the theme.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.h"
#include "theme/color.h"

namespace kt::theme {

enum class Appearance : uint8_t { light, dark };
std::string_view to_string(Appearance);

enum class ThemeFormat : uint8_t { keepterm, ghostty };

struct UiToken {
    std::string name;   // a UI color token: "surface", "border", ...
    Rgba value = 0;
    friend bool operator==(const UiToken&, const UiToken&) = default;
};

struct Theme {
    std::string name;
    Appearance appearance = Appearance::dark;
    std::array<Rgb, 16> palette{};
    Rgb foreground = 0xE0DEF4;
    Rgb background = 0x302F3A;
    Rgb cursor = 0xE0DEF4;
    Rgb cursor_text = 0x302F3A;
    Rgb selection_background = 0x33405A;
    std::optional<Rgb> selection_foreground;   // empty: keep the cell colors
    std::optional<Rgb> bold;                   // bold text in the default foreground
    std::optional<Rgb> link;                   // hyperlinks under the pointer
    std::optional<Rgb> accent;                 // UI accent; empty: the OS accent
    std::vector<UiToken> ui;                   // [ui] overrides, sorted by name

    ThemeFormat format = ThemeFormat::keepterm;
    bool builtin = false;
    std::string source;                        // "built-in" or the display path of the file

    friend bool operator==(const Theme&, const Theme&) = default;
};

struct ParseResult {
    std::optional<Theme> theme;                // empty: rejected (see diagnostics)
    std::vector<config::Diagnostic> diagnostics;
};

// `file` is the display path used in diagnostics and Theme::source.
ParseResult parse_keepterm_theme(std::string_view text, std::string_view file);
// `name` is the theme name (the file name in Ghostty).
ParseResult parse_ghostty_theme(std::string_view text, std::string_view name, std::string_view file);

// The format of a file: `.toml` is keepterm, anything else Ghostty (a file with a `[colors]`
// table header is keepterm too).
ThemeFormat detect_format(std::string_view filename, std::string_view text);
// Detects the format; the Ghostty name is the file name. `filename` is the bare file name.
ParseResult parse_theme_file(std::string_view filename, std::string_view text, std::string_view display_path);

// The xterm palette used for colors a file leaves out.
const std::array<Rgb, 16>& default_palette();
// Token names a [ui] table may set.
std::span<const std::string_view> ui_token_names();
// light when the background's relative luminance is above 0.5.
Appearance appearance_of(Rgb background);

// The theme as a keepterm TOML file (export, `keepterm --theme show NAME --toml`).
std::string to_toml(const Theme&);

// "Tokyo Night" and "TokyoNight" are the same theme: lower case, letters and digits only.
std::string normalize_name(std::string_view name);

}  // namespace kt::theme
