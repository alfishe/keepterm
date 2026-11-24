// The workspace model: projects, tabs, splits, windows. Core code.
//
// Owned by the UI thread: every call must be made on the thread that created the Workspace (a
// debug assertion checks it). Mutations return WsResult and change nothing when they fail.
// Every successful mutation emits Change records; all records of one operation (or of an
// explicit Transaction) reach the subscribers as one batch, synchronously, at commit.
// Subscribers must not mutate the model inside the callback; post the request to the UI loop.
//
// Sessions: the model generates a pane's session id when it creates the pane and reports
// panes that end through PaneRemoved (with the session id). SessionBinder (binder.h) listens to
// the bus, starts and ends hosts, and writes the binding facts back through the binder_* calls.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "workspace/change.h"
#include "workspace/error.h"
#include "workspace/model.h"

namespace kt::ws {

struct WorkspaceOptions {
    std::string home_dir;                  // start folder of scratch and quick panes, '/' separators
    bool auto_tile = false;                // splits.auto_tile: equalize after split and close
    bool close_last_pane_closes_tab = true;   // tabs.close_last_pane_closes_tab
};

struct NewPaneSpec {
    std::optional<std::string> cwd;        // default: the focused pane's cwd, else the project path
    std::optional<ShellSpec> shell;
    std::optional<std::string> run;        // typed after the prompt is ready (layouts, CLI)
    bool is_private = false;
    // A live session the pane takes over (the sessions list's "Attach"): the binder attaches
    // instead of spawning. The caller makes sure no other pane claims it.
    std::optional<session::SessionId> adopt;
};

struct NewTabOptions {
    std::optional<std::size_t> at;         // default: after the active tab
    bool focus = true;                     // false (CLI --no-focus): no selection change at all
    std::optional<WindowId> window;        // the window that selects the new tab (default: the key
                                           // window if it shows the project)
};

// Answers "does this pane run something other than an idle shell" (from the host's PROCESS_INFO).
// Unset: never busy.
using BusyProbe = std::function<bool(const Pane&)>;

class Workspace {
public:
    Workspace(IRandom& rng, IClock& clock, WorkspaceOptions opts = {});
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    void set_options(WorkspaceOptions opts) { opts_ = std::move(opts); }
    const WorkspaceOptions& options() const { return opts_; }
    void set_busy_probe(BusyProbe probe) { busy_ = std::move(probe); }

    // ------------------------------------------------------------ projects
    WsResult<ProjectId> add_project(std::string name, std::string path, std::optional<std::size_t> at = {});
    // "Open folder" (file manager, palette): the first project whose canonical path matches, or a
    // new one named after the folder.
    WsResult<ProjectId> find_or_add_project(std::string path);
    WsResult<void> rename_project(ProjectId, std::string name);
    WsResult<void> set_project_color(ProjectId, ColorTag);
    WsResult<void> set_project_shell(ProjectId, std::optional<ShellSpec>);
    WsResult<void> set_project_path(ProjectId, std::string path);   // project.replace_path
    WsResult<void> move_project(ProjectId, std::size_t to);
    // Ends the sessions of every pane of the project. force=false: Busy when a pane is busy.
    WsResult<void> remove_project(ProjectId, bool force);
    // Like remove_project / close_tab, but the sessions keep running, detached: no kill is sent
    // (PaneRemoved carries field::kDetached) and the Sessions view lists them for attaching.
    WsResult<void> detach_project(ProjectId);
    WsResult<void> detach_tab(TabId);

    // ------------------------------------------------------------ tabs
    // `project` may be a project, kScratchProject or kQuickProject. One pane with a new session.
    WsResult<TabId> new_tab(ProjectId project, NewPaneSpec spec = {}, NewTabOptions opts = {});
    WsResult<void> close_tab(TabId, bool force);
    WsResult<void> rename_tab(TabId, std::optional<std::string> title);
    WsResult<void> set_tab_pinned(TabId, bool pinned);
    WsResult<void> move_tab(TabId, std::size_t to);                  // reorder inside its project
    // Sessions stay untouched. `activate_in`: a drag into a window that shows `to`.
    WsResult<void> move_tab_to_project(TabId, ProjectId to, std::optional<std::size_t> at = {},
                                       std::optional<WindowId> activate_in = {});
    WsResult<void> select_tab(WindowId, TabId);
    // The project's own active tab (the one a window shows when it switches to the project),
    // without touching any window (presets, layouts).
    WsResult<void> set_active_tab(TabId);
    // tab.separate_panes: every pane but the focused one becomes its own tab (after this tab).
    WsResult<std::vector<TabId>> separate_panes(TabId);

