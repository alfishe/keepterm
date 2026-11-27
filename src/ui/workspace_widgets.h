// The main window's workspace widgets: sidebar with projects and tabs, title
// bar with the tab switcher, split container with draggable dividers, pane frames with the
// embedded terminal, the status area. MainView assembles them over an IWorkspaceView.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/resource_panel.h"
#include "ui/widgets.h"
#include "ui/workspace_view.h"

namespace kt::ui {

std::string format_bytes(uint64_t bytes);          // "812 MB", "1.2 GB"
// The status bar's history figures: "1.2 GB of 5 GB" under a limit; without one (0 or
// UINT64_MAX, the store's "off") the size alone and no limit.
StorageUsage storage_usage(uint64_t used, uint64_t limit);
// The last component of a path for the title bar ("keepterm", "Dev"); a root stays ("C:\\", "/").
std::string folder_label(const std::string& path);
std::string agent_state(const PaneItem& p);         // "running", "waiting for input", "done", "idle"
std::string format_uptime(uint64_t seconds);        // "45s", "12m", "1h 0m", "2d 3h"
std::string format_context_percent(float percent);   // "4%", "<1%", "100%": whole numbers
std::string format_tokens(uint64_t n);              // "850", "37.5k", "200k", "1M"
std::string agent_title(const std::string& id);     // "claude" -> "Claude Code"
// "37.5k / 200k (18.8%)", "37.5k", "" when nothing is known.
std::string context_line(const AgentStats& a);
Icon agent_icon(std::string_view agent);           // Icon::agent for known agents
Color agent_color(const Theme&, std::string_view agent);

class MainView;

// ---------------------------------------------------------------- the embedded terminal

// What a TerminalWidget asks of the view it sits in: the main window's MainView, or a window
// that shows one terminal of its own (the output viewer).
class ITerminalHost {
public:
    virtual ~ITerminalHost() = default;
    virtual ITerminalSurface* terminal(PaneKey) = 0;
    // A right click (the pane menu); none: the terminal gets it.
    virtual bool terminal_menu(PaneKey, Vec2 /*window_pos*/) { return false; }
    // The terminal took the keyboard focus.
    virtual void terminal_focused(PaneKey) {}
};

class TerminalWidget final : public Widget, public ImeClient {
public:
    TerminalWidget(ITerminalHost& host, PaneKey pane);
    ~TerminalWidget() override;
    PaneKey pane() const { return pane_; }
    ITerminalSurface* surface() const;

