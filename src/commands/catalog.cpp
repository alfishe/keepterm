// The built-in command catalog. Registry order is the palette's empty-state
// order and the Keymaps order: Palettes first, then by category.
//
// Titles follow Macterm's AppCommand titles where one exists. Handlers only translate the
// command into an ICommandTarget call; the workspace, the window and the terminal
// view do the work.
#include <string>
#include <utility>

#include "commands/registry.h"

namespace kt::commands {

namespace {

constexpr Ctx kWin = Ctx::Window | Ctx::QuickTerminal;   // tab, pane, project and view commands
constexpr Ctx kTerm = Ctx::Terminal;                     // the focused terminal
constexpr Ctx kAll = Ctx::Always;

using Handler = std::function<Status(CommandContext&, const CommandArgs&)>;
using Avail = std::function<Availability(const CommandContext&)>;

template <class F>
Handler h(F f) {
    return [f](CommandContext& c, const CommandArgs&) -> Status { return f(c.target, c); };
}

Command make(CommandId id, std::string_view title, std::string_view description, Category cat, Ctx ctx,
             Handler handler) {
    Command c;
    c.id = id;
    c.title = title;
    c.description = description;
    c.category = cat;
    c.contexts = ctx;
    c.handler = std::move(handler);
    return c;
}

Availability off(std::string reason, bool notify = true) { return Availability{false, std::move(reason), notify}; }

// ---- availability predicates (cheap: one state() call)

Availability need_window(const CommandContext& c) {
    return c.target.state().has_window ? Availability{} : off("No keepterm window is open", false);
}
Availability need_terminal(const CommandContext& c) {
    const auto s = c.target.state();
    if (!s.has_window) return off("No keepterm window is open", false);
    return s.has_terminal ? Availability{} : off("No terminal pane has focus", false);
}
Availability need_session(const CommandContext& c) {
    const auto s = c.target.state();
    if (!s.has_terminal) return off("No terminal pane has focus", false);
    return s.has_session ? Availability{} : off("The pane has no running session");
}
Availability need_split(const CommandContext& c) {
    const auto s = c.target.state();
    if (!s.has_window) return off("No keepterm window is open", false);
    return s.pane_count > 1 ? Availability{} : off("The tab has only one pane");
}
Availability need_tabs(const CommandContext& c) {
    const auto s = c.target.state();
    if (!s.has_window) return off("No keepterm window is open", false);
    return s.tab_count > 1 ? Availability{} : off("There is only one tab", false);
}
Availability need_project(const CommandContext& c) {
    const auto s = c.target.state();
    if (!s.has_window) return off("No keepterm window is open", false);
    if (s.quick_terminal) return off("The quick terminal has no project");
    return s.has_project ? Availability{} : off("No project is open");
}
Availability need_projects(const CommandContext& c) {
    const auto s = c.target.state();
    if (!s.has_window) return off("No keepterm window is open", false);
    return s.project_count > 1 ? Availability{} : off("There is only one project", false);
}
Availability need_palette(const CommandContext& c) {
    return c.target.state().palette_open ? Availability{} : off("The palette is closed", false);
}

Avail all_of(Avail a, Avail b) {
    return [a, b](const CommandContext& c) {
        Availability x = a(c);
        return x.enabled ? b(c) : x;
    };
}

void add_palettes(CommandRegistry& r) {
    auto scope = [](PaletteScope s) {
        return h([s](ICommandTarget& t, CommandContext&) { return t.open_palette_scope(s); });
    };
    {
        Command c = make("palette.toggle", "Command Palette", "Search commands, projects, tabs, folders and sessions",
                         Category::Palettes, kAll, h([](ICommandTarget& t, CommandContext&) { return t.toggle_palette(); }));
        c.icon = "palette";
        c.palette_visible = false;   // the palette has to be open to see itself
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("palette.files", "Files", "Files and folders of the current project", Category::Palettes, kWin,
                         scope(PaletteScope::Files));
        c.icon = "files";
        c.opens_scope = PaletteScope::Files;
        c.availability = need_project;
        r.add(std::move(c));
    }
    {
        Command c = make("palette.sessions", "Sessions", "Every session: attach, kill or restart", Category::Palettes,
                         kAll, scope(PaletteScope::Sessions));
        c.icon = "sessions";
        c.opens_scope = PaletteScope::Sessions;
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("palette.tabs", "Go to Tab", "Every tab of every project", Category::Palettes, kWin,
                         scope(PaletteScope::Tabs));
        c.icon = "tabs";
        c.opens_scope = PaletteScope::Tabs;
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("palette.workspaces", "Workspaces", "Saved workspaces: open one in place of the current one or next to it",
                         Category::Palettes, kWin, scope(PaletteScope::Workspaces));
        c.icon = "workspace";
        c.opens_scope = PaletteScope::Workspaces;
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("palette.history", "Search History", "Every session of the history and their stored output",
                         Category::Palettes, kWin, scope(PaletteScope::History));
        c.icon = "history";
        c.opens_scope = PaletteScope::History;
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("palette.folders", "Recent Folders", "Folders your shells visited and projects you opened",
                         Category::Palettes, kWin, scope(PaletteScope::Folders));
        c.icon = "folder";
        c.opens_scope = PaletteScope::Folders;
        c.availability = need_window;
        r.add(std::move(c));
    }
    // Palette navigation (Palette context). Hidden from the palette list; bindable.
    struct Nav {
        CommandId id;
        std::string_view title;
        PaletteNav nav;
        bool repeatable;
    };
    const Nav navs[] = {
        {"palette.select_next", "Palette: Select Next", PaletteNav::Next, true},
        {"palette.select_previous", "Palette: Select Previous", PaletteNav::Previous, true},
        {"palette.page_down", "Palette: Page Down", PaletteNav::PageDown, true},
        {"palette.page_up", "Palette: Page Up", PaletteNav::PageUp, true},
        {"palette.accept", "Palette: Run Selected", PaletteNav::Accept, false},
        {"palette.accept_alt", "Palette: Run Alternate Action", PaletteNav::AcceptAlt, false},
        {"palette.enter_scope", "Palette: Enter Scope", PaletteNav::EnterScope, false},
        {"palette.pop_scope", "Palette: Leave Scope", PaletteNav::PopScope, false},
        {"palette.close", "Palette: Close", PaletteNav::Close, false},
    };
    for (const auto& n : navs) {
        const PaletteNav nav = n.nav;
        Command c = make(n.id, n.title, "Palette navigation", Category::Palettes, Ctx::Palette,
                         h([nav](ICommandTarget& t, CommandContext&) { return t.palette_nav(nav); }));
        c.palette_visible = false;
        c.repeatable = n.repeatable;
        c.availability = need_palette;
        if (nav == PaletteNav::PopScope) {
            // Backspace pops the scope only on an empty field; otherwise it edits the field.
            c.yields_when_unavailable = true;
            c.availability = [](const CommandContext& ctx) {
                const auto s = ctx.target.state();
                if (!s.palette_open || !s.palette_in_scope || !s.palette_query_empty)
                    return off("Nothing to leave", false);
                return Availability{};
            };
        }
        r.add(std::move(c));
    }
}

void add_tabs(CommandRegistry& r) {
    {
        Command c = make("tab.new", "New Tab", "A new tab in the current project", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.new_tab(false); }));
        c.icon = "tab.new";
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.new_scratch", "New Scratch Tab", "A new tab in the scratch workspace", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.new_tab(true); }));
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.close", "Close Tab", "Close the tab and end every session in it", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext& ctx) { return t.close_tab(ctx.force); }));
        c.availability = need_window;
        c.destructive = true;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.rename", "Rename Current Tab", "Give the tab a title of its own", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.begin_rename_tab(); }));
        c.availability = need_window;
        r.add(std::move(c));
    }
    auto cycle = [&](CommandId id, std::string_view title, std::string_view desc, int delta, bool across) {
        Command c = make(id, title, desc, Category::Tabs, kWin, h([delta, across](ICommandTarget& t, CommandContext&) {
                             return t.select_tab_relative(delta, across);
                         }));
        c.repeatable = true;
        c.availability = need_window;
        r.add(std::move(c));
    };
    cycle("tab.next", "Next Tab", "The next tab, across projects", 1, true);
    cycle("tab.previous", "Previous Tab", "The previous tab, across projects", -1, true);
    cycle("tab.next_in_project", "Next Tab in Project", "The next tab of this project", 1, false);
    cycle("tab.previous_in_project", "Previous Tab in Project", "The previous tab of this project", -1, false);
    {
        Command c = make("tab.recent", "Recent Tab", "Back to the tab you used before", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.select_recent_tab(); }));
        c.availability = need_tabs;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.select", "Go to Tab N", "Select a tab by its number (multi-digit while held)",
                         Category::Tabs, kWin, [](CommandContext& ctx, const CommandArgs& a) -> Status {
                             const auto i = a.get_int("index").value_or(1);
                             return ctx.target.select_tab_index(static_cast<std::uint32_t>(i));
                         });
        c.palette_visible = false;   // the Go to Tab scope lists them
        c.args = {ArgField{"index", ArgType::Int, true, 1, 0, "1-based tab number"}};
        c.availability = [](const CommandContext& ctx) {
            const auto s = ctx.target.state();
            if (!s.has_window) return off("No keepterm window is open", false);
            return s.tab_count > 0 ? Availability{} : off("There are no tabs", false);
        };
        r.add(std::move(c));
    }
    {
        Command c = make("tab.move_up", "Move Tab Up", "Move the tab one place earlier", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.move_tab(-1); }));
        c.repeatable = true;
        c.availability = need_tabs;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.move_down", "Move Tab Down", "Move the tab one place later", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.move_tab(1); }));
        c.repeatable = true;
        c.availability = need_tabs;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.move_to_project", "Move Tab to Project\xE2\x80\xA6", "Move the tab into another project",
                         Category::Tabs, kWin, [](CommandContext& ctx, const CommandArgs& a) -> Status {
                             if (const auto p = a.get_string("project")) return ctx.target.move_tab_to_project(*p);
                             return ctx.target.open_palette_scope(PaletteScope::MoveTabToProject);
                         });
        c.opens_scope = PaletteScope::MoveTabToProject;
        c.args = {ArgField{"project", ArgType::Id, false, 0, 0, "target project id; without it the palette asks"}};
        c.availability = all_of(need_window, [](const CommandContext& ctx) {
            return ctx.target.state().project_count > 0 ? Availability{} : off("There are no projects");
        });
        r.add(std::move(c));
    }
    {
        Command c = make("tab.separate_panes", "Separate All Panes", "Every pane of the tab becomes a tab of its own",
                         Category::Tabs, kWin, h([](ICommandTarget& t, CommandContext&) { return t.separate_all_panes(); }));
        c.availability = need_split;
        r.add(std::move(c));
    }
    {
        Command c = make("tab.pin", "Pin Tab", "Keep the tab at the front of the project", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.set_tab_pinned(true); }));
        c.availability = all_of(need_window, [](const CommandContext& ctx) {
            return ctx.target.state().tab_pinned ? off("The tab is already pinned", false) : Availability{};
        });
        r.add(std::move(c));
    }
    {
        Command c = make("tab.unpin", "Unpin Tab", "Let the tab move freely again", Category::Tabs, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.set_tab_pinned(false); }));
        c.availability = all_of(need_window, [](const CommandContext& ctx) {
            return ctx.target.state().tab_pinned ? Availability{} : off("The tab is not pinned", false);
        });
        r.add(std::move(c));
    }
}

