// A UI window: the widget tree of one platform window, its overlays, focus,
// hover, pointer capture, timers, animations, layout, damage and the compositor.
//
// The window does not own the platform window or the renderer. The application feeds it the
// platform events (on_mouse, on_key, ...), calls frame() when it asks for one, and submits the
// returned draw list with the returned damage. Headless tests drive it the same way with a fake
// clock (set_clock) and synthetic events.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/widget.h"

namespace kt::ui {

struct OverlayOptions {
    enum class Placement : uint8_t { Below, Above, Right, Left, Center, TopCenter, At };
    Rect anchor;                    // window DIP; Placement::At uses anchor.x/y as the top-left
    Placement placement = Placement::Below;
    bool modal = false;             // scrim, blocks input below, traps focus
    bool dismiss_on_outside_click = true;
    bool dismiss_on_escape = true;
    bool transient = true;          // closed by close_transient (menus, tooltips)
    bool pass_through = false;      // never hit-tested (tooltips, toasts)
    bool take_focus = true;
    bool own_keys = false;          // navigation keys (Tab, Escape ...) go to the focused widget
    float margin = 8;               // to the window edges
    std::function<void()> on_close;
};

struct FrameResult {
    std::vector<render::IRect> damage;   // px, merged
    bool full = false;
    bool drew = false;                   // false: nothing changed, nothing to submit
};

// Merges damage rectangles: snapped to an 8 px grid, at most 8 rectangles, full when
// more than 60 % of the surface.
std::vector<render::IRect> merge_damage(std::vector<render::IRect> rects, render::PxSize surface, bool& full);

class Window {
public:
    explicit Window(ITextService& text, Theme theme = Theme::dark());
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Callbacks to the host (platform window). All optional.
    std::function<void()> request_frame;
    std::function<void(Rect)> set_ime_rect;
    std::function<void(platform::CursorShape)> set_cursor;
    // The keymap: runs after IME and modal pre-dispatch; true consumes the key.
    std::function<bool(const KeyEvent&)> keymap;
    // A timer deadline changed: the host arms a platform timer and calls run_timers().
    std::function<void(TimePoint)> timer_changed;
    // Clipboard (text fields).
    std::function<std::string()> get_clipboard;
    std::function<void(const std::string&)> set_clipboard;

    Widget& root() { return *root_; }
    void set_root(std::unique_ptr<Widget>);
    void resize(Size dip, float scale);
    Size size() const { return size_; }
    float scale() const { return scale_; }
    render::PxSize px_size() const;
    const Theme& theme() const { return theme_; }
    void set_theme(Theme);
    ITextService& text() { return text_; }
    void set_motion(MotionLevel m) { motion_ = m; }
    MotionLevel motion() const { return motion_; }
    bool window_focused() const { return window_focused_; }

    // Time. The default clock is steady_clock; tests install a fake one.
    void set_clock(std::function<TimePoint()> c) { clock_ = std::move(c); }
    TimePoint now() const { return clock_(); }

    // Platform events.
    void on_mouse(const platform::MouseEvent&);
    void on_scroll(const platform::ScrollEvent&);
    void on_key(const KeyEvent&);
    void on_text(std::string_view committed);
    void on_composition(const platform::Composition&);
    void on_focus(bool focused);
    // Synthetic pointer helpers (scripts, tests).
    void pointer_move(Vec2 p, uint16_t mods = 0);
    void pointer_down(Vec2 p, platform::MouseButton b = platform::MouseButton::left, uint8_t clicks = 1, uint16_t mods = 0);
    void pointer_up(Vec2 p, platform::MouseButton b = platform::MouseButton::left, uint16_t mods = 0);
    Vec2 pointer() const { return pointer_; }

    // Frames.
    bool needs_frame() const;
    void invalidate_all();
    void add_damage(Rect window_dip);
    // Runs timers and animations up to now, lays out and paints into `out`.
    FrameResult frame(render::DrawList& out);
    void layout();   // layout only (tests)
    uint64_t frames() const { return frames_; }

    // Overlays (popovers, menus, dialogs, toasts, tooltips).
    WidgetId show_overlay(std::unique_ptr<Widget>, OverlayOptions);
    void close_overlay(WidgetId);
    void close_transient();
    bool has_modal() const;
    Widget* overlay(WidgetId) const;
    size_t overlay_count() const { return overlays_.size(); }

    // Focus.
    void set_focus(Widget*, bool by_keyboard = false);
    Widget* focused() const { return focus_; }
    bool focus_visible() const { return focus_visible_; }
    void focus_next(bool backward);

    // Timers (UI thread). Delay from now().
    uint64_t add_timer(Duration delay, std::function<void()> fn);
    void cancel_timer(uint64_t id);
    void run_timers();
    std::optional<TimePoint> next_deadline() const;

    // Lookup for tests and scripts: "sidebar/project[api]/tab[2]" matches automation ids along
    // the path (segments may skip intermediate widgets). Overlays are searched first.
    Widget* find(std::string_view path) const;
    std::vector<Widget*> find_all(std::string_view path) const;
    Widget* hit_test(Vec2 window_pos) const;
    Widget* captured() const { return capture_; }

    // Internal (widgets).
    void forget(Widget*);
    void register_anim(Animated*);
    void unregister_anim(Animated*);
    void request_animation_frame(Widget*);
    void layout_changed() { layout_dirty_ = true; ping(); }
    void ping();
    void finish_animations();
    size_t active_animations() const { return anims_.size(); }

private:
    struct OverlayEntry {
        WidgetId id;
        Widget* w;        // child of overlay_root_
        OverlayOptions o;
        Widget* prev_focus;
        // The content's own size style: placement overwrites width/height, re-measures use these.
        std::optional<float> own_w = std::nullopt, own_h = std::nullopt;
        bool own_saved = false;
    };
    void place_overlays();
    void update_hover();
    void dispatch_pointer(PointerEvent&, Widget* target);
    void show_tooltip_for(Widget*);
    void hide_tooltip();
    void paint_tree(Widget& w, Compositor& c, PaintContext& pc, Vec2 origin, float opacity, bool parent_dirty);
    Widget* hit_in(Widget& w, Vec2 local) const;
    void key_to_focus(const KeyEvent&);

    ITextService& text_;
    Theme theme_;
    std::unique_ptr<Widget> root_;          // holds the base root and the overlay root
    Widget* base_ = nullptr;
    Widget* overlay_root_ = nullptr;
    std::vector<OverlayEntry> overlays_;
    Size size_{800, 500};
    float scale_ = 1;
    MotionLevel motion_ = MotionLevel::Full;
    std::function<TimePoint()> clock_;

    Widget* focus_ = nullptr;
    bool focus_visible_ = false;
    bool window_focused_ = true;
    Widget* capture_ = nullptr;
    std::vector<Widget*> hover_path_;
    Vec2 pointer_{-1, -1};
    bool pointer_inside_ = false;
    platform::CursorShape cursor_ = platform::CursorShape::arrow;
    bool composing_ = false;

    struct Timer {
        TimePoint at;
        std::function<void()> fn;
    };
    std::map<uint64_t, Timer> timers_;
    uint64_t next_timer_ = 1;

    std::vector<Animated*> anims_;
    std::vector<Widget*> frame_requests_;
    std::vector<Rect> damage_;
    bool full_damage_ = true;
    bool layout_dirty_ = true;
    uint64_t frames_ = 0;

    // Tooltip state.
    Widget* tip_owner_ = nullptr;
    uint64_t tip_timer_ = 0;
    WidgetId tip_overlay_ = 0;
    TimePoint tip_hidden_at_{};
};

}  // namespace kt::ui
