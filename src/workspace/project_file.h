// Project files: one YAML file per project in <config>/projects,
// Macterm's format, so a Macterm project file works in keepterm unchanged and back. A file
// declares the project's path and, if wanted, its tabs and panes. "Save Layout" writes it,
// "Apply Layout" reconciles the live tabs toward it: panes that already match keep their
// sessions, the rest are spawned or closed. Core code over IFileSystem.
//
//     name: api               # optional, display only
//     path: ~/work/api        # required: the identity matched against projects
//     tabs:                   # optional; absent or empty = a bare declaration
//     - name: dev             # a tab is a node: a leaf (cwd, run, shell) or a split
//       split:
//         direction: horizontal      # horizontal = side by side, vertical = stacked
//         ratio: 0.6
//         first: { run: "npm run dev" }
//         second: {}
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/error.h"
#include "workspace/fs.h"
#include "workspace/paths.h"
#include "workspace/presets.h"

namespace kt::ws {

class Workspace;

// The tree of a declared tab reuses the preset types: PresetPane.cwd is the file's `cwd` as
// written (empty = the project root), `shell` its `shell` (program only), `run` its `run`.
struct ProjectFile {
    std::optional<std::string> name;
    std::string path;
    std::optional<std::vector<PresetTab>> tabs;
    bool operator==(const ProjectFile&) const = default;
};

struct ProjectHeader {
    std::optional<std::string> name, path;
};

// The modeline editors use to attach the format's JSON schema (shared with Macterm).
inline constexpr std::string_view kProjectSchemaModeline =
    "# yaml-language-server: $schema=https://macterm.thdxg.dev/schemas/project.schema.json";

// Errors: LayoutParse with "line L, column C: ..." (syntax) or the key at fault (schema).
// Unknown keys are ignored (Macterm's `zmxPath`, later additions).
WsResult<ProjectFile> parse_project_file(std::string_view yaml);
// Only `name` and `path`, so a file whose `tabs:` is wrong still says which project it declares.
// nullopt only when the YAML itself does not parse.
std::optional<ProjectHeader> parse_project_header(std::string_view yaml);
// The modeline, then name, path, tabs. Ratios with 2 decimals.
std::string write_project_file(const ProjectFile& f);

// Macterm's slug rules: lowercase, whitespace -> '_', letters, digits, '-' and '_' kept,
// everything else dropped; empty -> "project".
std::string project_slug(std::string_view name);
// "<slug>.yaml" for attempt 1, "<slug>_<n>.yaml" after.
std::string slug_filename(std::string_view slug, int attempt);
// Whether `filename` is one `slug` produces (extension and ASCII case ignored).
bool slug_owns(std::string_view filename, std::string_view slug);

// The comparison key of a project path: '~' expanded against `home`, then canonical_path.
// nullopt for a remote spec ("[user@]host:dir"; a drive letter is not a host).
std::optional<std::string> project_path_key(std::string_view path, std::string_view home, PathStyle style = kHostPathStyle);
// A remote spec ([user@]host:dir): keepterm has no remote projects yet.
bool is_remote_spec(std::string_view path, PathStyle style = kHostPathStyle);
// A declared cwd as an absolute folder: empty -> the root, '~' -> home, absolute as is,
// otherwise relative to the root. '/' separators on Windows are kept.
std::string resolve_layout_cwd(std::string_view cwd, std::string_view root, std::string_view home,
                               PathStyle style = kHostPathStyle);
// The reverse for Save Layout: empty for the root, relative inside the root, "~/..." under home
// (POSIX), else the folder as is.
std::string layout_cwd_of(std::string_view folder, std::string_view root, std::string_view home,
                          PathStyle style = kHostPathStyle);
// A project path as Save Layout writes it: "~/..." under home (POSIX, as Macterm), else as is.
std::string layout_path_of(std::string_view path, std::string_view home, PathStyle style = kHostPathStyle);

// The folders of project files: the first is keepterm's own (written), the others are read only
// (Macterm's, when the user turns it on).
class ProjectFileStore {
public:
    ProjectFileStore(IFileSystem& fs, std::string dir, std::vector<std::string> read_only_dirs, std::string home)
        : fs_(fs), dir_(std::move(dir)), ro_(std::move(read_only_dirs)), home_(std::move(home)) {}