void add_panes(CommandRegistry& r) {
    {
        Command c = make("pane.close", "Close Pane", "Close the pane; the last pane closes the tab", Category::Panes,
                         kWin, h([](ICommandTarget& t, CommandContext& ctx) { return t.close_pane(ctx.force); }));
        c.availability = need_window;
        r.add(std::move(c));
    }
    auto split = [&](CommandId id, std::string_view title, std::string_view desc, SplitDir d) {
        Command c = make(id, title, desc, Category::Panes, kWin,
                         h([d](ICommandTarget& t, CommandContext&) { return t.split_pane(d); }));
        c.availability = need_window;
        r.add(std::move(c));
    };
    split("pane.split_right", "Split Right", "A new pane to the right", SplitDir::Right);
    split("pane.split_down", "Split Down", "A new pane below", SplitDir::Down);
    split("pane.split_auto", "Split Automatically", "Split along the longer side", SplitDir::Auto);
    {
        Command c = make("pane.separate", "Separate Current Pane", "The pane becomes a tab of its own", Category::Panes,
                         kWin, h([](ICommandTarget& t, CommandContext&) { return t.separate_pane(); }));
        c.availability = need_split;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.zoom", "Zoom Pane", "Show only this pane until zoomed back", Category::Panes, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.toggle_zoom(); }));
        c.availability = need_split;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.equalize", "Equalize Panes", "Give every pane the same size", Category::Panes, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.equalize_panes(); }));
        c.availability = need_split;
        r.add(std::move(c));
    }
    struct DirCmd {
        std::string_view suffix, title_word;
        Dir dir;
    };
    const DirCmd dirs[] = {{"left", "Left", Dir::Left}, {"right", "Right", Dir::Right}, {"up", "Up", Dir::Up},
                           {"down", "Down", Dir::Down}};
    // Ids and titles must outlive the registry: they are string literals below.
    static constexpr std::string_view kFocusIds[] = {"pane.focus_left", "pane.focus_right", "pane.focus_up",
                                                     "pane.focus_down"};
    static constexpr std::string_view kFocusTitles[] = {"Focus Left", "Focus Right", "Focus Up", "Focus Down"};
    static constexpr std::string_view kResizeIds[] = {"pane.resize_left", "pane.resize_right", "pane.resize_up",
                                                      "pane.resize_down"};
    static constexpr std::string_view kResizeTitles[] = {"Resize Pane Left", "Resize Pane Right", "Resize Pane Up",
                                                         "Resize Pane Down"};
    static constexpr std::string_view kMoveIds[] = {"pane.move_left", "pane.move_right", "pane.move_up",
                                                    "pane.move_down"};
    static constexpr std::string_view kMoveTitles[] = {"Move Pane Left", "Move Pane Right", "Move Pane Up",
                                                       "Move Pane Down"};
    for (std::size_t i = 0; i < 4; ++i) {
        const Dir d = dirs[i].dir;
        Command c = make(kFocusIds[i], kFocusTitles[i], "Focus the neighbouring pane", Category::Panes, kWin,
                         h([d](ICommandTarget& t, CommandContext&) { return t.focus_pane(d); }));
        c.repeatable = true;
        c.availability = need_split;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.next", "Next Pane", "Focus the next pane in tree order", Category::Panes, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.focus_pane_cycle(1); }));
        c.repeatable = true;
        c.availability = need_split;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.previous", "Previous Pane", "Focus the previous pane in tree order", Category::Panes, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.focus_pane_cycle(-1); }));
        c.repeatable = true;
        c.availability = need_split;
        r.add(std::move(c));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const Dir d = dirs[i].dir;
        Command c = make(kResizeIds[i], kResizeTitles[i], "Move the divider by one step", Category::Panes, kWin,
                         h([d](ICommandTarget& t, CommandContext&) { return t.resize_pane(d); }));
        c.repeatable = true;
        c.availability = need_split;
        r.add(std::move(c));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const Dir d = dirs[i].dir;
        Command c = make(kMoveIds[i], kMoveTitles[i], "Swap the pane with its neighbour", Category::Panes, kWin,
                         h([d](ICommandTarget& t, CommandContext&) { return t.move_pane(d); }));
        c.availability = need_split;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.copy_session_id", "Copy Session ID", "The pane's session id to the clipboard",
                         Category::Panes, kWin, h([](ICommandTarget& t, CommandContext&) { return t.copy_session_id(); }));
        c.availability = need_session;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.capture", "Capture Pane", "A PNG of the pane to the clipboard", Category::Panes, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.capture_pane(); }));
        c.availability = need_terminal;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.read_only", "Read-Only Mode", "Ignore typing in this pane until turned off",
                         Category::Panes, kWin, h([](ICommandTarget& t, CommandContext&) {
                             return t.set_pane_read_only(!t.state().pane_read_only);
                         }));
        c.availability = need_terminal;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.toggle_private", "Private Pane: Don't Save History",
                         "Nothing of this pane's text is written to disk; turning it on deletes what was saved",
                         Category::Panes, kWin, h([](ICommandTarget& t, CommandContext&) {
                             return t.set_pane_private(!t.state().pane_private);
                         }));
        c.icon = "lock";
        c.availability = need_session;
        r.add(std::move(c));
    }
    {
        Command c = make("pane.restart_session", "Restart Session", "End the pane's session and start a new shell",
                         Category::Panes, kWin, h([](ICommandTarget& t, CommandContext&) { return t.restart_session(); }));
        c.availability = need_terminal;
        c.destructive = true;
        r.add(std::move(c));
    }
}