    // ------------------------------------------------------------ panes
    WsResult<PaneId> split_pane(PaneId, Direction, NewPaneSpec spec = {}, bool focus = true);
    WsResult<void> close_pane(PaneId, bool force);
    WsResult<void> focus_pane(PaneId);
    WsResult<void> focus_direction(PaneId from, Direction);
    WsResult<void> focus_next(PaneId from);                   // tree order, wraps (pane.next)
    WsResult<void> focus_previous(PaneId from);               // pane.previous
    WsResult<void> resize_pane(PaneId, Direction, double delta = kResizeStep);
    WsResult<void> set_split_ratio(PaneId, Axis, double ratio);
    // Divider drag; the view passes the cell-minimum bounds.
    WsResult<void> set_branch_ratio(TabId, NodeRef, double ratio, double min_ratio, double max_ratio);
    WsResult<void> equalize(TabId);
    WsResult<void> toggle_zoom(PaneId);
    WsResult<void> move_pane(PaneId, Direction);              // swap with the neighbor
    WsResult<void> move_pane_to_tab(PaneId, TabId target, PaneId next_to, Direction side);
    WsResult<TabId> break_pane_to_tab(PaneId);                // pane.separate
    // The pane's shell ended by itself (EXIT_STATUS): the pane goes away without a kill.
    WsResult<void> pane_exited(PaneId);

    // ------------------------------------------------------------ windows
    WindowId open_window(ProjectId shown);
    // The quick terminal's window: shows the quick workspace, is not saved and
    // cannot show a project. One at a time; a second call returns the first.
    WindowId open_quick_window();
    WsResult<void> close_window(WindowId);                    // sessions keep running
    WsResult<void> show_project(WindowId, ProjectId);
    WsResult<void> update_window_frame(WindowId, RectDip, std::string monitor_key, double scale, bool maximized,
                                       bool fullscreen);
    WsResult<void> update_sidebar(WindowId, double width, bool visible, std::vector<ProjectId> expanded);
    WsResult<void> set_key_window(WindowId);

    // ------------------------------------------------------------ pane facts from other modules
    // From the host (CWD, PROCESS_INFO). `last_known` never replaces a stored value.
    void set_pane_cwd(PaneId, std::string cwd, CwdSource);
    void set_pane_attention(PaneId, Attention);
    WsResult<void> set_pane_private(PaneId, bool);            // the binder forwards it to the host
    WsResult<void> set_pane_read_only(PaneId, bool);          // the GUI refuses input to the pane
    WsResult<void> set_pane_shell(PaneId, std::optional<ShellSpec>);   // takes effect at the next spawn
    void set_agent_resume(PaneId, std::optional<agents::AgentResume>);
    void clear_agent_resume_pending(PaneId);
    void clear_pending_run(PaneId);                           // the view typed Pane::pending_run

    // ------------------------------------------------------------ session binding (SessionBinder only)
    void binder_set_session(PaneId, session::SessionId);      // id swap; persistent, saved at once
    void binder_set_bind(PaneId, BindState, std::string endpoint = {}, std::string error = {});
    void binder_set_restore(PaneId, RestoreStep);
    void binder_set_agent_pending(PaneId, bool);
    // Fires after a commit that swapped a session id (the save scheduler writes at once).
    bool last_commit_swapped_session() const { return last_commit_swap_; }

    // ------------------------------------------------------------ quick terminal
    const ProjectWorkspace& quick_workspace() const { return s_.quick.workspace; }
    const std::map<std::string, RectFraction>& quick_frames() const { return s_.quick.frames; }
    void update_quick_frame(std::string monitor_key, RectFraction);

    // ------------------------------------------------------------ recent folders
    void note_folder(std::string path);
    std::span<const RecentFolder> recent_folders() const { return s_.recent_folders; }
    void forget_folder(std::string_view path);

