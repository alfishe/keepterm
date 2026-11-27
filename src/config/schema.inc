// The single registry of config keys. X-macro table, one row per key:
//
//   KT_KEY(id, path, type, default, range, apply, section, project_overridable, doc)
//
//   type     Bool, Int, Float, String, Enum, Color, Duration, Size, SizeOrOff, List, EnumList,
//            ColorList, StringMap, TableArray
//   default  a literal, PerOs(windows, macos, linux), List(...), NoTable or NoTables.
//            Durations ("10s") and sizes ("8MiB") are written as in the file.
//   range    NoRange, Range(lo, hi) (numbers; durations and sizes as strings), or
//            Values("a|b|c") for enums and enum lists
//   apply    Live, NewPanes, Restart
//   section  Settings section id, or "" for keys that only the file sets
//
// Ids are the path with '.' replaced by '_'. A default is never repeated anywhere else.

// ---------------------------------------------------------------- top level
KT_KEY(version, "version", Int, 1, Range(1, 1000), Live, "", false,
       "Schema version of the file.")
KT_KEY(include, "include", List, List(), NoRange, Live, "", false,
       "Files merged before this one, relative to it (max depth 4); this file wins.")
KT_KEY(renderer, "renderer", Enum, "auto", Values("auto|d3d11|metal|vulkan|opengl|software"), Live,
       "advanced", false, "Render backend; auto probes the best one.")
KT_KEY(renderer_remote, "renderer_remote", Enum, "software", Values("software|auto"), Live, "advanced", false, "Render backend in a remote session: software at once, or the normal probe.")

// ---------------------------------------------------------------- [general]
KT_KEY(general_language, "general.language", String, "system", NoRange, Restart, "general", false,
       "UI language: \"system\" or a BCP 47 tag.")
KT_KEY(general_read_macterm_projects, "general.read_macterm_projects", Bool, true, NoRange, Live, "",
       false, "macOS: also read Macterm's project files (~/.config/macterm/projects), never writing them.")
KT_KEY(general_confirm_quit, "general.confirm_quit", Enum, "busy", Values("always|busy|never"), Live, "general", false, "Ask before quitting: always, only when a pane runs a program, or never.")
KT_KEY(general_startup, "general.startup", Enum, "restore", Values("restore|clean|ask"), Live, "general",
       false, "At launch: restore the last workspace, start clean (its sessions stay detached), or ask.")
KT_KEY(general_open_folder_as, "general.open_folder_as", Enum, "project", Values("project|tab"), Live, "general", false, "What \"Open in keepterm\" from the file manager creates.")
KT_KEY(general_log_level, "general.log_level", Enum, "info", Values("error|warn|info|debug"), Live, "advanced", false, "Lowest level written to the log.")

// ---------------------------------------------------------------- [font]
KT_KEY(font_family, "font.family", String, "", NoRange, Live, "appearance", true,
       "Terminal font family; empty uses the platform default monospace font.")
KT_KEY(font_fallback, "font.fallback", List, List(), NoRange, Live, "", true,
       "Fallback families tried before the platform chain.")
KT_KEY(font_size, "font.size", Float, PerOs(12.0, 13.0, 12.0), Range(4, 96), Live, "appearance", true,
       "Terminal font size in points.")
KT_KEY(font_line_height, "font.line_height", Float, 1.0, Range(0.8, 2.0), Live, "appearance", true,
       "Line height multiplier.")
KT_KEY(font_cell_width, "font.cell_width", Float, 1.0, Range(0.8, 1.5), Live, "appearance", true,
       "Cell width multiplier.")
KT_KEY(font_weight, "font.weight", Enum, "regular", Values("thin|light|regular|medium|semibold|bold"), Live,
       "appearance", true, "Weight of regular text.")
KT_KEY(font_bold_weight, "font.bold_weight", Enum, "bold", Values("thin|light|regular|medium|semibold|bold"), Live,
       "", true, "Weight of bold text.")
KT_KEY(font_ligatures, "font.ligatures", Bool, false, NoRange, Live, "appearance", true,
       "Enable programming ligatures.")
KT_KEY(font_antialias, "font.antialias", Enum, "auto", Values("auto|grayscale|subpixel|none"), Live, "",
       false, "Glyph antialiasing; subpixel only on opaque windows with an RGB panel.")
KT_KEY(font_features, "font.features", List, List(), NoRange, Live, "appearance", true,
       "OpenType features such as \"ss01\", \"zero\" or \"-calt\".")