    bool paint_external(Compositor&) override;
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_wheel(WheelEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    EventResult on_text(std::string_view) override;
    void on_focus(FocusChange) override;
    bool accepts_focus() const override { return true; }
    ImeClient* ime_client() override { return this; }
    platform::CursorShape cursor_at(Vec2) const override;
    std::string describe() const override { return "terminal"; }
    // The link preview: the URL under the pointer in a pill at the bottom-left corner.
    void paint_over(PaintContext&) const override;
    void on_hover(bool inside) override;
    const std::string& shown_link() const { return link_; }

    void ime_preedit(std::string_view text, std::span<const ImeSpan>, int caret) override;
    void ime_commit(std::string_view text) override;
    void ime_cancel() override;
    Rect ime_caret_rect() const override;

private:
    void sync_link();
    // The overlay scrollbar: shown while the view is scrolled up, for a moment after a scroll,
    // and while the pointer is near the right edge. The thumb drags; a click on the track pages.
    struct Bar {
        Rect track, thumb;
        bool valid = false;
    };
    Bar bar_geometry(const ScrollMetrics&) const;
    bool bar_shown(const ScrollMetrics&) const;
    void bar_flash();                      // shown for a moment, then a repaint hides it
    bool bar_pointer(PointerEvent&);       // true when the scrollbar took the event

    ITerminalHost& host_;
    PaneKey pane_;
    std::string link_;
    mutable ScrollMetrics painted_{};      // what the bar showed last; a change repaints it
    bool bar_hover_ = false, bar_drag_ = false;
    float drag_grab_ = 0;                  // pointer y inside the thumb at the press
    TimePoint flash_until_{};
    uint64_t flash_timer_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // the flash timer checks it
};

// ---------------------------------------------------------------- pane frame

class PaneFrame final : public Widget {
public:
    PaneFrame(MainView& mv, PaneKey pane);
    PaneKey pane() const { return pane_; }
    void update(const PaneItem& p, bool focused, bool multi, bool zoomed);
    TerminalWidget& terminal() { return *term_; }
    void paint_over(PaintContext&) const override;
    EventResult on_pointer(PointerEvent&) override;

private:
    MainView& mv_;
    PaneKey pane_;
    Box* header_ = nullptr;
    IconView* icon_ = nullptr;
    Label* title_ = nullptr;
    Label* cwd_ = nullptr;
    Badge* agent_ = nullptr;
    Badge* mem_ = nullptr;
    Button* zoom_ = nullptr;
    Button* close_ = nullptr;
    Badge* zoom_pill_ = nullptr;
    Badge* ro_pill_ = nullptr;   // pane.read_only
    Box* exit_box_ = nullptr;
    Label* exit_label_ = nullptr;
    TerminalWidget* term_ = nullptr;
    bool focused_ = false, multi_ = false;
    Animated dim_;
    // A press on the header (not on its buttons) may become a pane drag.
    bool grab_ = false, dragging_ = false;
    Vec2 grab_at_{};
};

// ---------------------------------------------------------------- split container

class DividerHandle final : public Widget {
public:
    DividerHandle(MainView& mv, TabKey tab);
    void set(const DividerRect& d) { d_ = d; invalidate_paint(); }
    const DividerRect& divider() const { return d_; }
    void paint(PaintContext&) const override;
    EventResult on_pointer(PointerEvent&) override;
    platform::CursorShape cursor_at(Vec2) const override {
        return d_.vertical ? platform::CursorShape::resize_ew : platform::CursorShape::resize_ns;
    }

private:
    MainView& mv_;
    TabKey tab_;
    DividerRect d_;
    bool dragging_ = false;
};

class SplitContainer final : public Widget {
public:
    SplitContainer(MainView& mv);
    void update(const WorkspaceSnapshot&, const TabItem* tab);
    void arrange(const Rect&) override;
    PaneFrame* frame_for(PaneKey) const;
    TabKey tab() const { return tab_; }

private:
    MainView& mv_;
    TabKey tab_ = 0;
    std::optional<PaneKey> zoomed_;
    std::vector<PaneFrame*> frames_;
    std::vector<DividerHandle*> dividers_;
};

// ---------------------------------------------------------------- sidebar

class Sidebar final : public Widget {
public:
    explicit Sidebar(MainView& mv);
    void update(const WorkspaceSnapshot&);
    // True at points of the header strip (above the project list) that drag the window.
    bool is_caption(Vec2 local) const;
    void paint(PaintContext&) const override;
    void paint_over(PaintContext&) const override;

