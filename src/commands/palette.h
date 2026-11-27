// The command palette engine: sources, scopes, prefix shortcuts, path
// mode, alt actions, ranking with source boosts and frecency. Logic only; the palette widget
// (the UI's PaletteView) renders a Result and calls run().
//
// Typical use by the widget:
//
//     engine.open(ctx);                         // palette shown; the context is a snapshot
//     const Result& r = engine.search(field_text);
//     ... draw r.sections; Enter: engine.run(row, /*alt=*/false); Tab: engine.enter(row)
//     Backspace on an empty field (fresh press): engine.pop_scope()
//
// Rows point into the engine's caches: valid until the next search(), open() or scope change.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "commands/context.h"
#include "commands/keymap.h"
#include "commands/keys.h"
#include "commands/matcher.h"
#include "commands/registry.h"
#include "commands/target.h"

namespace kt::commands::palette {

class PreviewSink {
public:
    virtual ~PreviewSink() = default;
    virtual void text(std::string_view label, std::string_view value) = 0;
};

struct AltAction {
    std::string title;   // shown as the subtitle while Alt / Option is held
    std::function<void()> run;
};

struct Item {
    std::string key;                    // stable: "cmd:pane.split_right", "prj:<id>", "tab:<id>", "dir:<path>", "ses:<id>"
    std::string title, subtitle;
    // A second line under the title (a line of output); its marks (byte offset, length) are
    // drawn highlighted.
    std::string detail;
    std::vector<std::pair<std::size_t, std::size_t>> detail_marks;
    std::string category;               // section header in the empty state
    std::string_view icon;
    std::vector<std::string> aliases;   // English title, path, project name
    std::optional<Sequence> keys;       // key caps
    bool enabled = true;
    std::string disabled_reason;
    bool checked = false;               // a toggle that is on (Private Pane)
    std::optional<PaletteScope> opens_scope;
    std::function<void()> run;
    std::optional<AltAction> alt;       // Alt+Enter / Option+Return
    std::function<void(PreviewSink&)> preview;
};

// ---- the data sources read (a snapshot from the workspace)

struct ProjectInfo {
    std::string id, name, path;
    std::int64_t last_used_ms = 0;
};
struct TabInfo {
    std::string id, project_id, project_name, title;
    std::string program;                // foreground program
    std::string status;                 // "running", "done", "bell" ...
    bool pinned = false, scratch = false, current = false;
};
struct FolderInfo {
    std::string path;
    std::int64_t used_ms = 0;
};
struct SessionInfo {
    std::string id, title, program;
    enum class State : std::uint8_t { Attached, Orphaned, Outdated } state = State::Attached;
    std::uint64_t memory_bytes = 0;
};
// A session of the history: the History scope.
struct HistoryInfo {
    std::string id, title, project, cwd;
    enum class State : std::uint8_t { Live, Background, Closed } state = State::Closed;
    std::vector<std::string> agents, tags;
    std::int64_t last_used_ms = 0, ended_ms = 0;
    bool pinned = false;
    std::vector<std::string> agent_names;   // the agents as people know them ("Claude Code"), same order
};
// A line of stored output that matched (store search).
struct OutputHit {
    std::string history_id, title, project, text;
    std::int64_t ts_ms = 0;
    std::size_t match_offset = 0, match_len = 0;   // the match in text
    std::uint64_t seq = 0;                         // with text: the line in the output viewer
};
// The output search of the History scope. It runs off the UI thread; hits() starts the search of
// a new query and returns null until it finished (the palette searches again when it did).
class IHistorySearch {
public:
    virtual ~IHistorySearch() = default;
    virtual const std::vector<OutputHit>* hits(std::string_view query) = 0;
};
struct WorkspaceInfo {   // a named workspace (<config>/workspaces/<name>.json)
    std::string name, summary;
    bool is_default = false;
};
struct PaletteData {
    std::vector<ProjectInfo> projects;  // sidebar order
    std::vector<TabInfo> tabs;          // every tab in every project
    std::vector<FolderInfo> folders;    // most recent first (≤ 200)
    std::vector<SessionInfo> sessions;
    std::vector<WorkspaceInfo> workspaces;   // the default first
    std::string current_project_id;
    // The History scope: the sessions (read when the scope opens) and the output search.
    std::function<std::vector<HistoryInfo>()> history;
    IHistorySearch* history_search = nullptr;
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    virtual std::string home() const = 0;
    virtual bool is_dir(std::string_view path) const = 0;
    // Names of the sub-folders of `dir` (no "." or ".."), any order; empty on error.
    virtual std::vector<std::string> list_dirs(std::string_view dir) const = 0;
};
// std::filesystem; does not follow linked folders into other trees.
std::unique_ptr<IFileSystem> make_std_file_system();

// Palette usage (palette_usage.json, owned by the workspace). Read through
// this interface; record() is called on every run.
class IUsageStore {
public:
    virtual ~IUsageStore() = default;
    virtual std::span<const std::int64_t> timestamps(std::string_view key) const = 0;   // newest first
    virtual void record(std::string_view key, std::int64_t now_ms) = 0;
};

// An in-memory store with the file's rules (last 10 timestamps and a count per key, at most
// 2,000 keys, least recent evicted): tests, the CLI, and the app until the workspace provides one.
class MemoryUsageStore : public IUsageStore {
public:
    static constexpr std::size_t kMaxKeys = 2000, kMaxStamps = 10;
    std::span<const std::int64_t> timestamps(std::string_view key) const override;
    void record(std::string_view key, std::int64_t now_ms) override;
    std::size_t size() const { return entries_.size(); }
    std::uint64_t count(std::string_view key) const;

private:
    struct Entry {
        std::string key;
        std::uint64_t n = 0;
        std::vector<std::int64_t> t;
    };
    std::vector<Entry> entries_;
};

// F = Σ w(age), w = 100 (< 4 h), 70 (< 1 day), 50 (< 3 days), 30 (< 1 week), 10 (< 4 weeks).
double frecency_value(std::span<const std::int64_t> timestamps, std::int64_t now_ms);
// min(32, ⌊6 · ln(1 + F/10)⌋).
int frecency_boost(std::span<const std::int64_t> timestamps, std::int64_t now_ms);

struct SourceContext {
    ICommandTarget* target = nullptr;            // runs the rows' actions
    const CommandRegistry* registry = nullptr;   // commands source
    const Keymap* keymap = nullptr;              // key caps
    const config::Config* config = nullptr;
    Os os = current_os();
    KeyContext keys;                             // the window's context before the palette opened
    TargetState state;
    const PaletteData* data = nullptr;
    const IFileSystem* fs = nullptr;             // path mode
    std::int64_t now_ms = 0;
};

enum class QueryMode : std::uint8_t { All, Commands, Projects, Tabs, Sessions, Path };

struct Query {
    std::string raw;
    std::string text;          // after the prefix shortcut, trimmed
    QueryMode mode = QueryMode::All;
    PaletteScope scope = PaletteScope::Root;
    SearchQuery terms;
    // Root: ">" commands, "@" projects, "#" tabs, "!" sessions; "~", "/", "C:\", "\\" path mode.
    static Query parse(std::string_view raw, PaletteScope scope);
    bool empty() const { return text.empty(); }
};

class Source {
public:
    virtual ~Source() = default;
    virtual std::string_view id() const = 0;
    // Whether this source answers in `scope` for `mode`.
    virtual bool serves(PaletteScope scope, QueryMode mode) const = 0;
    // Every candidate of the scope; the engine matches and ranks. Called once per open and
    // scope unless query_dependent().
    virtual void collect(const Query& q, const SourceContext& c, std::vector<Item>& out) = 0;
    // The empty-query rows; default: none.
    virtual void collect_empty(const SourceContext& c, std::vector<Item>& out) { (void)c; (void)out; }
    virtual int boost() const { return 0; }   // added to the score (Projects +8, Tabs +4)
    // Path mode: items depend on the query text and are not matched by the engine.
    virtual bool query_dependent() const { return false; }
};

// The built-in sources.
std::unique_ptr<Source> make_command_source();
std::unique_ptr<Source> make_project_source();   // also the Move Tab to Project scope
std::unique_ptr<Source> make_tab_source();
std::unique_ptr<Source> make_folder_source();
std::unique_ptr<Source> make_session_source();
std::unique_ptr<Source> make_workspace_source();
std::unique_ptr<Source> make_path_source();
std::unique_ptr<Source> make_history_source();          // the History scope's sessions
std::unique_ptr<Source> make_history_output_source();   // its "In output" section
// All of the above in source order, filtered by palette.sources (empty = all).
std::vector<std::unique_ptr<Source>> make_builtin_sources(std::span<const std::string> enabled = {});

struct Row {
    const Item* item = nullptr;
    std::int32_t score = 0;                  // fuzzy + source boost + frecency
    std::vector<std::uint32_t> highlights;   // byte offsets in title; only the visible rows
    std::uint16_t source = 0;                // index of the source
};
struct Section {
    std::string header;                      // "" for the flat ranked list
    std::vector<Row> rows;
};
struct Result {
    std::vector<Section> sections;
    PaletteScope scope = PaletteScope::Root;
    QueryMode mode = QueryMode::All;
    std::size_t matched = 0;                 // before the limit
    bool narrowed = false;                   // searched the last result set
    const Row* first_enabled() const;
    std::size_t row_count() const;
};

struct EngineOptions {
    bool frecency = true;                    // palette.frecency
    std::size_t recent_projects = 5;         // palette.recent_projects
    std::size_t recent_items = 5;            // the Recent section
    std::size_t visible = 50;                // rows that get highlights
};

class Engine {
public:
    Engine(std::vector<std::unique_ptr<Source>> sources, IUsageStore* usage, EngineOptions o = {});

