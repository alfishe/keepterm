// Workspace entities. Core code.
//
// Fields marked "transient" are never written to the state file.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agents/agent_resume.h"
#include "session/ids.h"
#include "workspace/ids.h"
#include "workspace/json.h"
#include "workspace/split_tree.h"

namespace kt::ws {

enum class ColorTag : std::uint8_t { None, Red, Orange, Yellow, Green, Teal, Blue, Purple, Pink, Gray };
std::string_view to_string(ColorTag);
std::optional<ColorTag> color_from_string(std::string_view);

struct ShellSpec {                  // per project or per pane
    std::string program;            // "pwsh", "cmd", "C:/Program Files/Git/bin/bash.exe", "/bin/zsh"
    std::vector<std::string> args;  // empty = the shell's interactive defaults
    bool operator==(const ShellSpec&) const = default;
};

struct Project {
    ProjectId id;
    std::string name;               // display, user-editable; 1..128 bytes after trim
    std::string path;               // absolute local folder as the user chose it
    ColorTag color = ColorTag::None;
    std::optional<ShellSpec> shell;
    json::Value extra;              // unknown keys from the file, written back
    std::uint64_t rev = 0;          // transient: bumps on every change
};

enum class Attention : std::uint8_t { Idle, Running, Done, Failed, Bell };
std::string_view to_string(Attention);
std::optional<Attention> attention_from_string(std::string_view);

// Source of a working directory, in order of trust. Text form = PROCESS_INFO cwd_source.
enum class CwdSource : std::uint8_t { Integration, OsQuery, ProcessPeb, PromptParse, LastKnown };
std::string_view to_string(CwdSource);
std::optional<CwdSource> cwd_source_from_string(std::string_view);

// Restore progress of a pane after its host died. Transient.
enum class RestoreStep : std::uint8_t {
    None,              // attached to a live host, or a new pane
    Respawning,        // step 1: new host spawning with restore_from
    HistoryRestored,   // step 2 done: the first snapshot (with the replayed history) arrived
    Failed,            // spawn failed; the pane shows the error in place and a Retry button
};
std::string_view to_string(RestoreStep);

// Where the pane's host stands (SessionBinder). Transient.
enum class BindState : std::uint8_t {
    Unbound,     // no host asked for yet
    Probing,     // looking for the saved session's host
    Spawning,    // a host is starting
    Ready,       // `Pane::endpoint` accepts clients: the view attaches its link now
    Failed,      // `Pane::bind_error` says why; Retry calls SessionBinder::retry
    Ended,       // the shell ended; the pane is about to close
};
std::string_view to_string(BindState);

struct Pane {
    PaneId id;
    session::SessionId session;                // generated with the pane; swapped on respawn
    std::string last_cwd;                      // respawn directory, '/' separators
    CwdSource last_cwd_source = CwdSource::LastKnown;
    std::optional<ShellSpec> shell;            // overrides the project's shell
    Attention attention = Attention::Idle;     // only Done and Failed are persisted
    bool is_private = false;                   // no history on disk
    bool read_only = false;                    // typing, paste and CLI input are refused (pane.read_only)
    std::optional<agents::AgentResume> agent_resume;   // persisted
    json::Value extra;

    // Transient.
    bool agent_resume_pending = false;         // set after a respawn reached HistoryRestored
    bool never_started = false;                // created in this run, no host yet: spawn, do not probe
    RestoreStep restore = RestoreStep::None;
    BindState bind = BindState::Unbound;
    std::string endpoint;                      // host endpoint when bind == Ready
    std::string bind_error;                    // English, when bind == Failed
    std::optional<std::string> pending_run;    // typed after the first prompt (layouts, CLI)
    std::uint64_t rev = 0;
};

struct Tab {
    TabId id;
    std::optional<std::string> title_override;   // at most 256 bytes
    SplitTree tree;
    PaneId focused;
    std::optional<PaneId> zoomed;
    bool pinned = false;                         // tab.pin / tab.unpin
    json::Value extra;

    std::vector<PaneId> focus_history;           // transient: MRU, most recent first, max 16
    std::uint64_t rev = 0;
};

struct ProjectWorkspace {              // one per project, plus scratch and quick
    ProjectId project;
    std::vector<Tab> tabs;             // display order
    std::optional<TabId> active_tab;   // fallback for windows that have none
    json::Value extra;
};

struct WindowState {
    WindowId id;
    ProjectId shown = kScratchProject;   // a project or kScratchProject
    std::optional<TabId> active_tab;     // per window
    RectDip frame{};                     // last normal (restored) frame, DIP
    std::string monitor_key;             // the platform's monitor identity
    double monitor_scale = 1.0;
    bool maximized = false, fullscreen = false;
    double sidebar_width = 240;
    bool sidebar_visible = true;
    std::vector<ProjectId> expanded;     // sidebar sections, kept sorted
    bool key = false;                    // the focused window (at save time)
    bool quick = false;                  // the quick terminal's window: shows kQuickProject, never saved
    json::Value extra;
    std::uint64_t rev = 0;
};

struct RecentFolder {
    std::string path;                    // as reported, '/' separators
    std::int64_t used_unix_ms = 0;
};
inline constexpr std::size_t kMaxRecentFolders = 200;

struct RectFraction {                    // fractions of a monitor's work area
    double x = 0, y = 0, w = 0, h = 0;
    bool operator==(const RectFraction&) const = default;
};

struct QuickTerminalState {
    ProjectWorkspace workspace;          // project == kQuickProject
    std::map<std::string, RectFraction> frames;   // per monitor key
    json::Value extra;
};

// Where a pane lives.
struct PaneLocation {
    ProjectId project;                   // a project, kScratchProject or kQuickProject
    TabId tab;
};

// The whole model as one value: what the state file holds, plus transient fields. Immutable once
// taken with Workspace::snapshot(); any thread may read it.
struct StateSnapshot {
    std::vector<Project> projects;                // sidebar order
    std::vector<ProjectWorkspace> workspaces;     // one per project and one for scratch
    QuickTerminalState quick;
    std::vector<WindowState> windows;
    std::vector<RecentFolder> recent_folders;     // most recent first
    std::map<PaneId, Pane> panes;                 // every pane of every tab
    json::Value extra;                            // unknown top-level keys
};

}  // namespace kt::ws