KT_KEY(font_hinting, "font.hinting", Enum, "light", Values("light|none"), Live, "advanced", false,
       "Glyph hinting.")
KT_KEY(font_thicken, "font.thicken", Float, PerOs(0.0, 0.5, 0.0), Range(0, 2), Live, "advanced", false,
       "Stem darkening amount (macOS: only at scale 2 and above).")
KT_KEY(font_cjk_lang, "font.cjk_lang", Enum, "", Values("|ja|ko|zh-Hans|zh-Hant"), Live, "", true,
       "Han fallback face language; empty follows the OS UI language.")
KT_KEY(font_ambiguous_width, "font.ambiguous_width", Enum, "auto", Values("auto|narrow|wide"), NewPanes,
       "appearance", true, "Width of East Asian ambiguous characters.")
KT_KEY(font_builtin_box_drawing, "font.builtin_box_drawing", Bool, true, NoRange, Live, "appearance",
       true, "Draw box drawing, block and Powerline glyphs to fill the cell.")
KT_KEY(font_cursor_ligature_break, "font.cursor_ligature_break", Bool, true, NoRange, Live, "", false,
       "Split a ligature at the cursor.")

// ---------------------------------------------------------------- [theme]
KT_KEY(theme_mode, "theme.mode", Enum, "system", Values("system|light|dark"), Live, "appearance", true,
       "Which theme side applies: follow the OS, or pin light or dark.")
KT_KEY(theme_light, "theme.light", String, "Keepterm Light", NoRange, Live, "appearance", true,
       "Theme used in light appearance.")
KT_KEY(theme_dark, "theme.dark", String, "Keepterm Dark", NoRange, Live, "appearance", true,
       "Theme used in dark appearance.")
KT_KEY(theme_minimum_contrast, "theme.minimum_contrast", Float, 1.0, Range(1.0, 21.0), Live, "appearance", true, "Minimum foreground contrast ratio; 1 is off.")
KT_KEY(theme_name, "theme.name", String, "", NoRange, Live, "appearance", true,
       "One theme for both sides, or \"light:NAME,dark:NAME\" (Ghostty syntax); set, it replaces theme.light and theme.dark.")
KT_KEY(theme_bold_is_bright, "theme.bold_is_bright", Bool, false, NoRange, Live, "appearance", true,
       "Bold text in ANSI colors 0-7 uses the bright colors 8-15.")

// ---------------------------------------------------------------- [colors] (empty = from theme)
KT_KEY(colors_background, "colors.background", Color, "", NoRange, Live, "", true,
       "Background color override.")
KT_KEY(colors_foreground, "colors.foreground", Color, "", NoRange, Live, "", true,
       "Foreground color override.")
KT_KEY(colors_cursor, "colors.cursor", Color, "", NoRange, Live, "", true, "Cursor color override.")
KT_KEY(colors_cursor_text, "colors.cursor_text", Color, "", NoRange, Live, "", true,
       "Color of text under the cursor.")
KT_KEY(colors_selection_background, "colors.selection_background", Color, "", NoRange, Live, "", true,
       "Selection background override.")
KT_KEY(colors_selection_foreground, "colors.selection_foreground", Color, "", NoRange, Live, "", true,
       "Selection foreground override.")
KT_KEY(colors_palette, "colors.palette", ColorList, List(), Range(0, 16), Live, "", true,
       "ANSI colors 0-15; empty uses the theme's palette.")
KT_KEY(colors_accent, "colors.accent", Color, "", NoRange, Live, "", true,
       "Accent color; empty uses the theme, else the OS accent.")
KT_KEY(colors_bold, "colors.bold", Color, "", NoRange, Live, "", true,
       "Color of bold text in the default foreground; empty uses the theme, else the foreground.")
KT_KEY(colors_link, "colors.link", Color, "", NoRange, Live, "", true,
       "Color of hyperlinks (OSC 8) under the pointer; empty uses the theme, else the accent.")

// ---------------------------------------------------------------- [window]
KT_KEY(window_opacity, "window.opacity", Float, 1.0, Range(0.3, 1.0), Live, "appearance", true,
       "Window opacity; not applied in a remote session.")
KT_KEY(window_blur, "window.blur", Bool, false, NoRange, Live, "appearance", false,
       "Blur behind a translucent window.")
KT_KEY(window_padding_x, "window.padding_x", Float, 6.0, Range(0, 64), Live, "appearance", true,
       "Horizontal padding around the terminal grid in DIPs.")