void add_terminal(CommandRegistry& r) {
    auto simple = [&](CommandId id, std::string_view title, std::string_view desc, Ctx ctx, bool repeatable,
                      Handler handler) -> Command& {
        Command c = make(id, title, desc, Category::Terminal, ctx, std::move(handler));
        c.repeatable = repeatable;
        c.availability = need_terminal;
        r.add(std::move(c));
        return const_cast<Command&>(*r.find(id));
    };
    simple("terminal.copy", "Copy", "Copy the selection", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.copy(); }));
    simple("terminal.paste", "Paste", "Paste the clipboard", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.paste(); }));
    simple("terminal.select_all", "Select All", "Select the screen and the scrollback", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.select_all(); }));
    simple("terminal.find", "Find", "Search the scrollback", kTerm | Ctx::FindBar, false,
           h([](ICommandTarget& t, CommandContext&) { return t.open_find(); }));
    simple("terminal.find_next", "Find Next", "The next match", kTerm | Ctx::FindBar, true,
           h([](ICommandTarget& t, CommandContext&) { return t.find_step(1); }));
    simple("terminal.find_previous", "Find Previous", "The previous match", kTerm | Ctx::FindBar, true,
           h([](ICommandTarget& t, CommandContext&) { return t.find_step(-1); }));
    simple("terminal.clear", "Clear Screen and Scrollback", "Erase the screen and the scrollback", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.clear_screen_and_scrollback(); }));
    simple("terminal.font_bigger", "Increase Font Size", "One point larger", kTerm, true,
           h([](ICommandTarget& t, CommandContext&) { return t.change_font_size(1); }));
    simple("terminal.font_smaller", "Decrease Font Size", "One point smaller", kTerm, true,
           h([](ICommandTarget& t, CommandContext&) { return t.change_font_size(-1); }));
    simple("terminal.font_reset", "Reset Font Size", "Back to the configured size", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.change_font_size(0); }));
    simple("terminal.scroll_page_up", "Scroll Page Up", "One page back in the scrollback", kTerm, true,
           h([](ICommandTarget& t, CommandContext&) { return t.scroll(ScrollTo::PageUp); }));
    simple("terminal.scroll_page_down", "Scroll Page Down", "One page forward", kTerm, true,
           h([](ICommandTarget& t, CommandContext&) { return t.scroll(ScrollTo::PageDown); }));
    simple("terminal.scroll_top", "Scroll to Top", "The start of the scrollback", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.scroll(ScrollTo::Top); }));
    simple("terminal.scroll_bottom", "Scroll to Bottom", "The live screen", kTerm, false,
           h([](ICommandTarget& t, CommandContext&) { return t.scroll(ScrollTo::Bottom); }));
    simple("terminal.prompt_previous", "Jump to Previous Prompt", "Needs shell integration (OSC 133)", kTerm, true,
           h([](ICommandTarget& t, CommandContext&) { return t.jump_to_prompt(-1); }));
    simple("terminal.prompt_next", "Jump to Next Prompt", "Needs shell integration (OSC 133)", kTerm, true,
           h([](ICommandTarget& t, CommandContext&) { return t.jump_to_prompt(1); }));
    simple("terminal.copy_mode", "Copy Mode", "Select and copy with the keyboard", kTerm | Ctx::CopyMode, false,
           h([](ICommandTarget& t, CommandContext&) { return t.toggle_copy_mode(); }));
    {
        // The resolver arms itself when it resolves this command; the handler shows the pill.
        // From the palette (no next chord through the resolver), the window arms its resolver.
        Command c = make("terminal.send_next_key", "Send Next Key to Terminal",
                         "The next key goes to the terminal as is, even if it is bound", Category::Terminal,
                         kTerm | kWin | Ctx::TextInput,
                         h([](ICommandTarget& t, CommandContext&) { return t.show_send_next_key_pill(); }));
        c.availability = need_terminal;
        r.add(std::move(c));
    }
    {
        Command c = make("terminal.send_text", "Send Text", "Write exact text or bytes to the terminal",
                         Category::Terminal, kTerm, [](CommandContext& ctx, const CommandArgs& a) -> Status {
                             return ctx.target.send_text(a.get_string("text").value_or(""));
                         });
        c.palette_visible = false;
        c.args = {ArgField{"text", ArgType::String, true, 0, 0, "text to write (TOML escapes: \\u001b)"}};
        c.availability = need_terminal;
        r.add(std::move(c));
    }
    {
        Command c = make("terminal.send_key", "Send Key", "Send a key, encoded like a typed key", Category::Terminal,
                         kTerm, [](CommandContext& ctx, const CommandArgs& a) -> Status {
                             auto s = parse_sequence(a.get_string("key").value_or(""), ctx.os);
                             if (!s) return make_error(Errc::invalid_argument, s.error().message);
                             return ctx.target.send_key(*s);
                         });
        c.palette_visible = false;
        c.args = {ArgField{"key", ArgType::Chord, true, 0, 0, "a chord: \"ctrl+c\", \"shift+tab\""}};
        c.availability = need_terminal;
        r.add(std::move(c));
    }
}