    // ------------------------------------------------------------ queries
    const Project* project(ProjectId) const;
    std::span<const Project> projects() const { return s_.projects; }   // sidebar order
    // A project's, scratch's or quick's workspace.
    const ProjectWorkspace* workspace_of(ProjectId) const;
    const Tab* tab(TabId) const;
    const Pane* pane(PaneId) const;
    const WindowState* window(WindowId) const;
    std::span<const WindowState> windows() const { return s_.windows; }
    std::optional<WindowId> key_window() const;
    // The tab a window shows: its active tab, else the workspace's active tab, else the first.
    const Tab* shown_tab(WindowId) const;
    std::optional<PaneLocation> locate(PaneId) const;
    std::optional<PaneLocation> locate(const session::SessionId&) const;
    std::optional<PaneLocation> locate_tab(TabId) const;
    // Display name: the project name, "Scratch" or "Quick Terminal".
    std::string display_name(ProjectId) const;
    std::vector<session::SessionId> claimed_sessions() const;  // orphan detection
    std::size_t attention_count() const;                       // badge
    // Attach order: panes of visible tabs (key window first), then the rest in
    // sidebar order (projects, scratch), then the quick terminal's panes.
    std::vector<PaneId> panes_in_attach_order() const;
    // The canonical comparison form of a path.
    std::string canonical(std::string_view path) const;
    const StateSnapshot& state() const { return s_; }
    StateSnapshot snapshot() const { return s_; }

    // ------------------------------------------------------------ observation
    Subscription subscribe(ChangeHandler handler);
    Transaction begin() { return Transaction(this); }

    // Replaces the model (after load, after repair). Emits one Reset.
    void load(StateSnapshot state);
    // The default first-launch state: scratch with one tab, one window showing it.
    void reset_to_default();

private:
    friend class Transaction;
    friend class Subscription;

    void check_thread() const;
    void emit(Change c);
    void tx_open();
    void tx_close(bool deliver);
    void unsubscribe(std::uint64_t id);

    ProjectWorkspace* ws_mut(ProjectId);
    Tab* tab_mut(TabId, ProjectWorkspace** owner = nullptr);
    Pane* pane_mut(PaneId);
    WindowState* window_mut(WindowId);
    Project* project_mut(ProjectId);

    Pane& create_pane(ProjectId project, const NewPaneSpec& spec, const Pane* origin);
    std::string start_dir(ProjectId project, const NewPaneSpec& spec, const Pane* origin) const;
    bool any_busy(std::span<const PaneId> panes) const;
    void destroy_pane(PaneId pane, ProjectId project, TabId tab, bool exited);
    void remove_tab_at(ProjectWorkspace& w, std::size_t index, bool destroy_panes, bool exited);
    void fix_active_after_removal(ProjectWorkspace& w, std::size_t removed_index, TabId removed);
    void record_focus(Tab& t, PaneId p);
    void set_focus(ProjectId project, Tab& t, PaneId p, bool clear_zoom);
    void touch_tab(ProjectId project, Tab& t, ChangeKind kind);
    void pane_changed(Pane& p, std::uint32_t fields, bool persistent);
    void window_changed(WindowState& w, std::uint32_t fields, bool persistent = true);
    void select_in_windows(ProjectId project, TabId tab, std::optional<WindowId> only);
    void update_attention_count();
    std::size_t compute_attention() const;
    bool visible_and_focused(PaneId) const;

    IRandom& rng_;
    IClock& clock_;
    WorkspaceOptions opts_;
    BusyProbe busy_;
    StateSnapshot s_;
    std::thread::id owner_;

    // Transactions and delivery.
    int tx_depth_ = 0;
    std::vector<Change> batch_;
    bool batch_aborted_ = false;
    bool delivering_ = false;
    std::deque<std::vector<Change>> queued_batches_;   // batches committed during delivery
    bool tx_swapped_ = false;
    bool last_commit_swap_ = false;
    bool detaching_ = false;                         // detach_project / detach_tab in progress
    std::size_t attention_cached_ = 0;
    struct Sub {
        std::uint64_t id;
        ChangeHandler fn;
    };
    std::vector<Sub> subs_;
    std::uint64_t next_sub_ = 1;
};

}  // namespace kt::ws
