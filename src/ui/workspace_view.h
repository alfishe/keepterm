// What the main window's workspace widgets show and what they ask for.
// The widgets read a WorkspaceSnapshot and call IWorkspaceView operations; the
// application implements the interface over the workspace model or, until that is
// wired, over the in-memory model in app/memory_workspace.h. Ids are opaque 64-bit keys.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "render/draw_list.h"
#include "ui/geometry.h"
#include "ui/theme.h"
#include "ui/widget.h"

namespace kt::ui {

using ProjectKey = uint64_t;
using TabKey = uint64_t;
using PaneKey = uint64_t;

enum class Attention : uint8_t { idle, running, done, failed, bell };
enum class SplitDir : uint8_t { right, down, left, up };

// Resource figures of a pane, a tab or a project.
struct Resources {
    bool valid = false;
    uint64_t memory = 0;      // bytes, private / unique set
    float cpu = 0;            // percent of one core
};

// What an agent reports about its session (the agent context telemetry), for the resource
// popover's Agent tab and the indicator's tooltip. Token counts are 0 when not reported.
struct AgentLimitItem {
    std::string name;            // "five_hour", "seven_day", "primary" ...
    float used_percent = 0;      // 0..100
    uint32_t window_minutes = 0;
    uint64_t resets_at_unix = 0;  // 0 = unknown
};

struct AgentStats {
    bool valid = false;          // the host answered with figures (or at least the model)
    std::string session_id, session_name, session_cwd, model, agent_version;
    uint64_t used_tokens = 0, window_tokens = 0;
    float percent = -1;          // 0..100, -1 = unknown
    uint64_t input_tokens = 0, output_tokens = 0, cache_read_tokens = 0, cache_write_tokens = 0;
    std::optional<double> cost_usd;
    uint32_t turns = 0, compactions = 0, api_calls = 0;
    bool after_compaction = false;
    std::string used_from, window_from;   // "status line", "transcript", "table", "config" ...
    uint64_t updated_unix_ms = 0;
    std::vector<AgentLimitItem> limits;
    // When the limits were current (the agent's API response they came with), 0 = unknown. An
    // account's limits are shared: the newest report of any pane of the same agent wins.
    uint64_t limits_unix_ms = 0;
};

struct PaneItem {
    PaneKey id = 0;
    std::string title;        // the terminal's title (OSC 0/2)
    std::string cwd;          // last known working directory
    std::string process;      // foreground program name, "" when unknown (the shell)
    uint32_t pid = 0;         // the foreground process (the shell when none runs), 0 = unknown
    std::string agent;        // "claude", "codex", ... when an agent runs in the pane
    uint32_t processes = 0;   // processes of the pane's tree (shell and below), 0 = unknown
    uint64_t uptime_s = 0;    // how long the agent (else the session's shell) runs, 0 = unknown
    // The agent's context window fill 0..1, -1 = not reported. No provider fills it yet (the
    // agent context research lands later); the header and the resource panel show it when set.
    float context = -1;
    uint64_t context_tokens = 0, context_limit = 0;
    // The agent session the pane's host resumes after a respawn (empty: none), and the pane's
    // "Resume on Restore" switch.
    std::string saved_agent, saved_agent_session;
    bool agent_restore = true;
    bool at_prompt = false;   // the foreground is the idle shell
    std::string model;        // the agent's model id ("claude-opus-4-6") when reported
    // What the agent says it does (agents::Activity: 0 unknown, 1 busy, 2 waiting, 3 idle), from
    // its own status (Claude's pid file) via the agent context telemetry.
    uint8_t agent_activity = 0;
    AgentStats agent_stats;
    Attention attention = Attention::idle;
    Resources res;
    bool exited = false;
    std::optional<int> exit_code;
    bool read_only = false;
};

struct TabItem {
    TabKey id = 0;
    std::string title;                // the override, else the focused pane's title
    std::vector<PaneKey> panes;       // tree order
    PaneKey focused = 0;
    std::optional<PaneKey> zoomed;
    bool pinned = false;
};

struct ProjectItem {
    ProjectKey id = 0;
    std::string name, path;
    ProjectColor color = ProjectColor::none;
    std::vector<TabItem> tabs;        // display order
    TabKey active_tab = 0;
    bool expanded = true;
    bool scratch = false;             // the scratch workspace (no folder)
};

struct WorkspaceSnapshot {
    std::vector<ProjectItem> projects;    // sidebar order; scratch last when present
    std::map<PaneKey, PaneItem> panes;
    ProjectKey shown = 0;                 // the project this window shows
    float sidebar_width = 220;
    bool sidebar_visible = true;
    uint64_t rev = 0;                     // bumps on every change
    std::string home;                     // the user's home folder, '/' separators (paths show as ~/...)

