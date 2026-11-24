// Applying a ThemeSet to a terminal and its view.
//
// The owner of a pane (the GUI) does, on start and on every config change that touches
// Live / theme. / colors. / font. / terminal.cursor_*, and when the OS appearance changes:
//
//   ThemeSet ts = make_theme_set(cfg, os_appearance, library);
//   theme::apply(ts, terminal);   // parser thread: default colors, cursor shape and blink
//   theme::apply(ts, view, terminal.effective_colors());   // UI thread
//
// and, on TerminalSink::colors_changed (OSC 4/10/11/12 from a program), the second line again,
// so program colors stay on top of the theme. A font change is a new FontSet built from
// font_spec_for_scale(ts, scale) and TerminalView::set_font_set.
#pragma once

#include "theme/theme_set.h"
#include "view/terminal/theme.h"
#include "vt/types.h"

namespace kt::vt {
class Terminal;
}
namespace kt::view {
class TerminalView;
}

namespace kt::theme {

// The theme as the view's colors (theme.minimum_contrast applied to the palette and the
// foreground; bold color and bold-is-bright).
view::Theme to_view_theme(const ThemeSet&);
// The theme as terminal default colors (OSC 4/10/11/12 queries answer with these).
vt::ThemeColors to_vt_colors(const ThemeSet&);
// `base` with a program's colors (Terminal::effective_colors) on top.
view::Theme with_program_colors(view::Theme base, const vt::ThemeColors& effective);

void apply(const ThemeSet&, vt::Terminal&);
// Sets the view's colors: the theme under `effective` (empty optionals keep the theme).
void apply(const ThemeSet&, view::TerminalView&, const vt::ThemeColors& effective = {});

// What changed between two sets, to skip work.
struct Change {
    bool colors = false;   // theme colors (apply to view and terminal)
    bool cursor = false;   // cursor shape or blink (apply to terminal)
    bool font = false;     // FontSpec: rebuild the font set
    bool window = false;   // opacity or blur
    bool any() const { return colors || cursor || font || window; }
};
Change compare(const ThemeSet& before, const ThemeSet& after);

}  // namespace kt::theme
