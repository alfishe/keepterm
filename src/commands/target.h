// What command handlers act on. The window, the workspace model
// and the terminal view implement it; tests implement a fake.
//
// Every operation has a default that fails with Errc::not_supported, so an integration can
// implement the parts it has and the rest reports "not available" instead of crashing.
// Ids cross this boundary as text (workspace ids are lowercase UUIDs, sessions base32).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/error.h"
#include "commands/context.h"
#include "commands/keys.h"

namespace kt::commands {

// A line of a session's stored output: where its output viewer opens (session/history_output.h).
struct HistoryLine {
    std::uint64_t seq = 0;
    std::string text;
};

using Status = Expected<void, Error>;

enum class Dir : std::uint8_t { Left, Right, Up, Down };
enum class SplitDir : std::uint8_t { Right, Down, Auto };
enum class ScrollTo : std::uint8_t { PageUp, PageDown, Top, Bottom };
enum class FolderAction : std::uint8_t { OpenAsProject, AddProject, OpenAsTab };

enum class PaletteNav : std::uint8_t {
    Next, Previous, PageDown, PageUp, Accept, AcceptAlt, EnterScope, PopScope, Close,
};
enum class SidebarNav : std::uint8_t { Previous, Next, Collapse, Expand, Open, Rename, Remove };

// Cheap facts that availability predicates read (availability < 5 µs). The target
// fills it per invocation; the defaults describe "a window with one empty-ish tab".
struct TargetState {
    bool has_window = true;          // false for the CLI without a running app
    bool has_terminal = true;        // a focused (or last focused) terminal pane
    bool has_project = false;        // the window shows a project (not scratch, not quick)
    bool quick_terminal = false;     // the quick terminal is the key window
    std::uint32_t project_count = 0;
    std::uint32_t tab_count = 1;     // tabs of the shown workspace
    std::uint32_t tab_index = 1;     // 1-based active tab, 0 = none
    std::uint32_t pane_count = 1;    // panes of the active tab
    bool zoomed = false;
    bool tab_pinned = false;
    bool pane_private = false;
    bool pane_read_only = false;
    bool has_session = true;         // the focused pane has a live session
    bool has_selection = false;
    bool has_layout_file = false;    // the project has a layout YAML
    bool pane_cwd_known = false;     // project.replace_path needs one
    bool fullscreen = false;
    bool sidebar_visible = true;
    bool sidebar_has_selection = false;
    bool palette_open = false;
    bool palette_query_empty = true; // palette.pop_scope only acts on an empty field
    bool palette_in_scope = false;
    bool history_enabled = true;     // session.history; private panes work on top
};

class ICommandTarget {
public:
    virtual ~ICommandTarget() = default;

    virtual TargetState state() const { return {}; }
    // A toast with an English message (localized by the UI); unavailable commands, failures.
    virtual void notify(std::string_view message) { (void)message; }

    // ---- palettes
    virtual Status toggle_palette() { return no("palette"); }
    virtual Status open_palette_scope(PaletteScope s) { (void)s; return no("palette scope"); }
    virtual Status palette_nav(PaletteNav n) { (void)n; return no("palette navigation"); }

    // ---- tabs (new_tab, close_tab, rename_tab, move_tab, select_tab ...)
    virtual Status new_tab(bool scratch) { (void)scratch; return no("new tab"); }
    virtual Status close_tab(bool force) { (void)force; return no("close tab"); }
    virtual Status begin_rename_tab() { return no("rename tab"); }
    virtual Status select_tab_relative(int delta, bool across_projects) { (void)delta; (void)across_projects; return no("tab cycling"); }
    virtual Status select_recent_tab() { return no("recent tab"); }
    virtual Status select_tab_index(std::uint32_t index) { (void)index; return no("tab selection"); }   // 1-based
    virtual Status move_tab(int delta) { (void)delta; return no("tab reorder"); }
    virtual Status move_tab_to_project(std::string_view project_id) { (void)project_id; return no("move tab"); }
    virtual Status separate_all_panes() { return no("separate panes"); }
    virtual Status set_tab_pinned(bool pinned) { (void)pinned; return no("pinned tabs"); }

    // ---- panes
    virtual Status close_pane(bool force) { (void)force; return no("close pane"); }
    virtual Status split_pane(SplitDir d) { (void)d; return no("split"); }
    virtual Status separate_pane() { return no("separate pane"); }
    virtual Status toggle_zoom() { return no("zoom"); }
    virtual Status equalize_panes() { return no("equalize"); }
    virtual Status focus_pane(Dir d) { (void)d; return no("pane focus"); }
    virtual Status focus_pane_cycle(int delta) { (void)delta; return no("pane focus"); }
    virtual Status resize_pane(Dir d) { (void)d; return no("pane resize"); }
    virtual Status move_pane(Dir d) { (void)d; return no("pane move"); }
    virtual Status copy_session_id() { return no("copy session id"); }
    virtual Status capture_pane() { return no("capture pane"); }
    virtual Status set_pane_read_only(bool on) { (void)on; return no("read-only mode"); }
    virtual Status set_pane_private(bool on) { (void)on; return no("private panes"); }
    virtual Status restart_session() { return no("restart session"); }