void add_projects(CommandRegistry& r) {
    {
        Command c = make("project.open", "Open Project", "Open a folder as a project", Category::Projects, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.open_project_dialog(); }));
        c.availability = need_window;
        r.add(std::move(c));
    }
    {
        Command c = make("project.rename", "Rename Current Project", "Change the project's name", Category::Projects,
                         kWin, h([](ICommandTarget& t, CommandContext&) { return t.begin_rename_project(); }));
        c.availability = need_project;
        r.add(std::move(c));
    }
    {
        Command c = make("project.remove", "Remove Current Project", "Remove the project and end its sessions",
                         Category::Projects, kWin,
                         h([](ICommandTarget& t, CommandContext& ctx) { return t.remove_project(ctx.force); }));
        c.availability = need_project;
        c.destructive = true;
        r.add(std::move(c));
    }
    {
        Command c = make("project.replace_path", "Replace Project Path with Current Directory",
                         "The project's folder becomes the focused pane's folder", Category::Projects, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.replace_project_path(); }));
        c.availability = all_of(need_project, [](const CommandContext& ctx) {
            return ctx.target.state().pane_cwd_known ? Availability{} : off("The pane's folder is not known");
        });
        r.add(std::move(c));
    }
    {
        Command c = make("project.apply_layout", "Apply Layout", "Open the tabs and panes of the project's saved layout",
                         Category::Projects, kWin, h([](ICommandTarget& t, CommandContext&) { return t.apply_layout(); }));
        c.availability = all_of(need_project, [](const CommandContext& ctx) {
            return ctx.target.state().has_layout_file ? Availability{} : off("The project has no saved layout");
        });
        r.add(std::move(c));
    }
    {
        Command c = make("project.save_layout", "Save Layout", "Save the project's tabs and panes as its layout",
                         Category::Projects, kWin, h([](ICommandTarget& t, CommandContext&) { return t.save_layout(); }));
        c.availability = need_project;
        r.add(std::move(c));
    }
    {
        Command c = make("project.next", "Next Project", "Show the next project", Category::Projects, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.select_project_relative(1); }));
        c.repeatable = true;
        c.availability = need_projects;
        r.add(std::move(c));
    }
    {
        Command c = make("project.previous", "Previous Project", "Show the previous project", Category::Projects, kWin,
                         h([](ICommandTarget& t, CommandContext&) { return t.select_project_relative(-1); }));
        c.repeatable = true;
        c.availability = need_projects;
        r.add(std::move(c));
    }
}

