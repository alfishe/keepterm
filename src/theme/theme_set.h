// The resolved look of a terminal pane (config [theme], [colors], [font]):
// the active theme side with the [colors] overrides, the cursor, and the font spec.
//
// Theme selection, Macterm / Ghostty style:
//   theme.name = "Dracula"                      one theme for both appearances
//   theme.name = "light:Rose Pine Dawn,dark:Rose Pine"   a pair (order free; a missing side
//                                                         uses the other)
//   theme.name = "" (default)                   theme.light and theme.dark
// theme.mode picks the side: "system" follows the OS appearance, "light" / "dark" pin it.
// A name that no theme has falls back to the built-in default of that side, with a warning.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "text/font.h"
#include "theme/library.h"
#include "theme/theme.h"
#include "vt/types.h"

namespace kt::theme {

struct Selection {
    std::string light, dark;
};
// Parses a theme.name value; empty sides take `fallback`'s.
Selection parse_selection(std::string_view value, const Selection& fallback);
// theme.name, else theme.light / theme.dark.
Selection selection(const config::Config&);
// The side in use for theme.mode and the OS appearance.
Appearance effective_appearance(const config::Config&, Appearance os);

struct ThemeSet {
    Theme theme;                          // the active side, [colors] overrides applied
    Appearance appearance = Appearance::dark;   // the side in use
    Appearance os_appearance = Appearance::dark;
    std::string requested;                // the theme name asked for
    bool fallback = false;                // `requested` was not found: built-in default
    bool bold_is_bright = false;          // theme.bold_is_bright
    double minimum_contrast = 1.0;        // theme.minimum_contrast
    vt::CursorShape cursor_shape = vt::CursorShape::Block;   // terminal.cursor_style
    bool cursor_blink = true;             // terminal.cursor_blink
    text::FontSpec font;                  // font.* at the configured size; see font_spec_for_scale
    double opacity = 1.0;                 // window.opacity (the UI applies it)
    bool blur = false;                    // window.blur
    std::vector<config::Diagnostic> diagnostics;   // theme lookup and theme file problems
};

ThemeSet make_theme_set(const config::Config&, Appearance os, const ThemeLibrary&);

// The [colors] keys of `cfg` on top of `t`.
Theme with_overrides(Theme t, const config::Config& cfg);

// The font spec of the font.* keys: family, fallback, size, line height, cell width, weights,
// ligatures, features, box drawing, hinting, thicken.
text::FontSpec font_spec(const config::Config&);
// The spec for a font set at `scale`: on macOS font.thicken applies only at scale 2 and above
//; `size_pt` > 0 replaces the configured size (zoom).
text::FontSpec font_spec_for_scale(const ThemeSet&, float scale, float size_pt = 0);

}  // namespace kt::theme
