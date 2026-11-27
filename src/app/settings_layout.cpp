#include "app/settings_layout.h"

#include <cctype>
#include <set>
#include <string>

namespace kt::app::settings {

namespace {

using C = Control;

std::vector<PaneDef> build() {
    std::vector<PaneDef> p;

    p.push_back({"general", "General", ui::Icon::gear, Special::General,
                 {
                     {"Startup",
                      {
                          {"general.startup", "At launch", "Clean: old sessions keep running, detached.",
                           C::Segmented, 0, {}, kAll, "restore=Restore|clean=Start clean|ask=Ask"},
                          {"agents.restore", "Restore agent sessions", "Reopen Claude Code and other recognized agents where they were."},
                          {"session.history", "Restore terminal output", "Keep each terminal's scrollback across restarts."},
                          {"session.history_size", "History per terminal", "", C::Text, 0, "session.history"},
                          {"sessions.restart_dead_sessions", "Restart exited sessions", "Start a new shell in restored panes whose program had exited."},
                          {"sessions.show_outdated_badge", "Mark outdated sessions", "A badge on panes restored from an older keepterm."},
                          {"sessions.report_orphans", "Report orphaned processes", ""},
                      }},
                     {"Closing",
                      {
                          {"general.confirm_quit", "Confirm before quitting", "", C::Segmented, 0, {}, kAll,
                           "always=Always|busy=When busy|never=Never"},
                          {"window.confirm_close", "Confirm before closing a window", "", C::Segmented, 0, {}, kAll,
                           "always=Always|busy=When busy|never=Never"},
                          {"tabs.close_last_pane_closes_tab", "Closing the last pane closes the tab", ""},
                      }},
                     {"Shell",
                      {
                          {"terminal.shell", "Shell", "Empty: the login shell.", C::Text},
                          {"terminal.shell_args", "Shell arguments", "Comma separated.", C::Text},
                          {"terminal.new_tab_directory", "New tabs open in", "", C::Segmented, 0, {}, kAll,
                           "project=Project folder|inherit=Current folder|home=Home"},
                          {"terminal.new_split_directory", "New splits open in", "", C::Segmented, 0, {}, kAll,
                           "project=Project folder|inherit=Current folder|home=Home"},
                          {"general.open_folder_as", "Opening a folder creates", "", C::Segmented, 0, {}, kAll,
                           "project=A project|tab=A tab"},
                      }},
                     {"Language", {{"general.language", "Language", "Empty: the system language.", C::Text}}},
                 }});

    p.push_back({"appearance", "Appearance", ui::Icon::palette, Special::Appearance,
                 {
                     {"Theme",
                      {
                          {"theme.mode", "Appearance", "", C::Segmented, 0, {}, kAll, "system=System|light=Light|dark=Dark"},
                          {"theme.minimum_contrast", "Minimum contrast", "1 keeps the theme's colors.", C::Slider, 0.5},
                          {"theme.bold_is_bright", "Bold text uses bright colors", ""},
                      }},
                     {"Font",
                      {
                          {"font.family", "Font", "", C::FontFamily},
                          {"font.size", "Size", "", C::Stepper, 0.5},
                          {"font.weight", "Weight", "", C::Dropdown},
                          {"font.bold_weight", "Bold weight", "", C::Dropdown},
                          {"font.line_height", "Line height", "", C::Slider, 0.05},
                          {"font.cell_width", "Character width", "", C::Slider, 0.05},
                          {"font.ligatures", "Ligatures", ""},
                          {"font.cursor_ligature_break", "Break ligatures under the cursor", "", C::Auto, 0, "font.ligatures"},
                          {"font.features", "OpenType features", "Comma separated, for example ss01, zero.", C::Text},
                          {"font.fallback", "Fallback fonts", "Comma separated, tried in order.", C::Text},
                          {"font.builtin_box_drawing", "Draw box and block characters", "Pixel-exact lines instead of the font's glyphs."},
                          {"font.ambiguous_width", "Ambiguous-width characters", "", C::Segmented},
                          {"font.cjk_lang", "CJK glyph variant", "", C::Dropdown, 0, {}, kAll,
                           "=Automatic|ja=Japanese|ko=Korean|zh-Hans=Simplified Chinese|zh-Hant=Traditional Chinese"},
                          {"font.antialias", "Antialiasing", "", C::Segmented},
                      }},
                     {"Window",
                      {
                          {"window.opacity", "Opacity", "", C::Percent, 0.01},
                          {"window.blur", "Blur behind", "", C::Auto, 0, {}, kMac | kWindows},
                          {"window.padding_x", "Horizontal padding", "", C::Stepper, 1},
                          {"window.padding_y", "Vertical padding", "", C::Stepper, 1},
                          {"window.unfocused_split_opacity", "Unfocused split opacity", "", C::Percent, 0.01},
                          {"window.titlebar", "Title bar", "", C::Segmented, 0, {}, kWindows | kLinux},
                          {"window.show_title_bar", "Show the title bar", ""},
                          {"ui.font_scale", "Interface text size", "", C::Percent, 0.05},
                      }},
                     {"Sidebar",
                      {
                          {"sidebar.tab_icon", "Tab icon", "", C::Dropdown},
                          {"sidebar.icon_size", "Icon size", "", C::Segmented},
                          {"sidebar.project_icon", "Project icon", "A symbol name or an emoji; empty for the default.", C::Text},
                          {"sidebar.show_agent_icons", "Show agent icons", ""},
                          {"sidebar.spinner_over_agent_icons", "Spinner over agent icons", "", C::Auto, 0, "sidebar.show_agent_icons"},
                          {"sidebar.show_status", "Show status", ""},
                          {"sidebar.show_new_project_button", "Show the New Project button", ""},
                          {"sidebar.show_project_new_tab_button", "Show New Tab on projects", ""},
                          {"sidebar.auto_assign_project_colors", "Color new projects", ""},
                          {"sidebar.peek_when_hidden", "Peek when hidden", "Show the sidebar while the pointer is at the window edge."},
                          {"sidebar.peek_style", "Peek style", "", C::Segmented, 0, "sidebar.peek_when_hidden"},
                      }},
                     {"Tab Switching",
                      {
                          {"tabs.switcher", "Tab bar", "", C::Segmented},
                          {"tabs.switcher_position", "Tab bar position", "", C::Segmented},
                          {"tabs.previews", "Previews", ""},
                          {"tabs.recent_limit", "Recent tabs", "", C::Stepper, 1},
                      }},
                     {"Colors",
                      {
                          {"colors.background", "Background", "", C::Color},
                          {"colors.foreground", "Foreground", "", C::Color},
                          {"colors.cursor", "Cursor", "", C::Color},
                          {"colors.cursor_text", "Text under the cursor", "", C::Color},
                          {"colors.selection_background", "Selection", "", C::Color},
                          {"colors.selection_foreground", "Selected text", "", C::Color},
                          {"colors.bold", "Bold text", "", C::Color},
                          {"colors.link", "Links", "", C::Color},
                          {"colors.accent", "Accent", "", C::Color},
                      },
                      "Empty colors come from the theme."},
                 }});

    p.push_back({"terminal", "Terminal", ui::Icon::terminal, Special::None,
                 {
                     {"Cursor",
                      {
                          {"terminal.cursor_style", "Cursor style", "", C::Segmented},
                          {"terminal.cursor_blink", "Blink", ""},
                      }},
                     {"Scrollback",
                      {
                          {"terminal.scrollback_lines", "Scrollback lines", "", C::Text},
                          {"terminal.scrollback_max_bytes", "Scrollback memory limit", "", C::Text},
                          {"terminal.mouse_scroll_multiplier", "Scroll speed", "", C::Slider, 0.1},
                      }},
                     {"Selection and Clipboard",
                      {
                          {"terminal.copy_on_select", "Copy on select", ""},
                          {"terminal.ctrl_c_copies_selection", "Ctrl+C copies the selection", "", C::Auto, 0, {}, kWindows | kLinux},
                          {"terminal.confirm_multiline_paste", "Confirm multi-line paste", ""},
                          {"terminal.word_separators", "Word separators", "Characters that end a word on double click.", C::Text},
                          {"terminal.clipboard_write", "Programs may write the clipboard", "", C::Segmented},
                          {"terminal.clipboard_read", "Programs may read the clipboard", "", C::Segmented},
                      }},
                     {"Keyboard and Mouse",
                      {
                          {"macos.option_as_alt", "Option key acts as Meta", "", C::Segmented, 0, {}, kMac},
                          {"terminal.link_detection", "Detect links", ""},
                          {"terminal.link_modifier", "Open links with", "", C::Segmented, 0, "terminal.link_detection"},
                          {"terminal.focus_follows_mouse", "Focus follows the mouse", ""},
                      }},
                     {"Bell",
                      {{"terminal.bell", "Bell", "", C::Segmented}},
                      {}},
                     {"Shell",
                      {
                          {"terminal.env", "Environment", "NAME=value, one per line.", C::WideText},
                          {"terminal.shell_integration", "Shell integration", "Prompts, command marks and the working directory."},
                          {"tabs.title", "Tab title", "", C::Segmented, 0, {}, kAll, "program=Program|osc=Set by the program|shell=Shell"},
                          {"session.linger_after_exit", "Keep exited panes open for", "", C::Text},
                      }},
                     {"Splits",
                      {
                          {"splits.auto_tile", "Tile new splits", ""},
                          {"splits.resize_step", "Resize step", "", C::Stepper, 1},
                      }},
                 }});

    p.push_back({"animations", "Animations", ui::Icon::spinner, Special::None,
                 {
                     {"Motion",
                      {
                          {"ui.motion", "Motion", "Automatic follows the system's reduce motion setting.", C::Segmented},
                          {"animations.smooth_scrolling", "Smooth scrolling", ""},
                          {"animations.snap_scroll_to_row", "Snap scrolling to rows", "", C::Auto, 0, "animations.smooth_scrolling"},
                          {"animations.smooth_cursor", "Smooth cursor", ""},
                          {"animations.cursor_morph", "Cursor morph", "", C::Auto, 0, "animations.smooth_cursor"},
                          {"animations.cursor_trail", "Cursor trail", "", C::Auto, 0, "animations.smooth_cursor"},
                          {"animations.animate_splits", "Animate splits", ""},
                      }},
                 }});

    p.push_back({"quick_terminal", "Quick Terminal", ui::Icon::split_down, Special::QuickTerminal,
                 {
                     {"Quick Terminal",
                      {
                          {"quick_terminal.enabled", "Enable the quick terminal", ""},
                          {"quick_terminal.screen", "Screen", "", C::Segmented, 0, "quick_terminal.enabled", kAll,
                           "mouse=With the pointer|active=Active window|primary=Primary"},
                          {"quick_terminal.position_mode", "Position", "", C::Segmented, 0, "quick_terminal.enabled"},
                          {"quick_terminal.x", "Horizontal position", "", C::Percent, 0.01, "quick_terminal.position_mode=fixed"},
                          {"quick_terminal.y", "Vertical position", "", C::Percent, 0.01, "quick_terminal.position_mode=fixed"},
                          {"quick_terminal.size_mode", "Size", "", C::Segmented, 0, "quick_terminal.enabled"},
                          {"quick_terminal.width", "Width", "", C::Percent, 0.01, "quick_terminal.size_mode=fixed"},
                          {"quick_terminal.height", "Height", "", C::Percent, 0.01, "quick_terminal.size_mode=fixed"},
                          {"quick_terminal.animation", "Animation", "", C::Segmented, 0, "quick_terminal.enabled"},
                          {"quick_terminal.hide_on_focus_loss", "Hide when it loses focus", "", C::Auto, 0, "quick_terminal.enabled"},
                      }},
                 }});

    p.push_back({"notifications", "Notifications", ui::Icon::bell, Special::None,
                 {
                     {"Notifications",
                      {
                          {"notifications.command_finished", "Notify when a command finishes", "", C::Segmented, 0, {}, kAll,
                           "unfocused=When unfocused|always=Always|never=Never"},
                          {"notifications.command_finished_after", "Only after it ran for", "", C::Text},
                          {"notifications.agent_finished", "Notify when an agent finishes", ""},
                          {"notifications.badge", "Badge the app icon", ""},
                          {"notifications.flash_taskbar", "Flash the taskbar button", "", C::Auto, 0, {}, kWindows | kLinux},
                          {"notifications.sound", "Play a sound", ""},
                      }},
                 }});

    p.push_back({"keymaps", "Keymaps", ui::Icon::keyboard, Special::Keymaps,
                 {
                     {"Programs",
                      {
                          {"terminal.copy_mode_keymap", "Copy mode keys", "", C::Segmented},
                          {"keys.passthrough_programs", "Pass keys to these programs", "Comma separated; keepterm leaves their keys alone.", C::Text},
                      }},
                 }});

    p.push_back({"agents", "Agents", ui::Icon::agent, Special::Agents,
                 {
                     {"Restore",
                      {
                          {"agents.restore", "Restore agent sessions", ""},
                          {"agents.capture_interval", "Check for sessions every", "", C::Text, 0, "agents.restore"},
                          {"agents.restore_timeout_scale", "Restore timeout scale", "Raise on slow machines.", C::Slider, 0.5, "agents.restore"},
                          {"agents.claude.config_dir", "Claude Code config folder", "Empty: ~/.claude or CLAUDE_CONFIG_DIR.", C::Text},
                      }},
                     {"Limits",
                      {
                          {"agents.refresh_limits", "Refresh limits from the agent",
                           "A tiny request once a minute (Claude Code: Haiku, about 600 tokens)."},
                      }},
                 }});

    p.push_back({"resources", "Resources", ui::Icon::memory, Special::None,
                 {
                     {"Monitoring",
                      {
                          {"resources.enabled", "Monitor resource usage", ""},
                          {"resources.interval", "Sampling interval", "", C::Text, 0, "resources.enabled"},
                          {"resources.panel_interval", "Panel refresh interval", "", C::Text, 0, "resources.enabled"},
                          {"resources.history", "History length", "", C::Text, 0, "resources.enabled"},
                          {"resources.tab_badge", "Tab badges", "", C::Segmented, 0, "resources.enabled", kAll, "off=Off|memory=Memory"},
                          {"resources.show_total", "Show the total", "", C::Auto, 0, "resources.enabled"},
                          {"resources.windows.allow_breakaway", "Track processes that leave the job", "", C::Auto, 0, {}, kWindows},
                          {"resources.linux.exclude_page_cache", "Exclude the page cache", "", C::Auto, 0, {}, kLinux},
                      }},
                     {"Memory Warnings",
                      {
                          {"resources.warn_session", "Warn when a session uses", "Empty: off. For example 2GiB.", C::Text},
                          {"resources.warn_project", "Warn when a project uses", "", C::Text},
                          {"resources.warn_total", "Warn when everything uses", "", C::Text},
                      }},
                 }});

    p.push_back({"history", "History", ui::Icon::history, Special::History,
                 {
                     {"Retention",
                      {
                          {"storage.keep_closed", "Delete closed sessions after",
                           "Pinned sessions are kept.", C::Preset, 0, {}, kAll,
                           "0s=Never|24h=1 day|72h=3 days|168h=1 week|336h=2 weeks|720h=1 month|1440h=2 months|2160h=3 months|"
                           "4320h=6 months|8760h=1 year"},
                          {"storage.cold_after", "Compress after", "Older output is packed tighter.", C::Preset, 0, {}, kAll,
                           "1h=1 hour|6h=6 hours|24h=1 day|72h=3 days|168h=1 week"},
                      }},
                 }});

    p.push_back({"storage", "Storage", ui::Icon::storage, Special::Storage,
                 {
                     {"Limits",
                      {
                          {"storage.max_total", "Total limit", "", C::Text},
                          {"storage.max_per_session", "Limit per session", "", C::Text},
                          {"storage.min_free_disk", "Keep free on the disk", "", C::Text},
                          {"storage.min_free_disk_percent", "Keep free on the disk (percent)", "", C::Stepper, 1},
                      }},
                     {"Retention",
                      {
                          {"storage.max_age", "Delete sessions older than", "", C::Text},
                          {"storage.raw_max_age", "Keep raw output for", "", C::Text},
                          {"storage.events_max_age", "Keep events for", "", C::Text},
                      }},
                     {"Compression",
                      {
                          {"storage.codec", "Codec", "", C::Segmented, 0, {}, kAll, "none=None|zstd=zstd|lzma=LZMA"},
                          {"storage.codec_level", "Level", "", C::Stepper, 1},
                          {"storage.block_size", "Block size", "", C::Text},
                          {"storage.cold_codec", "Cold codec", "", C::Segmented, 0, {}, kAll, "none=None|zstd=zstd|lzma=LZMA"},
                          {"storage.cold_codec_level", "Cold level", "", C::Stepper, 1},
                          {"storage.cold_block_size", "Cold block size", "", C::Text},
                          {"session.output_ring", "Live output buffer", "", C::Text},
                      }},
                 }});

    p.push_back({"extended", "Extended", ui::Icon::cpu, Special::None,
                 {
                     {"Windows",
                      {
                          {"extended.openconsole", "Use OpenConsole", "A newer console host shipped with keepterm."},
                          {"extended.process_cwd", "Read the working directory of processes", ""},
                          {"extended.nt_process_info", "Detailed process information", ""},
                          {"console_agent.cls_clears_scrollback", "cls clears the scrollback", ""},
                          {"console_agent.use_console_palette", "Use the console palette", ""},
                      }},
                 },
                 kWindows});

    p.push_back({"advanced", "Advanced", ui::Icon::info, Special::Advanced,
                 {
                     {"Rendering",
                      {
                          {"renderer", "Renderer", "", C::Dropdown, 0, {}, kAll,
                           "auto=Automatic|d3d11=Direct3D 11|metal=Metal|vulkan=Vulkan|opengl=OpenGL|software=Software"},
                          {"renderer_remote", "Renderer over remote desktop", "", C::Segmented, 0, {}, kWindows,
                           "software=Software|auto=Automatic"},
                          {"render.vsync", "Vertical sync", ""},
                          {"render.max_fps", "Frame rate limit", "0: the display's rate.", C::Stepper, 10},
                          {"render.subpixel_text", "Subpixel text", "", C::Segmented, 0, {}, kAll, "auto=Automatic|on=On|off=Off"},
                          {"font.hinting", "Hinting", "", C::Segmented},
                          {"font.thicken", "Thicken text", "", C::Slider, 0.1},
                          {"terminal.pty", "Pseudo console", "", C::Segmented, 0, {}, kWindows},
                      }},
                     {"Interface Fonts",
                      {
                          {"ui.font", "Interface font", "Empty: the system font.", C::Text},
                          {"ui.mono_font", "Interface monospace font", "", C::Text},
                      }},
                     {"Command Palette",
                      {
                          {"palette.recent_projects", "Recent projects", "", C::Stepper, 1},
                          {"palette.frecency", "Rank by frequency and recency", ""},
                          {"palette.preview", "Preview", ""},
                          {"palette.sources", "Sources", "Comma separated: commands, projects, tabs, folders, sessions.", C::Text},
                      }},
                     {"Logging",
                      {{"general.log_level", "Log level", "", C::Segmented}}},
                 }});
    return p;
}

}  // namespace

const std::vector<PaneDef>& panes() {
    static const std::vector<PaneDef> p = build();
    return p;
}

const PaneDef* find_pane(std::string_view id) {
    for (const auto& p : panes()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::uint8_t os_bit(commands::Os os) {
    switch (os) {
        case commands::Os::MacOS: return kMac;
        case commands::Os::Windows: return kWindows;
        default: return kLinux;
    }
}

bool row_visible(const RowDef& r, commands::Os os) { return (r.os & os_bit(os)) != 0; }

std::vector<std::string_view> row_keys() {
    std::vector<std::string_view> out;
    for (const auto& p : panes())
        for (const auto& g : p.groups)
            for (const auto& r : g.rows) out.push_back(r.key);
    // Keys the special panes edit with their own controls.
    out.push_back("theme.name");
    out.push_back("theme.light");
    out.push_back("theme.dark");
    out.push_back("agents.restore_skip");
    return out;
}

std::vector<config::KeyId> keys_without_row() {
    std::set<std::string_view> shown;
    for (auto k : row_keys()) shown.insert(k);
    std::vector<config::KeyId> out;
    for (const auto& s : config::schema()) {
        if (!shown.count(s.path)) out.push_back(s.id);
    }
    return out;
}

std::string option_label(const RowDef& r, std::string_view value) {
    std::string_view o = r.options;
    while (!o.empty()) {
        const size_t bar = o.find('|');
        std::string_view item = o.substr(0, bar);
        const size_t eq = item.find('=');
        if (eq != std::string_view::npos && item.substr(0, eq) == value) return std::string(item.substr(eq + 1));
        if (bar == std::string_view::npos) break;
        o.remove_prefix(bar + 1);
    }
    if (value.empty()) return "Default";
    std::string s(value);
    for (char& c : s) {
        if (c == '-' || c == '_') c = ' ';
    }
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

}  // namespace kt::app::settings