void add_window(CommandRegistry& r) {
    auto simple = [&](CommandId id, std::string_view title, std::string_view desc, Ctx ctx, Handler handler,
                      Avail avail) {
        Command c = make(id, title, desc, Category::Window, ctx, std::move(handler));
        c.availability = std::move(avail);
        r.add(std::move(c));
    };
    simple("view.toggle_sidebar", "Toggle Sidebar", "Show or hide the project list", Ctx::Window,
           h([](ICommandTarget& t, CommandContext&) { return t.toggle_sidebar(); }), need_window);
    simple("view.toggle_resources", "Toggle Resources Panel", "Memory and processes of the sessions", Ctx::Window,
           h([](ICommandTarget& t, CommandContext&) { return t.toggle_resources(); }), need_window);
    simple("view.toggle_title_bar", "Toggle Title Bar", "Show or hide the title bar", Ctx::Window,
           h([](ICommandTarget& t, CommandContext&) { return t.toggle_title_bar(); }), need_window);
    simple("view.toggle_fullscreen", "Toggle Full Screen", "Enter or leave full screen", Ctx::Window,
           h([](ICommandTarget& t, CommandContext&) { return t.toggle_fullscreen(); }), need_window);
    simple("window.new", "New Window", "A new main window", kAll,
           h([](ICommandTarget& t, CommandContext&) { return t.new_window(); }), {});
    simple("window.close", "Close Window", "Close the window; its sessions keep running", Ctx::Window,
           h([](ICommandTarget& t, CommandContext&) { return t.close_window(); }), need_window);
    // Sidebar navigation (Sidebar context), hidden from the palette.
    struct Nav {
        CommandId id;
        std::string_view title;
        SidebarNav nav;
        bool repeatable;
    };
    const Nav navs[] = {
        {"sidebar.select_previous", "Sidebar: Select Previous", SidebarNav::Previous, true},
        {"sidebar.select_next", "Sidebar: Select Next", SidebarNav::Next, true},
        {"sidebar.collapse", "Sidebar: Collapse", SidebarNav::Collapse, false},
        {"sidebar.expand", "Sidebar: Expand", SidebarNav::Expand, false},
        {"sidebar.open", "Sidebar: Open", SidebarNav::Open, false},
        {"sidebar.rename", "Sidebar: Rename", SidebarNav::Rename, false},
        {"sidebar.remove", "Sidebar: Remove", SidebarNav::Remove, false},
    };
    for (const auto& n : navs) {
        const SidebarNav nav = n.nav;
        Command c = make(n.id, n.title, "Sidebar navigation", Category::Window, Ctx::Sidebar,
                         h([nav](ICommandTarget& t, CommandContext&) { return t.sidebar_nav(nav); }));
        c.palette_visible = false;
        c.repeatable = n.repeatable;
        r.add(std::move(c));
    }
}