    // Drag reorder (rows call these while they hold the pointer capture).
    void drag_move(Widget& row, Vec2 window_pos);
    void drag_end(Widget& row, Vec2 window_pos, bool cancel);
    bool dragging() const { return drag_row_ != nullptr; }

private:
    struct Drop {
        bool valid = false;
        ProjectKey project = 0;
        size_t index = 0;   // project index, or tab index inside `project`
        float y = 0;        // indicator, local
        float x = 0;
    };
    Drop drop_for(Widget& row, Vec2 local) const;
    MainView& mv_;
    Widget* top_ = nullptr;           // header: [traffic lights] [menu] [sidebar toggle] [search history]
    Button* menu_ = nullptr;
    Button* toggle_ = nullptr;
    Button* search_ = nullptr;
    ScrollView* scroll_ = nullptr;
    Box* list_ = nullptr;
    Widget* drag_row_ = nullptr;
    Drop drop_;
};

// ---------------------------------------------------------------- title bar and tab switcher

// Next to the title: ONE compact indicator of the shown tab's user processes: CPU (with one
// tiny sparkline), memory and, when an agent runs, its context fill. The whole widget is one
// click target: it opens the resource popover for the tab. The tooltip has the foreground
// program, its pid and state, the process count and the uptime.
class ResourceIndicator final : public Widget {
public:
    explicit ResourceIndicator(MainView& mv);
    void update(const WorkspaceSnapshot&);
    void paint(PaintContext&) const override;
    EventResult on_pointer(PointerEvent&) override;
    void on_hover(bool) override;
    std::string describe() const override;
    // The left edges of the segments (DIP, local): fixed slots, the same for every value.
    std::vector<float> segment_x() const;

protected:
    Size intrinsic_size(Size) const override;

private:
    // CPU (with a sparkline), memory, agent context, agent state: always all four, each in a
    // slot as wide as its widest text, so nothing moves when values, units or tabs change.
    struct Segment {
        Icon icon = Icon::none;
        TextRunPtr run;
        float x = 0;        // left edge of the segment
        float slot = 0;     // value slot width (the value is right-aligned in it)
        bool spark = false, dim = false, numeric = true;
    };
    struct Layout {
        std::vector<Segment> segs;
        float width = 0, height = 0, baseline = 0, xheight = 0;
    };
    Layout layout_for(ITextService&, const Theme&, float scale, float height) const;
    size_t shown_segments() const;
    MainView& mv_;
    TabKey tab_ = 0, hist_tab_ = 0;
    std::string cpu_, mem_, ctx_, state_;
    bool agent_ = false, pressed_ = false;
    std::vector<float> cpu_hist_;
    Resources last_{};
};

class TitleBar final : public Widget {
public:
    explicit TitleBar(MainView& mv);
    void update(const WorkspaceSnapshot&);
    void paint(PaintContext&) const override;
    // True at points that drag the window (empty title bar).
    bool is_caption(Vec2 local) const;
    EventResult on_pointer(PointerEvent&) override;

private:
    MainView& mv_;
    Widget* lead_ = nullptr;
    Button* menu_ = nullptr;          // shown only while the sidebar is hidden
    Button* sidebar_btn_ = nullptr;   // shown only while the sidebar is hidden
    Button* search_btn_ = nullptr;    // shown only while the sidebar is hidden
    Widget* title_box_ = nullptr;
    Label* title_ = nullptr;
    Label* path_ = nullptr;
    ResourceIndicator* indicator_ = nullptr;
};

// ---------------------------------------------------------------- status area

struct StatusInfo {
    StorageUsage storage;
    Resources total;                 // every pane of the window
    std::vector<float> cpu_history;  // percent, oldest first
    std::vector<float> mem_history;  // bytes
    std::string renderer;            // "metal", "software" ...
    size_t attention = 0;            // panes that finished or need attention
};

class StatusBar final : public Widget {
public:
    explicit StatusBar(MainView& mv);
    // Drops the least important items as the bar narrows instead of eliding them to "1...".
    void arrange(const Rect& frame) override;
    EventResult on_pointer(PointerEvent&) override;
    void set(const StatusInfo&);
    void update(const WorkspaceSnapshot&);
    void paint(PaintContext&) const override;

private:
    MainView& mv_;
    Widget* storage_row_ = nullptr;
    Label* path_ = nullptr;   // "All sessions: N panes"
    Widget* mem_row_ = nullptr;
    Widget* cpu_row_ = nullptr;
    IconView* storage_icon_ = nullptr;
    Gauge* gauge_ = nullptr;
    Label* storage_ = nullptr;
    Label* mem_ = nullptr;
    Sparkline* mem_spark_ = nullptr;
    Label* cpu_ = nullptr;
    Sparkline* cpu_spark_ = nullptr;
    Badge* attention_ = nullptr;
    Label* renderer_ = nullptr;
};

// ---------------------------------------------------------------- main view

struct MainViewOptions {
    bool mac_titlebar = false;     // reserve the traffic lights' space (full-size content view)
    bool in_window_menu = false;   // a menu button in the title bar (Windows, Linux)
    bool show_status = true;
};

struct MainViewHooks {
    std::function<ITerminalSurface*(PaneKey)> terminal;
    std::function<void(Rect anchor)> app_menu;          // the in-window menu button
    std::function<void()> search_history;               // the magnifier button: the palette on the history
    std::function<void(Vec2 window_pos, int clicks)> caption_press;   // window drag / zoom
    std::function<void(PaneKey)> pane_focused;          // keyboard focus moved to a pane
    // Opens the resource popover for a scope, anchored to a window rect (above: status bar).
    std::function<void(ResourceScope, Rect anchor, bool above)> show_resources;
    // The pane's context menu (full) or its agent items only (the sidebar's tab menu).
    std::function<std::vector<MenuItem>(PaneKey, bool full)> pane_menu_items;
};

class MainView final : public Widget, public ITerminalHost {
public:
    MainView(IWorkspaceView& view, MainViewHooks hooks, MainViewOptions opts = {});
    ~MainView() override;