    struct Entry {
        std::string file;                    // full path
        std::string name;                    // file name
        bool read_only = false;
        std::optional<ProjectHeader> header; // nullopt: the YAML does not parse
        std::string error;                   // the parse error, when any
    };
    // Every *.yaml / *.yml, keepterm's folder first, each folder sorted by name.
    std::vector<Entry> list() const;

    struct Found {
        std::string file;
        std::string text;
        bool read_only = false;
        std::size_t duplicates = 0;          // other files declaring the same path
    };
    // The file of the project at `path`: the files whose `path:` matches; among several, the one
    // the project's slug owns; keepterm's folder before the read-only ones.
    std::optional<Found> find(std::string_view path, std::string_view project_name) const;

    // Writes `f` for the project `project_name`: over the project's own file in keepterm's folder
    // when there is one, else into a new "<slug>.yaml" ("<slug>_2.yaml" ... when taken). The path.
    WsResult<std::string> save(const ProjectFile& f, std::string_view project_name);

    const std::string& dir() const { return dir_; }

private:
    IFileSystem& fs_;
    std::string dir_;
    std::vector<std::string> ro_;
    std::string home_;
};

// ---------------------------------------------------------------- Apply Layout: the plan

// What a live pane is now, for matching (Macterm's LayoutReconciler).
struct LivePaneFacts {
    std::optional<std::string> command;   // the foreground command line; nullopt at the prompt
    std::string shell;                    // the shell's program name ("zsh", "pwsh"), lowercase
    std::string cwd;                      // the live folder
};
using LivePaneFactsFn = std::function<LivePaneFacts(const Pane&)>;

struct LayoutPlanTab {
    std::optional<TabId> existing;        // the live tab it matched (by name, else by position)
    std::optional<std::string> title;
    PresetNode tree;                      // leaves: resolved cwd, shell, run
    std::vector<std::optional<PaneId>> keep;   // per leaf in tree order: the live pane it keeps
};

struct LayoutPlan {
    std::vector<LayoutPlanTab> tabs;
    std::vector<PaneId> destroy;          // live panes no leaf matched (also those of closed tabs)
    std::vector<TabId> close;             // live tabs no declared tab matched
    std::vector<PaneId> destroy_busy;     // of `destroy`, those running a program
    bool destructive() const { return !destroy.empty() || !close.empty(); }
    std::size_t kept() const;
};

// A command leaf keeps a live pane running that exact command line in that folder; a plain leaf
// keeps an idle pane (positionally), one with `shell:` only an idle pane of that shell. Tabs match
// by name, else the next unmatched live tab. Pure: reads the workspace and the facts only.
LayoutPlan plan_layout(const Workspace& ws, ProjectId project, const std::vector<PresetTab>& tabs, std::string_view root,
                       std::string_view home, const LivePaneFactsFn& facts);

struct LayoutApplyResult {
    std::size_t tabs = 0, kept = 0, spawned = 0, closed = 0, detached = 0;
    std::vector<std::pair<PaneId, PaneId>> adopted;   // (old pane, the new pane with its session)
};
// Carries out the plan in one transaction: new tabs in the declared order (kept panes adopt their
// sessions, with their private, read-only and agent settings), then the old tabs go. Unmatched
// panes end, or keep running detached when `keep_sessions`.
WsResult<LayoutApplyResult> apply_layout_plan(Workspace& ws, ProjectId project, const LayoutPlan& plan, bool keep_sessions);

// Save Layout: the project's live tabs as a file (`run` from the facts' command, `cwd` relative
// to the project, `shell` only when the pane's differs from the project's).
std::optional<ProjectFile> capture_project_file(const Workspace& ws, ProjectId project, std::string_view home,
                                                const LivePaneFactsFn& facts);

}  // namespace kt::ws