KT_KEY(window_padding_y, "window.padding_y", Float, 4.0, Range(0, 64), Live, "appearance", true,
       "Vertical padding around the terminal grid in DIPs.")
KT_KEY(window_titlebar, "window.titlebar", Enum, PerOs("custom", "custom", "native"), Values("custom|native"),
       Restart, "appearance", false, "Title bar drawn by keepterm or by the OS.")
KT_KEY(window_show_title_bar, "window.show_title_bar", Bool, true, NoRange, Live, "appearance", false,
       "Show the title bar.")
KT_KEY(window_unfocused_split_opacity, "window.unfocused_split_opacity", Float, 0.7, Range(0.4, 1.0), Live, "appearance", true, "Opacity of split panes without focus.")
KT_KEY(window_confirm_close, "window.confirm_close", Enum, "busy", Values("always|busy|never"), Live, "general", false, "Ask before closing a window or tab.")

// ---------------------------------------------------------------- [terminal]
KT_KEY(terminal_shell, "terminal.shell", String, "", NoRange, NewPanes, "terminal", true,
       "Shell program; empty uses the platform default.")
KT_KEY(terminal_shell_args, "terminal.shell_args", List, List(), NoRange, NewPanes, "terminal", true,
       "Arguments passed to the shell.")
KT_KEY(terminal_env, "terminal.env", StringMap, NoTable, NoRange, NewPanes, "", true,
       "Environment variables added to new sessions (values redacted in logs).")
KT_KEY(terminal_new_tab_directory, "terminal.new_tab_directory", Enum, "project", Values("project|inherit|home"),
       Live, "terminal", true, "Working folder of a new tab.")
KT_KEY(terminal_new_split_directory, "terminal.new_split_directory", Enum, "inherit",
       Values("project|inherit|home"), Live, "terminal", true, "Working folder of a new split pane.")
KT_KEY(terminal_scrollback_lines, "terminal.scrollback_lines", Int, 10000, Range(0, 1000000), NewPanes,
       "terminal", true, "Lines of scrollback kept per session.")
KT_KEY(terminal_scrollback_max_bytes, "terminal.scrollback_max_bytes", Size, "8MiB", Range("1MB", "1GB"),
       NewPanes, "terminal", true, "Memory limit of the scrollback per session.")
KT_KEY(terminal_cursor_style, "terminal.cursor_style", Enum, "block", Values("block|bar|underline"), Live,
       "terminal", true, "Cursor shape.")
KT_KEY(terminal_cursor_blink, "terminal.cursor_blink", Bool, true, NoRange, Live, "terminal", true,
       "Blink the cursor.")
KT_KEY(terminal_copy_on_select, "terminal.copy_on_select", Bool, false, NoRange, Live, "terminal",
       false, "Copy the selection to the clipboard when it is made.")
KT_KEY(terminal_ctrl_c_copies_selection, "terminal.ctrl_c_copies_selection", Bool, false, NoRange, Live,
       "terminal", false, "Windows and Linux: Ctrl+C copies when there is a selection.")
KT_KEY(terminal_confirm_multiline_paste, "terminal.confirm_multiline_paste", Bool, true, NoRange, Live,
       "terminal", false, "Ask before pasting several lines when bracketed paste is off.")
KT_KEY(terminal_word_separators, "terminal.word_separators", String, " \t\"'`()[]{}<>│,;:", NoRange, Live,
       "", false, "Characters that end a word for double-click selection.")
KT_KEY(terminal_bell, "terminal.bell", Enum, "attention", Values("attention|audible|visual|none"), Live,
       "terminal", true, "What the bell does.")
KT_KEY(terminal_shell_integration, "terminal.shell_integration", Bool, true, NoRange, NewPanes, "terminal", true, "Inject the shell integration scripts.")
KT_KEY(terminal_link_detection, "terminal.link_detection", Bool, true, NoRange, Live, "terminal",
       false, "Detect URLs and paths in the output.")
KT_KEY(terminal_link_modifier, "terminal.link_modifier", Enum, PerOs("ctrl", "cmd", "ctrl"),
       Values("ctrl|cmd|alt|none"), Live, "terminal", false, "Modifier that makes a link clickable.")
KT_KEY(terminal_clipboard_write, "terminal.clipboard_write", Enum, "allow", Values("allow|ask|deny"), Live,
       "terminal", true, "Programs may set the clipboard (OSC 52).")