void add_app(CommandRegistry& r) {
    auto simple = [&](CommandId id, std::string_view title, std::string_view desc, Handler handler) -> Command& {
        r.add(make(id, title, desc, Category::App, kAll, std::move(handler)));
        return const_cast<Command&>(*r.find(id));
    };
    simple("app.settings", "Settings", "Open the Settings window",
           h([](ICommandTarget& t, CommandContext&) { return t.open_settings(); }));
    simple("app.reload_config", "Reload Config", "Read config.toml again",
           h([](ICommandTarget& t, CommandContext&) { return t.reload_config(); }));
    simple("workspace.save", "Save Workspace", "Write the projects, tabs and panes to disk now",
           h([](ICommandTarget& t, CommandContext&) { return t.save_workspace(); }));
    simple("workspace.save_as", "Save Workspace As\xE2\x80\xA6", "Save the projects, tabs and panes as a named workspace",
           h([](ICommandTarget& t, CommandContext&) { return t.save_workspace_as(); }));
    simple("workspace.open", "Open Workspace\xE2\x80\xA6", "Load a named workspace in place of the current one or next to it",
           h([](ICommandTarget& t, CommandContext&) { return t.open_workspace(); }));
    simple("workspace.manage", "Manage Workspaces\xE2\x80\xA6", "Rename, duplicate, delete or set the default workspace",
           h([](ICommandTarget& t, CommandContext&) { return t.manage_workspaces(); }));
    simple("workspace.start_clean", "Start Clean\xE2\x80\xA6", "A fresh workspace; the current one is kept as \"Last Workspace\"",
           h([](ICommandTarget& t, CommandContext&) { return t.start_clean(); }));
    simple("workspace.restore_last", "Restore Last Workspace", "Open the workspace that Start Clean put aside",
           h([](ICommandTarget& t, CommandContext&) { return t.restore_last_workspace(); }));
    simple("workspace.export", "Export Workspace State\xE2\x80\xA6", "Write a copy of the workspace state file",
           h([](ICommandTarget& t, CommandContext&) { return t.export_workspace(); }));
    simple("workspace.import", "Import Workspace State\xE2\x80\xA6", "Load a workspace state file in place of the current workspace",
           h([](ICommandTarget& t, CommandContext&) { return t.import_workspace(); }));
    simple("session.overview", "Sessions\xE2\x80\xA6", "Every pane's session per project, and the live sessions no tab shows",
           h([](ICommandTarget& t, CommandContext&) { return t.show_sessions(); }));
    simple("session.history", "Session History", "Every session, live and closed: find, restore, resume, tag",
           h([](ICommandTarget& t, CommandContext&) { return t.show_session_history(); }))
        .icon = "history";
    simple("app.open_config", "Open Config File", "Open config.toml in the default editor",
           h([](ICommandTarget& t, CommandContext&) { return t.open_config_file(); }));
    simple("quick_terminal.toggle", "Toggle Quick Terminal", "Show or hide the quick terminal from anywhere",
           h([](ICommandTarget& t, CommandContext&) { return t.toggle_quick_terminal(); }))
        .icon = "quick_terminal";
    simple("app.quit", "Quit keepterm", "Quit; sessions keep running and reattach next time",
           h([](ICommandTarget& t, CommandContext&) { return t.quit(false); }));
    simple("app.quit_end_sessions", "Quit and End All Sessions", "Quit and end every session",
           h([](ICommandTarget& t, CommandContext&) { return t.quit(true); }))
        .destructive = true;
}

}  // namespace

void add_builtin_commands(CommandRegistry& r) {
    add_palettes(r);
    add_tabs(r);
    add_panes(r);
    add_terminal(r);
    add_projects(r);
    add_window(r);
    add_app(r);
}

const CommandRegistry& builtin_registry() {
    static const CommandRegistry r = [] {
        CommandRegistry x;
        add_builtin_commands(x);
        return x;
    }();
    return r;
}

}  // namespace kt::commands