    const ProjectItem* project(ProjectKey k) const;
    const ProjectItem* shown_project() const { return project(shown); }
    const TabItem* tab(TabKey k, const ProjectItem** owner = nullptr) const;
    const TabItem* shown_tab() const;
    const PaneItem* pane(PaneKey k) const;
};

// A laid-out split tree in DIPs (the view asks the model, like ws::SplitTree::layout()).
struct PaneRect {
    PaneKey pane = 0;
    Rect r;
};
struct DividerRect {
    uint64_t ref = 0;         // the branch, for set_divider_ratio
    bool vertical = false;    // a vertical line between left and right children
    Rect r;                   // the divider line (thickness = the divider width)
    Rect parent;              // the branch's area
    float ratio = 0.5f;
};

// Storage figures for the status area (the history store's usage(), else a stub).
struct StorageUsage {
    bool valid = false;
    uint64_t used = 0, limit = 0;   // bytes
    std::string label;              // "History 1.2 GB of 5 GB"
};

class IWorkspaceView {
public:
    virtual ~IWorkspaceView() = default;
    virtual const WorkspaceSnapshot& snapshot() const = 0;

    // Projects.
    virtual void show_project(ProjectKey) = 0;
    virtual void set_expanded(ProjectKey, bool) = 0;
    virtual void new_project() = 0;   // the host asks for a folder (dialog / picker)
    virtual void rename_project(ProjectKey, std::string name) = 0;
    virtual void set_project_color(ProjectKey, ProjectColor) = 0;
    virtual void move_project(ProjectKey, size_t to) = 0;
    virtual void remove_project(ProjectKey) = 0;

    // Tabs.
    virtual void new_tab(ProjectKey) = 0;
    virtual void select_tab(TabKey) = 0;
    virtual void close_tab(TabKey) = 0;
    virtual void rename_tab(TabKey, std::string title) = 0;   // empty: back to the pane title
    virtual void move_tab(TabKey, ProjectKey to_project, size_t to) = 0;

    // Panes.
    virtual void split(PaneKey, SplitDir) = 0;
    virtual void close_pane(PaneKey) = 0;
    virtual void focus_pane(PaneKey) = 0;
    virtual void toggle_zoom(PaneKey) = 0;
    virtual void layout(TabKey, Rect area, float divider, std::vector<PaneRect>& panes,
                        std::vector<DividerRect>& dividers) const = 0;
    virtual void set_divider_ratio(TabKey, uint64_t ref, float ratio) = 0;
    // A pane dragged by its header and dropped on a side of another pane of the shown tab.
    virtual void dock_pane(PaneKey, PaneKey /*target*/, SplitDir /*side*/) {}

    // Window.
    virtual void set_sidebar(float width, bool visible) = 0;

    // Host facilities used by the widgets (clipboard of a path, reveal in the file manager).
    virtual void copy_text(const std::string&) {}
    // Opens a folder in the file manager; false when the host cannot (headless, no opener).
    virtual bool open_folder(const std::string&) { return false; }
};

// Where the view stands in the scrollback, for the overlay scrollbar.
struct ScrollMetrics {
    uint64_t history = 0;   // lines above the live screen (0 on the alternate screen)
    uint32_t rows = 0;      // the screen
    uint32_t offset = 0;    // rows the view is scrolled up from the live screen
    uint32_t fg = 0xFFFFFF;  // the terminal theme's foreground (0xRRGGBB): the thumb's color
};

// A terminal pane's renderer and input sink, implemented by the application (one per pane).
class ITerminalSurface {
public:
    virtual ~ITerminalSurface() = default;
    // The pane's content rect in physical pixels of the window.
    virtual void set_bounds(render::IRect px) = 0;
    // Draws into the window's list. `full`: everything (else damage only). Adds damage (px).
    virtual void paint(render::DrawList& out, bool full, std::vector<render::IRect>& damage) = 0;
    // True when the terminal has a frame to draw (the shell asks for a window frame).
    virtual bool dirty() const = 0;
    virtual void set_focused(bool) = 0;
    virtual void key(const KeyEvent&) = 0;
    virtual void text(std::string_view) = 0;
    virtual void pointer(const PointerEvent&, Vec2 local_dip) = 0;
    virtual bool wheel(const WheelEvent&, Vec2 local_dip) = 0;
    virtual Rect caret_rect_dip() const = 0;   // relative to the bounds' top-left
    virtual void preedit(std::string_view text, int caret) = 0;
    // The program asked for mouse reports: a right-click goes to it (Shift+right-click: the menu).
    virtual bool wants_mouse() const { return false; }
    // The link under the pointer (OSC 8 or a detected URL), "" when none: the widget shows it.
    virtual std::string hover_link() const { return {}; }
    virtual ScrollMetrics scroll_metrics() const { return {}; }
    virtual void scroll_to_offset(uint32_t /*rows_up*/) {}
};

}  // namespace kt::ui