KT_KEY(terminal_clipboard_read, "terminal.clipboard_read", Enum, "ask", Values("allow|ask|deny"), Live, "terminal", true, "Programs may read the clipboard (OSC 52 query).")
KT_KEY(terminal_mouse_scroll_multiplier, "terminal.mouse_scroll_multiplier", Float, 3.0, Range(0.1, 20), Live,
       "terminal", false, "Lines per mouse wheel notch.")
KT_KEY(terminal_focus_follows_mouse, "terminal.focus_follows_mouse", Bool, false, NoRange, Live, "terminal",
       false, "Focus the pane under the mouse pointer.")
KT_KEY(terminal_copy_mode_keymap, "terminal.copy_mode_keymap", Enum, "vim", Values("vim|emacs"), Live, "terminal", false, "Key set of copy mode.")
KT_KEY(terminal_pty, "terminal.pty", Enum, "auto", Values("auto|conpty|agent|openconsole"), NewPanes, "advanced", false, "Windows PTY backend; openconsole needs extended.openconsole = true.")

// ---------------------------------------------------------------- [macos]
KT_KEY(macos_option_as_alt, "macos.option_as_alt", Enum, "none", Values("none|left|right|both"), Live, "terminal", false, "Option keys that act as Alt instead of typing special characters.")

// ---------------------------------------------------------------- [render]
KT_KEY(render_vsync, "render.vsync", Bool, true, NoRange, Live, "advanced", false,
       "Wait for vertical sync on GPU backends.")
KT_KEY(render_subpixel_text, "render.subpixel_text", Enum, "auto", Values("auto|on|off"), Live, "advanced", false, "Subpixel text rendering.")
KT_KEY(render_max_fps, "render.max_fps", Int, 0, Range(0, 360), Live, "advanced", false,
       "Frame rate cap; 0 follows the monitor (60 in remote sessions).")

// ---------------------------------------------------------------- [console_agent] (Windows)
KT_KEY(console_agent_cls_clears_scrollback, "console_agent.cls_clears_scrollback", Bool, false, NoRange, NewPanes,
       "advanced", true, "cls also clears the scrollback on the console agent path.")
KT_KEY(console_agent_use_console_palette, "console_agent.use_console_palette", Bool, false, NoRange, NewPanes,
       "advanced", true, "Forward palette changes made through the console API.")

// ---------------------------------------------------------------- [extended] (Windows)
KT_KEY(extended_openconsole, "extended.openconsole", Bool, false, NoRange, NewPanes, "extended", false,
       "headless OpenConsole as the PTY backend where ConPTY is missing (undocumented interfaces).")
KT_KEY(extended_process_cwd, "extended.process_cwd", Bool, false, NoRange, Live, "extended", false,
       "read the shell's working folder from its process environment block.")
KT_KEY(extended_nt_process_info, "extended.nt_process_info", Bool, false, NoRange, Live, "extended",
       false, "one-call process snapshot and command lines without WMI.")

// ---------------------------------------------------------------- [ui]
KT_KEY(ui_font, "ui.font", String, "", NoRange, Live, "", false,
       "UI font family; empty uses the platform UI font.")
KT_KEY(ui_mono_font, "ui.mono_font", String, "", NoRange, Live, "", false,
       "UI monospace font family; empty uses the platform monospace font.")
KT_KEY(ui_font_scale, "ui.font_scale", Float, 1.0, Range(0.8, 1.6), Live, "appearance", false,
       "Scale of UI text.")
KT_KEY(ui_motion, "ui.motion", Enum, "auto", Values("auto|full|reduced|none"), Live, "animations", false,
       "Animation level; auto is reduced over RDP and when the OS asks.")

// ---------------------------------------------------------------- [sidebar]
KT_KEY(sidebar_peek_when_hidden, "sidebar.peek_when_hidden", Bool, false, NoRange, Live, "appearance", false,
       "Show the hidden sidebar when the pointer reaches the edge.")
KT_KEY(sidebar_peek_style, "sidebar.peek_style", Enum, "resize", Values("overlay|resize"), Live, "appearance",
       false, "How the peeking sidebar appears.")
KT_KEY(sidebar_project_icon, "sidebar.project_icon", String, "folder", NoRange, Live, "appearance", true,
       "Icon id of projects.")
KT_KEY(sidebar_tab_icon, "sidebar.tab_icon", Enum, "terminal",
       Values("terminal|number|number-circle|number-square|none"), Live, "appearance", false,
       "Icon of tabs in the sidebar.")