    IWorkspaceView& view() { return view_; }
    const MainViewHooks& hooks() const { return hooks_; }
    const MainViewOptions& options() const { return opts_; }

    // Re-reads the snapshot (after every model change). Cheap when nothing changed.
    void sync();
    void set_status(const StatusInfo& s);
    void toggle_sidebar();
    bool sidebar_visible() const { return sidebar_on_; }

    // Focus the shown tab's focused pane (after a tab switch, at start).
    void focus_active_pane();
    TerminalWidget* terminal_widget(PaneKey) const;
    PaneKey focused_pane() const;
    // Drop targets: the visible pane under a window point (0 = none), or the sidebar.
    PaneKey pane_at(Vec2 window_pos) const;
    bool over_sidebar(Vec2 window_pos) const;
    bool is_caption(Vec2 window_pos) const;

    // Sidebar resize from the divider next to it.
    void begin_sidebar_resize();
    void sidebar_resize(float width);
    void end_sidebar_resize();

    // Context menus and dialogs (shared by sidebar rows and the title bar).
    void project_menu(ProjectKey, Vec2 at);
    void tab_menu(TabKey, Vec2 at);
    void pane_menu(PaneKey, Vec2 at);
    // ITerminalHost
    ITerminalSurface* terminal(PaneKey k) override { return hooks_.terminal ? hooks_.terminal(k) : nullptr; }
    bool terminal_menu(PaneKey k, Vec2 at) override {
        pane_menu(k, at);
        return true;
    }
    void terminal_focused(PaneKey) override;
    void rename_project_dialog(ProjectKey);
    void rename_tab_dialog(TabKey);
    void confirm_remove_project(ProjectKey);

    void arrange(const Rect&) override;
    void paint_over(PaintContext&) const override;

    // Pane drag (from a pane header): the drop target under a window point (pane 0: none) and
    // the preview of where the pane would land.
    struct PaneDrop {
        PaneKey target = 0;
        SplitDir side = SplitDir::right;
        Rect preview;   // window coordinates
    };
    PaneDrop pane_drop_at(PaneKey dragged, Vec2 window_pos) const;
    void set_pane_drop(const PaneDrop&);

private:
    PaneDrop drop_{};
    IWorkspaceView& view_;
    MainViewHooks hooks_;
    MainViewOptions opts_;
    uint64_t rev_ = ~uint64_t{0};
    Sidebar* sidebar_ = nullptr;
    Widget* sidebar_edge_ = nullptr;
    TitleBar* titlebar_ = nullptr;
    SplitContainer* split_ = nullptr;
    StatusBar* status_ = nullptr;
    Animated sidebar_w_;
    bool sidebar_on_ = true;
    float sidebar_target_ = 220;
    bool resizing_ = false;
    TabKey shown_tab_ = 0;
    PaneKey shown_focus_ = 0;
};

}  // namespace kt::ui