    // ---- terminal (the terminal view)
    virtual Status copy() { return no("copy"); }
    virtual Status paste() { return no("paste"); }
    virtual Status select_all() { return no("select all"); }
    virtual Status open_find() { return no("find"); }
    virtual Status find_step(int delta) { (void)delta; return no("find"); }
    virtual Status clear_screen_and_scrollback() { return no("clear"); }
    virtual Status change_font_size(int delta) { (void)delta; return no("font size"); }   // 0 = reset
    virtual Status scroll(ScrollTo to) { (void)to; return no("scroll"); }
    virtual Status jump_to_prompt(int delta) { (void)delta; return no("prompt marks"); }
    virtual Status toggle_copy_mode() { return no("copy mode"); }
    // The resolver already routes the next chord; this shows the status pill.
    virtual Status show_send_next_key_pill() { return {}; }
    virtual Status send_text(std::string_view text) { (void)text; return no("send text"); }
    virtual Status send_key(const Sequence& key) { (void)key; return no("send key"); }

    // ---- projects
    virtual Status open_project_dialog() { return no("open project"); }
    virtual Status begin_rename_project() { return no("rename project"); }
    virtual Status remove_project(bool force) { (void)force; return no("remove project"); }
    virtual Status replace_project_path() { return no("replace project path"); }
    virtual Status apply_layout() { return no("layouts"); }
    virtual Status save_layout() { return no("layouts"); }

    // ---- workspace save and presets
    virtual Status save_workspace() { return no("save workspace"); }
    virtual Status save_workspace_as() { return no("workspace presets"); }
    virtual Status open_workspace() { return no("workspace presets"); }
    virtual Status manage_workspaces() { return no("workspace presets"); }
    // Opens the named workspace (asks replace or merge).
    virtual Status open_workspace_named(std::string_view name) { (void)name; return no("workspace presets"); }
    // A clean workspace in place of the current one, which is kept as the "Last Workspace" preset.
    virtual Status start_clean() { return no("start clean"); }
    virtual Status restore_last_workspace() { return no("restore last workspace"); }
    // The workspace state file: write a copy / load one in place of the current workspace.
    virtual Status export_workspace() { return no("export workspace"); }
    virtual Status import_workspace() { return no("import workspace"); }
    // The sessions overview: every pane per project and the live sessions no pane shows.
    virtual Status show_sessions() { return no("sessions overview"); }
    // The Session History window: every session, live and closed.
    virtual Status show_session_history() { return no("session history"); }
    // The window with this session selected.
    virtual Status reveal_in_history(std::string_view history_id) { (void)history_id; return no("session history"); }
    // The main action of a history session: Show (live), Attach (background), Restore (closed).
    virtual Status open_history_session(std::string_view history_id) { (void)history_id; return no("session history"); }
    // An output line found in the history (`line`: its stream offset and text, when known), or a
    // command of a closed session (its offset and command line): a running session's pane with
    // the find bar on `query`; a closed session's stored output in a viewer tab, at that line.
    virtual Status open_history_output(std::string_view history_id, std::string_view query,
                                       const std::optional<HistoryLine>& line) {
        (void)history_id;
        (void)query;
        (void)line;
        return no("session history");
    }
    virtual Status select_project_relative(int delta) { (void)delta; return no("project cycling"); }

    // ---- window and view
    virtual Status toggle_sidebar() { return no("sidebar"); }
    virtual Status toggle_resources() { return no("resources panel"); }
    virtual Status toggle_title_bar() { return no("title bar"); }
    virtual Status toggle_fullscreen() { return no("full screen"); }
    virtual Status new_window() { return no("new window"); }
    virtual Status close_window() { return no("close window"); }
    virtual Status sidebar_nav(SidebarNav n) { (void)n; return no("sidebar navigation"); }

    // ---- app
    virtual Status open_settings() { return no("settings"); }
    virtual Status reload_config() { return no("reload config"); }
    virtual Status open_config_file() { return no("open config"); }
    virtual Status toggle_quick_terminal() { return no("quick terminal"); }
    virtual Status quit(bool end_sessions) { (void)end_sessions; return no("quit"); }

    // ---- palette rows (sources)
    virtual Status activate_tab(std::string_view tab_id) { (void)tab_id; return no("go to tab"); }
    virtual Status move_tab_here(std::string_view tab_id) { (void)tab_id; return no("move tab"); }
    virtual Status open_project(std::string_view project_id, bool new_window) { (void)project_id; (void)new_window; return no("open project"); }
    virtual Status open_folder(std::string_view path, FolderAction a) { (void)path; (void)a; return no("open folder"); }
    virtual Status attach_session(std::string_view session_id) { (void)session_id; return no("attach session"); }
    virtual Status kill_session(std::string_view session_id) { (void)session_id; return no("kill session"); }
    virtual Status open_file(std::string_view path, bool with_os_default) { (void)path; (void)with_os_default; return no("open file"); }

protected:
    static Status no(std::string_view what) {
        return make_error(Errc::not_supported, std::string(what) + " is not available here");
    }
};

}  // namespace kt::commands