KT_KEY(sidebar_icon_size, "sidebar.icon_size", Enum, "medium", Values("small|medium|large"), Live, "appearance", false, "Sidebar icon size.")
KT_KEY(sidebar_show_agent_icons, "sidebar.show_agent_icons", Bool, true, NoRange, Live, "appearance",
       false, "Show agent icons on tabs.")
KT_KEY(sidebar_show_status, "sidebar.show_status", Bool, true, NoRange, Live, "appearance", false,
       "Show tab status (running, attention).")
KT_KEY(sidebar_spinner_over_agent_icons, "sidebar.spinner_over_agent_icons", Bool, true, NoRange, Live, "appearance", false, "Draw the busy spinner over agent icons.")
KT_KEY(sidebar_show_new_project_button, "sidebar.show_new_project_button", Bool, true, NoRange, Live, "appearance", false, "Show the New Project button.")
KT_KEY(sidebar_show_project_new_tab_button, "sidebar.show_project_new_tab_button", Bool, true, NoRange, Live, "appearance", false, "Show the New Tab button of each project.")
KT_KEY(sidebar_auto_assign_project_colors, "sidebar.auto_assign_project_colors", Bool, false, NoRange, Live,
       "projects", false, "Give new projects a color tag automatically.")

// ---------------------------------------------------------------- [tabs]
KT_KEY(tabs_switcher, "tabs.switcher", Enum, "when-multiple", Values("when-multiple|always|hidden"), Live, "appearance", false, "When the tab switcher shows.")
KT_KEY(tabs_switcher_position, "tabs.switcher_position", Enum, "trailing", Values("trailing|center"), Live, "appearance", false, "Position of the tab switcher.")
KT_KEY(tabs_previews, "tabs.previews", Bool, true, NoRange, Live, "appearance", false,
       "Show tab preview cards while Ctrl+Tab is held.")
KT_KEY(tabs_recent_limit, "tabs.recent_limit", Int, 0, Range(0, 50), Live, "appearance", false,
       "Tabs in the recent list; 0 is unlimited.")
KT_KEY(tabs_title, "tabs.title", Enum, "program", Values("program|osc|shell"), Live, "terminal", true,
       "Source of the tab title.")
KT_KEY(tabs_close_last_pane_closes_tab, "tabs.close_last_pane_closes_tab", Bool, true, NoRange, Live, "",
       false, "Closing the last pane closes its tab.")

// ---------------------------------------------------------------- [splits]
KT_KEY(splits_auto_tile, "splits.auto_tile", Bool, false, NoRange, Live, "terminal", true,
       "Equal pane sizes after split and close.")
KT_KEY(splits_resize_step, "splits.resize_step", Int, 2, Range(1, 20), Live, "", false,
       "Cells per keyboard resize step.")

// ---------------------------------------------------------------- [animations] (gated by ui.motion)
KT_KEY(animations_smooth_scrolling, "animations.smooth_scrolling", Bool, true, NoRange, Live, "animations",
       false, "Smooth scrolling.")
KT_KEY(animations_snap_scroll_to_row, "animations.snap_scroll_to_row", Bool, true, NoRange, Live, "animations", false, "Snap smooth scrolling to whole rows when it stops.")
KT_KEY(animations_smooth_cursor, "animations.smooth_cursor", Bool, false, NoRange, Live, "animations", false,
       "Animate cursor movement.")
KT_KEY(animations_cursor_morph, "animations.cursor_morph", Bool, false, NoRange, Live, "animations", false,
       "Morph the cursor between shapes.")
KT_KEY(animations_cursor_trail, "animations.cursor_trail", Bool, false, NoRange, Live, "animations", false,
       "Draw a trail behind the moving cursor.")
KT_KEY(animations_animate_splits, "animations.animate_splits", Bool, true, NoRange, Live, "animations", false,
       "Animate split pane changes.")

// ---------------------------------------------------------------- [quick_terminal]
KT_KEY(quick_terminal_enabled, "quick_terminal.enabled", Bool, true, NoRange, Live, "quick_terminal",
       false, "Enable the quick terminal.")
KT_KEY(quick_terminal_position_mode, "quick_terminal.position_mode", Enum, "fixed", Values("fixed|dynamic"), Live,
       "quick_terminal", false, "Fixed position, or remember where it was dragged.")
KT_KEY(quick_terminal_x, "quick_terminal.x", Float, 0.5, Range(0, 1), Live, "quick_terminal", false,
       "Horizontal position: 0 left, 1 right.")
KT_KEY(quick_terminal_y, "quick_terminal.y", Float, 0.0, Range(0, 1), Live, "quick_terminal", false,
       "Vertical position: 0 top (drop-down), 1 bottom.")
