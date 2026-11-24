#include "theme/apply.h"

#include "view/terminal/terminal_view.h"
#include "vt/terminal.h"

namespace kt::theme {

namespace {

bool same_font(const text::FontSpec& a, const text::FontSpec& b) {
    return a.families == b.families && a.size_pt == b.size_pt && a.line_height == b.line_height &&
           a.features == b.features && a.ligatures == b.ligatures && a.fallback == b.fallback &&
           a.builtin_box_drawing == b.builtin_box_drawing && a.hinting == b.hinting && a.cell_width == b.cell_width &&
           a.weight == b.weight && a.bold_weight == b.bold_weight && a.thicken == b.thicken;
}

}  // namespace

view::Theme to_view_theme(const ThemeSet& ts) {
    const Theme& t = ts.theme;
    view::Theme v;
    v.name = t.name;
    v.background = t.background;
    v.foreground = ensure_contrast(t.foreground, t.background, ts.minimum_contrast);
    for (std::size_t i = 0; i < 16; ++i) v.palette[i] = ensure_contrast(t.palette[i], t.background, ts.minimum_contrast);
    v.cursor = t.cursor;
    v.cursor_text = t.cursor_text;
    v.selection_background = t.selection_background;
    v.selection_foreground = t.selection_foreground;
    if (t.bold) v.bold = ensure_contrast(*t.bold, t.background, ts.minimum_contrast);
    v.bold_is_bright = ts.bold_is_bright;
    return v;
}

vt::ThemeColors to_vt_colors(const ThemeSet& ts) {
    vt::ThemeColors c;
    c.fg = ts.theme.foreground;
    c.bg = ts.theme.background;
    c.cursor = ts.theme.cursor;
    for (std::size_t i = 0; i < 16; ++i) c.palette[i] = ts.theme.palette[i];
    return c;
}

view::Theme with_program_colors(view::Theme base, const vt::ThemeColors& eff) {
    if (eff.fg) base.foreground = *eff.fg;
    if (eff.bg) base.background = *eff.bg;
    if (eff.cursor) base.cursor = *eff.cursor;
    for (std::size_t i = 0; i < 16; ++i) {
        if (eff.palette[i]) base.palette[i] = *eff.palette[i];
    }
    return base;
}

void apply(const ThemeSet& ts, vt::Terminal& term) {
    term.set_theme(to_vt_colors(ts));
    term.set_default_cursor(ts.cursor_shape, ts.cursor_blink);
}

void apply(const ThemeSet& ts, view::TerminalView& view, const vt::ThemeColors& effective) {
    // Entries equal to the theme's own defaults change nothing, so the minimum contrast and
    // the theme's colors survive; real program overrides win.
    const vt::ThemeColors theme_colors = to_vt_colors(ts);
    vt::ThemeColors program;
    if (effective.fg != theme_colors.fg) program.fg = effective.fg;
    if (effective.bg != theme_colors.bg) program.bg = effective.bg;
    if (effective.cursor != theme_colors.cursor) program.cursor = effective.cursor;
    for (std::size_t i = 0; i < 16; ++i) {
        if (effective.palette[i] != theme_colors.palette[i]) program.palette[i] = effective.palette[i];
    }
    view.set_theme(with_program_colors(to_view_theme(ts), program));
}

Change compare(const ThemeSet& a, const ThemeSet& b) {
    Change c;
    c.colors = !(a.theme == b.theme) || a.bold_is_bright != b.bold_is_bright || a.minimum_contrast != b.minimum_contrast;
    c.cursor = a.cursor_shape != b.cursor_shape || a.cursor_blink != b.cursor_blink;
    c.font = !same_font(a.font, b.font);
    c.window = a.opacity != b.opacity || a.blur != b.blur;
    return c;
}

}  // namespace kt::theme
