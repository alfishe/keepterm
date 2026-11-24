// The resource popover. Left: a live tree project → tab → pane (session)
// → processes with PID, memory and CPU, sortable by memory or CPU, with a selection. Right: the
// selected node: memory and CPU charts over the time the panel is open, the memory breakdown,
// CPU time, threads, start time, command line, folder, containment; for a group the top
// processes by memory and by CPU. Kill and kill-tree with a confirmation; "copy as text".
//
// The application fills ResourceTrees from the collector on every monitor tick and
// calls ResourcePanel::refresh(); the panel reads names from the workspace snapshot.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ui/widgets.h"
#include "ui/workspace_view.h"

namespace kt::ui {

struct ProcItem {
    uint32_t pid = 0;
    uint64_t start = 0;                 // OS-native start time (identity across PID reuse)
    std::string name;                   // image name, or the command line when known
    std::string role;                   // "host", "shell", "agent", "console host", "" (workload)
    std::string tip;                    // the metric breakdown for the tooltip
    std::string cmdline, cwd;           // when the OS reports them (foreground chain), else ""
    uint64_t memory = 0, subtree_memory = 0;   // main metric, bytes
    uint64_t resident = 0, private_resident = 0, peak = 0, committed = 0;   // 0 = not provided by the OS
    uint64_t cpu_ns = 0;                // user + system, cumulative
    uint32_t threads = 0;
    uint64_t start_unix_ms = 0;         // 0 = unknown
    float cpu = 0, subtree_cpu = 0;            // percent of one core
    bool no_access = false;
    bool killable = true;               // false for keepterm's own processes
    bool keepterm = false;              // keepterm's own process (session host, console agent)
    std::vector<ProcItem> children;
};

// The user's processes of one session: the shell (or the program the pane runs) and everything
// below it. keepterm's own processes (session host, console agent) are not in `roots` and not
// in the totals; their cost is reported apart as overhead.
struct SessionTree {
    std::string session;
    std::string containment;            // "job", "cgroup", "none" ...
    std::string accuracy;               // "exact", "best effort"
    uint64_t memory = 0;                // the user's processes only
    float cpu = 0;
    std::vector<ProcItem> roots;        // usually one: the shell
    uint64_t overhead_memory = 0;       // keepterm's processes of the session
    float overhead_cpu = 0;
    uint32_t overhead_processes = 0;
};

using ResourceTrees = std::map<PaneKey, SessionTree>;

// Fills `out` from a session's whole tree: the user's processes (the first non-keepterm node on
// every path, with its whole subtree) become roots and the totals; keepterm's nodes above them
// count as overhead.
void split_session_tree(ProcItem whole, SessionTree& out);

struct ResourceScope {
    enum class Kind : uint8_t { all, project, tab, pane } kind = Kind::all;
    uint64_t key = 0;                   // ProjectKey, TabKey or PaneKey
    // The page to open on: 0 the last one used, 1 Processes, 2 Agent.
    uint8_t page = 0;
};

struct ResourcePanelHooks {
    std::function<const ResourceTrees&()> trees;
    std::function<void(const ProcItem&, bool tree)> kill;   // after the user confirmed
    std::function<void(const std::string&)> copy;
    std::function<void()> refresh;   // ask the agents for their figures now (the agent page opened)
};

class ResourceTable;
class ResourceHeader;
class SegmentedControl;

class ResourcePanel final : public Widget {
public:
    // The column the tree is sorted by (a click on a column header; again: the other direction).
    enum class Sort : uint8_t { name, pid, memory, cpu, context };
    struct Row {
        enum class Kind : uint8_t { project, tab, pane, process } kind = Kind::process;
        int depth = 0;
        uint64_t key = 0;               // collapse key
        std::string name, detail, tip;
        std::string args;               // process rows: the arguments after the program, dim after the name
        uint64_t memory = 0;
        float cpu = 0;
        uint32_t pid = 0;               // process rows
        PaneKey pane = 0;               // pane rows and the row of a single-pane tab
        float context = -1;             // agent context fill 0..1 (pane rows), -1 = not reported
        bool has_children = false, collapsed = false, killable = false, valid = true;
        std::string containment;        // pane rows (and tab rows of a single pane)
        std::string overhead;           // "keepterm overhead: 2 processes, 12 MB" (pane / tab rows)
        ProcItem proc;                  // process rows: the node; children = every descendant, flat
        std::vector<ProcItem> procs;    // group rows: every process below, flat
    };

    ResourcePanel(IWorkspaceView& view, ResourcePanelHooks hooks, ResourceScope scope);
    void refresh();
    // A new sample arrived (monitor tick): refresh and extend the charts.
    void tick();
    void select(uint64_t key);
    uint64_t selected() const { return selected_; }
    const Row* selected_row() const;
    void set_expanded(bool large);
    // The popover's two pages: the process tree with its figures, and the agents' sessions.
    enum class Page : uint8_t { processes, agent };
    void set_page(Page p);
    Page page() const { return page_; }
    // Sorts by `s`; the same column again flips the direction. Name and PID start ascending,
    // the figures descending.
    void set_sort(Sort s);
    Sort sort() const { return sort_; }
    bool descending() const { return desc_; }
    bool has_context() const;           // some row reports an agent context fill
    void toggle(uint64_t key);
    const std::vector<Row>& rows() const { return rows_; }
    std::string as_text() const;
    void confirm_kill(size_t row, bool tree);
    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(24); }
    std::string describe() const override;
    void on_attached() override;

private:
    void add_proc(const ProcItem& p, int depth, std::vector<Row>& out);
    bool before(const std::string& an, uint32_t ap, uint64_t am, float ac, float ax, const std::string& bn, uint32_t bp,
                uint64_t bm, float bc, float bx) const;
    void update_detail();
    void update_agent();
    void fit_tree_width();
    std::vector<const PaneItem*> scope_agent_panes() const;
    IWorkspaceView& view_;
    ResourcePanelHooks hooks_;
    ResourceScope scope_;
    Sort sort_ = Sort::memory;
    bool desc_ = true;
    std::set<uint64_t> collapsed_;
    std::vector<Row> rows_;
    uint64_t selected_ = 0;
    bool large_ = false;
    std::map<uint64_t, std::vector<float>> mem_hist_, cpu_hist_;
    Label* title_ = nullptr;
    Box* detail_ = nullptr;
    Button* expand_ = nullptr;
    Label* total_ = nullptr;
    ResourceTable* table_ = nullptr;
    ResourceHeader* header_ = nullptr;
    ScrollView* scroll_ = nullptr;
    Widget* body_ = nullptr;
    Box* agent_ = nullptr;
    ScrollView* agent_scroll_ = nullptr;   // holds agent_
    SegmentedControl* pages_ = nullptr;
    Page page_ = Page::processes;
    std::string agent_sig_;          // what the Agent page shows, to rebuild only on change
    float tree_w_ = 0;               // the tree's width (fits the names, see fit_tree_width)
};

// Opens the panel as a popover below `anchor` (window DIP). Returns the overlay id.
// `above`: open above the anchor (the status bar).
WidgetId show_resource_panel(Window&, IWorkspaceView&, ResourcePanelHooks, ResourceScope, Rect anchor, bool above = false);

std::string format_cpu(float percent);   // "12%", "0.4%"

}  // namespace kt::ui