    void set_options(EngineOptions o) { opts_ = o; }
    // The palette opened (or the model changed under it): drop caches, root scope.
    void open(const SourceContext& c);
    void invalidate();                       // keep the scope, recollect on the next search

    const Result& search(std::string_view raw, std::size_t limit = 200);
    const Result& result() const { return result_; }

    // Enter: runs the row (records usage) or enters its scope. Alt runs the alt action when
    // the row has one. Disabled rows do nothing and return false.
    bool run(const Row& r, bool alt = false);
    // Tab on a row with opens_scope.
    bool enter(const Row& r);
    void push_scope(PaletteScope s);
    bool pop_scope();                        // false at the root
    PaletteScope scope() const { return scopes_.back(); }
    std::span<const PaletteScope> scopes() const { return scopes_; }

private:
    struct Prepared {
        std::vector<Item> items;
        std::vector<std::vector<SearchText>> fields;   // title, aliases..., subtitle
        std::vector<std::uint16_t> source;
        bool ready = false;
    };
    Prepared& cache_for(const Query& q);
    void collect_into(Prepared& p, const Query& q);
    void empty_state(const Query& q);
    int usage_boost(const Item& it) const;

    std::vector<std::unique_ptr<Source>> sources_;
    IUsageStore* usage_;
    EngineOptions opts_;
    SourceContext ctx_;
    std::vector<PaletteScope> scopes_{PaletteScope::Root};
    // [scope][mode]: one row per PaletteScope (History is the last), one per QueryMode (Path).
    static constexpr std::size_t kScopes = static_cast<std::size_t>(PaletteScope::History) + 1;
    static constexpr std::size_t kModes = static_cast<std::size_t>(QueryMode::Path) + 1;
    Prepared caches_[kScopes][kModes];
    std::vector<Item> path_items_;
    std::vector<Item> empty_items_;
    Scratch scratch_;
    Result result_;
    std::optional<Query> last_query_;
    std::vector<std::uint32_t> last_matches_;
};

}  // namespace kt::commands::palette
