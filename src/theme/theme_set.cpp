#include "theme/theme_set.h"

#include "text/font_source.h"   // css_weight

namespace kt::theme {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

void set_color(const std::string& v, Rgb& out) {
    if (auto c = parse_rgb(v)) out = *c;
}

void set_opt(const std::string& v, std::optional<Rgb>& out) {
    if (auto c = parse_rgb(v)) out = *c;
}

}  // namespace

Selection parse_selection(std::string_view value, const Selection& fallback) {
    value = trim(value);
    if (value.empty()) return fallback;
    Selection s;
    bool paired = false;
    std::string plain;
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        const std::string_view part = trim(value.substr(0, comma));
        value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
        if (part.starts_with("light:")) {
            s.light = std::string(trim(part.substr(6)));
            paired = true;
        } else if (part.starts_with("dark:")) {
            s.dark = std::string(trim(part.substr(5)));
            paired = true;
        } else if (!part.empty()) {
            plain = std::string(part);
        }
    }
    if (!paired) return Selection{plain, plain};
    if (s.light.empty()) s.light = s.dark.empty() ? fallback.light : s.dark;
    if (s.dark.empty()) s.dark = s.light;
    return s;
}

Selection selection(const config::Config& cfg) {
    return parse_selection(cfg.theme_name(), Selection{cfg.theme_light(), cfg.theme_dark()});
}

Appearance effective_appearance(const config::Config& cfg, Appearance os) {
    const std::string& m = cfg.theme_mode();
    if (m == "light") return Appearance::light;
    if (m == "dark") return Appearance::dark;
    return os;
}

Theme with_overrides(Theme t, const config::Config& cfg) {
    set_color(cfg.colors_background(), t.background);
    set_color(cfg.colors_foreground(), t.foreground);
    set_color(cfg.colors_cursor(), t.cursor);
    set_color(cfg.colors_cursor_text(), t.cursor_text);
    set_color(cfg.colors_selection_background(), t.selection_background);
    set_opt(cfg.colors_selection_foreground(), t.selection_foreground);
    set_opt(cfg.colors_accent(), t.accent);
    set_opt(cfg.colors_bold(), t.bold);
    set_opt(cfg.colors_link(), t.link);
    const auto& pal = cfg.colors_palette();
    for (std::size_t i = 0; i < pal.size() && i < 16; ++i) set_color(pal[i], t.palette[i]);
    return t;
}

text::FontSpec font_spec(const config::Config& cfg) {
    text::FontSpec f;
    if (!cfg.font_family().empty()) f.families.push_back(cfg.font_family());
    f.fallback = cfg.font_fallback();
    f.size_pt = static_cast<float>(cfg.font_size());
    f.line_height = static_cast<float>(cfg.font_line_height());
    f.cell_width = static_cast<float>(cfg.font_cell_width());
    f.weight = text::css_weight(cfg.font_weight());
    f.bold_weight = text::css_weight(cfg.font_bold_weight());
    f.ligatures = cfg.font_ligatures();
    f.features = cfg.font_features();
    f.builtin_box_drawing = cfg.font_builtin_box_drawing();
    f.hinting = cfg.font_hinting() == "none" ? text::Hinting::none : text::Hinting::light;
    f.thicken = static_cast<float>(cfg.font_thicken());
    return f;
}

text::FontSpec font_spec_for_scale(const ThemeSet& ts, float scale, float size_pt) {
    text::FontSpec f = ts.font;
    if (size_pt > 0) f.size_pt = size_pt;
#if defined(__APPLE__)
    if (scale < 2.0f) f.thicken = 0.0f;
#else
    (void)scale;
#endif
    return f;
}

ThemeSet make_theme_set(const config::Config& cfg, Appearance os, const ThemeLibrary& lib) {
    ThemeSet ts;
    ts.os_appearance = os;
    ts.appearance = effective_appearance(cfg, os);
    const Selection sel = selection(cfg);
    ts.requested = ts.appearance == Appearance::light ? sel.light : sel.dark;
    const Theme* t = lib.find(ts.requested);
    if (!t) {
        t = &builtin_default(ts.appearance);
        ts.fallback = true;
        config::Diagnostic d;
        d.level = config::Diagnostic::Level::Warning;
        d.key = cfg.theme_name().empty() ? (ts.appearance == Appearance::light ? "theme.light" : "theme.dark")
                                         : "theme.name";
        d.message = "no theme \"" + ts.requested + "\"; using \"" + t->name + "\"";
        d.hint = "keepterm --theme list shows the themes";
        ts.diagnostics.push_back(std::move(d));
    }
    // Problems of the theme file in use (other files' problems stay in the library).
    for (const config::Diagnostic& d : lib.diagnostics()) {
        if (!t->builtin && d.file == t->source) ts.diagnostics.push_back(d);
    }
    ts.theme = with_overrides(*t, cfg);
    ts.bold_is_bright = cfg.theme_bold_is_bright();
    ts.minimum_contrast = cfg.theme_minimum_contrast();
    const std::string& cs = cfg.terminal_cursor_style();
    ts.cursor_shape = cs == "bar" ? vt::CursorShape::Bar : cs == "underline" ? vt::CursorShape::Underline
                                                                             : vt::CursorShape::Block;
    ts.cursor_blink = cfg.terminal_cursor_blink();
    ts.font = font_spec(cfg);
    ts.opacity = cfg.window_opacity();
    ts.blur = cfg.window_blur();
    return ts;
}

}  // namespace kt::theme