KT_KEY(quick_terminal_size_mode, "quick_terminal.size_mode", Enum, "fixed", Values("fixed|dynamic"), Live,
       "quick_terminal", false, "Fixed size, or remember the last size.")
KT_KEY(quick_terminal_width, "quick_terminal.width", Float, 0.6, Range(0.2, 1.0), Live, "quick_terminal", false, "Width as a fraction of the screen.")
KT_KEY(quick_terminal_height, "quick_terminal.height", Float, 0.5, Range(0.2, 1.0), Live, "quick_terminal", false, "Height as a fraction of the screen.")
KT_KEY(quick_terminal_screen, "quick_terminal.screen", Enum, "mouse", Values("mouse|active|primary"), Live,
       "quick_terminal", false, "Screen the quick terminal opens on.")
KT_KEY(quick_terminal_hide_on_focus_loss, "quick_terminal.hide_on_focus_loss", Bool, true, NoRange, Live,
       "quick_terminal", false, "Hide when another window gets focus.")
KT_KEY(quick_terminal_animation, "quick_terminal.animation", Enum, "auto", Values("auto|slide|fade|none"), Live,
       "quick_terminal", false, "Show and hide animation.")

// ---------------------------------------------------------------- [notifications]
KT_KEY(notifications_command_finished, "notifications.command_finished", Enum, "unfocused",
       Values("unfocused|always|never"), Live, "notifications", true,
       "Notify when a long command finishes.")
KT_KEY(notifications_command_finished_after, "notifications.command_finished_after", Duration, "10s", NoRange, Live,
       "notifications", true, "Shorter commands never notify.")
KT_KEY(notifications_agent_finished, "notifications.agent_finished", Bool, true, NoRange, Live, "notifications", true, "Notify when an agent turn ends.")
KT_KEY(notifications_badge, "notifications.badge", Bool, true, NoRange, Live, "notifications", false,
       "Taskbar or Dock badge with the count of tabs needing attention.")
KT_KEY(notifications_flash_taskbar, "notifications.flash_taskbar", Bool, true, NoRange, Live, "notifications", false, "Flash the taskbar button or bounce the Dock icon.")
KT_KEY(notifications_sound, "notifications.sound", Bool, false, NoRange, Live, "notifications", false,
       "Play a sound with notifications.")

// ---------------------------------------------------------------- [sessions] (GUI side)
KT_KEY(sessions_show_outdated_badge, "sessions.show_outdated_badge", Bool, true, NoRange, Live, "general", false, "Mark sessions whose host is from an older keepterm.")
KT_KEY(sessions_restart_dead_sessions, "sessions.restart_dead_sessions", Bool, true, NoRange, Live, "general", true, "Start a new shell in the last folder when a session is gone.")
KT_KEY(sessions_report_orphans, "sessions.report_orphans", Bool, true, NoRange, Live, "general", false,
       "Report session hosts that no window owns.")

// ---------------------------------------------------------------- [session] (host side)
KT_KEY(session_history, "session.history", Bool, true, NoRange, NewPanes, "general", true,
       "Save terminal history to disk for restore; private panes never are.")
KT_KEY(session_history_size, "session.history_size", Size, "8MiB", Range("1MiB", "256MiB"), NewPanes, "general", true, "Journal ring size per session.")
KT_KEY(session_history_lines, "session.history_lines", Int, 10000, Range(0, 1000000), NewPanes, "", true,
       "Scrollback lines in each checkpoint.")
KT_KEY(session_linger_after_exit, "session.linger_after_exit", Duration, "10m", Range("0s", "24h"), NewPanes,
       "", false, "How long a host whose program ended waits for a window to acknowledge it.")
KT_KEY(session_output_ring, "session.output_ring", Size, "4MiB", Range("256KiB", "64MiB"), NewPanes, "",
       false, "Raw output kept in memory for fast resume.")

// ---------------------------------------------------------------- [agents]
KT_KEY(agents_restore, "agents.restore", Bool, true, NoRange, Live, "general", true,
       "Record agent sessions and start them again; off also deletes the records.")
KT_KEY(agents_capture_interval, "agents.capture_interval", Duration, "60s", Range("15s", "10m"), Live, "",
       false, "How often agent sessions are recorded.")
KT_KEY(agents_custom, "agents.custom", TableArray, NoTables, NoRange, Live, "", false,
       "Config-defined agent CLIs ([[agents.custom]]: name, command, resume arguments).")
