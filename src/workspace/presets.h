// Workspace presets: the whole project tree (projects, their tabs and splits, working folders,
// shells and, optionally, the agent resume records) saved under a name and loaded later, either
// in place of the current workspace or next to it.
//
// File format (`<config>/workspaces/<name>.json`, readable and hand-editable):
//
//   {
//     "format": "keepterm-workspace", "version": 1, "name": "backend",
//     "projects": [
//       { "name": "api", "path": "/src/api", "color": "orange", "scratch": false,
//         "shell": { "program": "zsh", "args": [] },            // optional, the project default
//         "active": 0,                                           // index of the active tab
//         "tabs": [
//           { "title": "server",                                 // optional
//             "focused": 1,                                      // pane index in tree order
//             "layout": { "split": { "axis": "horizontal", "ratio": 0.6,
//                                    "first":  { "pane": { "cwd": "/src/api" } },
//                                    "second": { "pane": { "cwd": "/src/api/web", "run": "npm run dev",
//                                                          "shell": { "program": "bash", "args": [] },
//                                                          "agent": { "agent": "claude", "session_id": "...",
//                                                                     "directory": "/src/api", "arguments": [] } } } } } }
//         ] }
//     ]
//   }
//
// "axis" is "horizontal" (first | second) or "vertical" (first over second). The scratch
// workspace is a project entry with "scratch": true (name and path ignored). Unknown keys are
// ignored. Core code: no file system access (the store is preset_store.h).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agents/agent_resume.h"
#include "workspace/error.h"
#include "workspace/json.h"
#include "workspace/model.h"
#include "workspace/split_tree.h"

namespace kt::ws {

class Workspace;

inline constexpr std::string_view kPresetFormat = "keepterm-workspace";
inline constexpr int kPresetVersion = 1;

struct PresetPane {
    std::string cwd;
    std::optional<ShellSpec> shell;
    std::optional<std::string> run;                 // typed once the prompt is ready
    std::optional<agents::AgentResume> agent;
    bool operator==(const PresetPane&) const;
};

// A split tree whose leaves are panes.
struct PresetNode {
    std::optional<PresetPane> pane;                 // set: a leaf
    Axis axis = Axis::Horizontal;
    double ratio = 0.5;
    std::unique_ptr<PresetNode> first, second;

    PresetNode() = default;
    PresetNode(const PresetNode&);
    PresetNode& operator=(const PresetNode&);
    PresetNode(PresetNode&&) noexcept = default;
    PresetNode& operator=(PresetNode&&) noexcept = default;
    bool operator==(const PresetNode&) const;
    std::size_t pane_count() const;
    // Leaves in tree order (first before second).
    void leaves(std::vector<const PresetPane*>& out) const;
};

struct PresetTab {
    std::optional<std::string> title;
    PresetNode layout;
    std::size_t focused = 0;                        // leaf index in tree order
    bool operator==(const PresetTab&) const = default;
};

struct PresetProject {
    std::string name, path;
    bool scratch = false;
    ColorTag color = ColorTag::None;
    std::optional<ShellSpec> shell;
    std::vector<PresetTab> tabs;
    std::size_t active = 0;
    bool operator==(const PresetProject&) const = default;
};

struct Preset {
    std::string name;
    std::vector<PresetProject> projects;
    bool operator==(const Preset&) const = default;
    std::size_t pane_count() const;
    bool has_agents() const;
};

struct CaptureOptions {
    bool include_agents = true;                     // the agent resume records of the panes
    bool include_scratch = true;                    // the scratch tabs, when it has any
    // The pane's live working folder (the host's answer, newer than Pane::last_cwd, which is
    // the respawn folder); empty or unset: last_cwd.
    std::function<std::string(PaneId)> live_cwd;
};

// The current project tree of `ws` (projects in sidebar order, then scratch).
Preset capture_preset(const Workspace& ws, std::string name, CaptureOptions opts = {});
// A whole saved state (an imported workspace.json) as a preset; null: the default state (Start
// Clean). Agent records included.
Preset preset_from_state(const StateSnapshot* state, std::string name, IRandom& rng, IClock& clock, std::string home);
// Only one project (the per-project layout: "Save layout…").
std::optional<PresetProject> capture_project(const Workspace& ws, ProjectId project, bool include_agents,
                                             const std::function<std::string(PaneId)>& live_cwd = {});

json::Value preset_to_json(const Preset&);
std::string preset_to_text(const Preset&);         // pretty JSON with a trailing newline
WsResult<Preset> preset_from_json(const json::Value&);
WsResult<Preset> preset_from_text(std::string_view);
json::Value project_to_json(const PresetProject&);
WsResult<PresetProject> project_from_json(const json::Value&);

struct PresetLoad {
    enum class Mode : std::uint8_t {
        Replace,   // the preset's projects take the place of the current ones
        Merge,     // the preset's projects are added after the current ones
    };
    Mode mode = Mode::Replace;
    // Replace only: true = the current panes' sessions keep running, detached (the Sessions view
    // lists them for attaching); false = they end.
    bool keep_sessions = true;
    bool include_agents = true;                     // restore the agent records of the preset
    // The line that resumes an agent in a pane with that shell (nullopt: unsupported). Unset:
    // the record is kept on the pane but nothing is typed.
    std::function<std::optional<std::string>(const agents::AgentResume&, const std::optional<ShellSpec>&)> resume_line;
};

struct PresetLoadResult {
    std::vector<ProjectId> projects;                // created (Merge, Replace) in preset order
    std::size_t tabs = 0, panes = 0, agents = 0;
    std::size_t detached = 0, closed = 0;           // Replace: panes of the old workspace
};

WsResult<PresetLoadResult> apply_preset(Workspace& ws, const Preset& preset, const PresetLoad& how);

// A preset name usable as a file name on every OS: 1..64 bytes, no path separators, no
// control characters, none of <>:"|?*, not "." or "..", no leading or trailing space or dot.
bool valid_preset_name(std::string_view name);

}  // namespace kt::ws