KT_KEY(agents_claude_config_dir, "agents.claude.config_dir", String, "", NoRange, Live, "", false,
       "Claude config folder; empty uses CLAUDE_CONFIG_DIR, else ~/.claude.")
KT_KEY(agents_restore_timeout_scale, "agents.restore_timeout_scale", Float, 1.0, Range(0.5, 10), Live, "",
       false, "Multiplies the restore ready timeouts, for slow machines.")
KT_KEY(agents_restore_skip, "agents.restore_skip", List, List(), NoRange, Live, "agents", true,
       "Agent ids (\"claude\", \"qodercli\") whose sessions are not recorded or restored while agents.restore is on.")
KT_KEY(agents_refresh_limits, "agents.refresh_limits", Bool, true, NoRange, Live, "agents", false,
       "Once a minute while an agent runs, and when its popover opens, ask the agent for the account's current limits with "
       "a tiny request of its own (Claude Code: a Haiku call of about 600 tokens).")
KT_KEY(agents_context_enabled, "agents.context.enabled", Bool, true, NoRange, Live, "", false,
       "Show each agent's context fill and token counters (read from the agent's own files).")
KT_KEY(agents_context_warn_percent, "agents.context.warn_percent", Int, 70, Range(1, 100), Live, "", false,
       "Context fill shown as a warning from this percent.")
KT_KEY(agents_context_alert_percent, "agents.context.alert_percent", Int, 90, Range(1, 100), Live, "", false,
       "Context fill shown as an alert from this percent.")
KT_KEY(agents_context_show_in_private_panes, "agents.context.show_in_private_panes", Bool, false, NoRange, Live,
       "", false, "Also show the context of agents in private panes.")
KT_KEY(agents_context_windows, "agents.context.windows", StringMap, NoTable, NoRange, Live, "", false,
       "Context window per model id glob, for example { \"claude-opus-4-6*\" = \"1M\" }; overrides the built-in table.")
KT_KEY(agents_codex_home, "agents.codex.home", String, "", NoRange, Live, "", false,
       "Codex folder; empty uses CODEX_HOME, else ~/.codex.")
KT_KEY(agents_claude_statusline_default, "agents.claude.statusline_default", Enum, "compact", Values("compact|none"),
       Live, "", false,
       "What keepterm's Claude Code status line prints when no previous status line is chained.")

// ---------------------------------------------------------------- [resources]
KT_KEY(resources_enabled, "resources.enabled", Bool, true, NoRange, Live, "resources", false,
       "Measure memory and CPU per session.")
KT_KEY(resources_interval, "resources.interval", Duration, "2s", Range("1s", "60s"), Live, "resources",
       false, "Sampling interval.")
KT_KEY(resources_panel_interval, "resources.panel_interval", Duration, "1s", Range("500ms", "10s"), Live, "", false, "Sampling interval while the resource panel is open.")
KT_KEY(resources_tab_badge, "resources.tab_badge", Enum, "off", Values("off|memory"), Live, "resources",
       false, "Memory figure on each tab and split pane header.")
KT_KEY(resources_show_total, "resources.show_total", Bool, true, NoRange, Live, "resources", false,
       "Show the total in the title bar.")
KT_KEY(resources_history, "resources.history", Duration, "10m", Range("1m", "60m"), Live, "resources",
       false, "Length of the kept resource history.")
KT_KEY(resources_warn_session, "resources.warn_session", SizeOrOff, "", NoRange, Live, "resources", true,
       "Warn when one session uses more memory; empty is off.")
KT_KEY(resources_warn_project, "resources.warn_project", SizeOrOff, "", NoRange, Live, "resources", true,
       "Warn when one project's sessions use more memory; empty is off.")
KT_KEY(resources_warn_total, "resources.warn_total", SizeOrOff, "", NoRange, Live, "resources", false,
       "Warn when keepterm and all sessions use more memory; empty is off.")
KT_KEY(resources_windows_allow_breakaway, "resources.windows.allow_breakaway", Bool, true, NoRange, NewPanes,
       "resources", false, "Windows: the session job allows CREATE_BREAKAWAY_FROM_JOB.")
KT_KEY(resources_linux_exclude_page_cache, "resources.linux.exclude_page_cache", Bool, false, NoRange, Live,
       "resources", false, "Linux: subtract reclaimable file cache from session memory.")

// ---------------------------------------------------------------- [palette]
KT_KEY(palette_recent_projects, "palette.recent_projects", Int, 5, Range(0, 20), Live, "", false,
       "Recent projects in the empty palette.")
KT_KEY(palette_frecency, "palette.frecency", Bool, true, NoRange, Live, "", false,
       "Rank palette results by usage.")
KT_KEY(palette_preview, "palette.preview", Bool, true, NoRange, Live, "", false,
       "Preview the selected palette item.")
KT_KEY(palette_sources, "palette.sources", EnumList, List("commands", "projects", "tabs", "folders", "sessions"),
       Values("commands|projects|tabs|folders|sessions"), Live, "", false, "Sources searched by the palette.")

// ---------------------------------------------------------------- [keys] (the command module parses the bindings)
KT_KEY(keys_passthrough_programs, "keys.passthrough_programs", List, List(), NoRange, Live, "keymaps",
       false, "Programs that receive chords of bindings with passthrough = true.")

// ---------------------------------------------------------------- [storage]
KT_KEY(storage_max_total, "storage.max_total", Size, "0B", Range("0B", "64TiB"), Live, "", false,
       "Disk cap for the whole history store; 0 = none (nothing is deleted for it).")
KT_KEY(storage_max_per_session, "storage.max_per_session", Size, "0B", Range("0B", "64TiB"), Live, "",
       true, "Disk cap per session; 0 = none.")
KT_KEY(storage_max_age, "storage.max_age", Duration, "0s", Range("0s", "87600h"), Live, "", true,
       "Age limit for stored output and text (e.g. 2160h = 90 days); 0 = none.")
KT_KEY(storage_keep_closed, "storage.keep_closed", Duration, "0s", Range("0s", "87600h"), Live, "", true,
       "How long the history of closed sessions is kept (e.g. 720h = 30 days); 0 = for good.")
KT_KEY(storage_raw_max_age, "storage.raw_max_age", Duration, "0s", Range("0s", "87600h"), Live, "", true,
       "Age limit for the raw output stream (e.g. 168h = 7 days); after it only the text and commands stay. Pinned sessions keep raw. 0 = none.")
KT_KEY(storage_events_max_age, "storage.events_max_age", Duration, "0s", Range("0s", "87600h"), Live, "",
       true, "Age limit for phase 2/3 events; 0 = none.")
KT_KEY(storage_resources_downsample, "storage.resources_downsample", List, List("24h", "720h", "2160h"), NoRange, Live,
       "", false, "Resource samples: full rate up to the first age, per minute up to the second, dropped after the third.")
// Compression tiers. The defaults are provisional: they are to
// come from the live-data measurement on real session data.
// Switching is a config change; old blocks keep their codec until the janitor repacks them.
KT_KEY(storage_codec, "storage.codec", Enum, "zstd", Values("none|zstd|lzma"), Live, "", false,
       "Codec of fresh blocks, written by the session host. lzma needs a build with KT_STORE_LZMA.")
KT_KEY(storage_codec_level, "storage.codec_level", Int, 1, Range(-7, 22), Live, "", false,
       "Level of the fresh codec (zstd -7..22, lzma 0..10, 10 = 9e).")
KT_KEY(storage_block_size, "storage.block_size", Size, "64KiB", Range("16KiB", "16MiB"), Live, "", false,
       "Raw bytes per fresh block.")
KT_KEY(storage_cold_codec, "storage.cold_codec", Enum, "zstd", Values("none|zstd|lzma"), Live, "", false,
       "Codec the janitor repacks aged segments with.")
KT_KEY(storage_cold_codec_level, "storage.cold_codec_level", Int, 19, Range(-7, 22), Live, "", false,
       "Level of the cold codec (zstd -7..22, lzma 0..10, 10 = 9e).")
KT_KEY(storage_cold_block_size, "storage.cold_block_size", Size, "1MiB", Range("16KiB", "16MiB"), Live, "",
       false, "Raw bytes per repacked block.")
KT_KEY(storage_cold_after, "storage.cold_after", Duration, "24h", Range("0s", "87600h"), Live, "", false,
       "Age after which the janitor repacks a sealed segment with the cold codec and block size.")
KT_KEY(storage_min_free_disk, "storage.min_free_disk", Size, "1GiB", Range("0B", "1TiB"), Live, "", false,
       "Warm history writes stop below this much free disk space or min_free_disk_percent, whichever is smaller.")
KT_KEY(storage_min_free_disk_percent, "storage.min_free_disk_percent", Float, 2.0, Range(0, 50), Live, "",
       false, "Free disk space in percent below which warm history writes stop (see min_free_disk).")
