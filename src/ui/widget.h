// The widget tree, layout styles, display lists and events.
// UI thread only. Geometry in DIPs.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform_gui/input.h"
#include "platform_gui/window.h"
#include "render/draw_list.h"
#include "ui/geometry.h"
#include "ui/icons.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace kt::ui {

class Window;
class Widget;

// ---------------------------------------------------------------- layout

struct LayoutStyle {
    enum class Dir : uint8_t { Row, Column, Stack };
    enum class Align : uint8_t { Start, Center, End, Stretch };
    enum class Justify : uint8_t { Start, Center, End, SpaceBetween };
    enum class Position : uint8_t { Flow, Absolute };

    Dir dir = Dir::Column;
    Align align_items = Align::Stretch;
    std::optional<Align> align_self;
    Justify justify = Justify::Start;
    float gap = 0;
    Insets padding, margin;
    float grow = 0, shrink = 1;
    std::optional<float> basis;
    std::optional<float> width, height;
    float min_w = 0, min_h = 0, max_w = kInf, max_h = kInf;
    Position position = Position::Flow;
    Insets inset;                 // Absolute: distances from the parent's content box edges
    bool inset_r = false, inset_b = false;   // Absolute: anchor to the right / bottom edge
    bool fit_w = false;                      // Absolute, left-anchored: its own width, not the stretch
    bool clip = false;            // clip children to the own bounds
};

struct Constraints {
    float min_w = 0, max_w = kInf, min_h = 0, max_h = kInf;
    bool operator==(const Constraints&) const = default;
};

// ---------------------------------------------------------------- display lists

struct DisplayOp {
    enum class Kind : uint8_t { Rect, RoundRect, Shadow, Text, Icon, PushClip, PopClip, PushOpacity, PopOpacity, External };
    Kind kind = Kind::Rect;
    Rect r;
    float radius = 0, border = 0, sigma = 0, opacity = 1;
    Color color, color2;
    TextRunPtr text;
    Icon icon = Icon::none;
    bool hairline = false;   // Rect: at least one physical pixel thick on its thin side
};

class DisplayList {
public:
    void rect(Rect r, Color c) { push(op(DisplayOp::Kind::Rect, r, c)); }
    // A 1-physical-pixel line along the rect's thin side.
    void hairline(Rect r, Color c) {
        DisplayOp o = op(DisplayOp::Kind::Rect, r, c);
        o.hairline = true;
        push(std::move(o));
    }
    void round_rect(Rect r, float radius, Color fill, float border = 0, Color border_color = {}) {
        DisplayOp o = op(DisplayOp::Kind::RoundRect, r, fill);
        o.radius = radius;
        o.border = border;
        o.color2 = border_color;
        push(std::move(o));
    }
    void shadow(Rect r, float radius, float sigma, Color c) {
        DisplayOp o = op(DisplayOp::Kind::Shadow, r, c);
        o.radius = radius;
        o.sigma = sigma;
        push(std::move(o));
    }
    // `origin` is the left end of the baseline.
    void text(TextRunPtr run, Vec2 origin, Color c) {
        DisplayOp o = op(DisplayOp::Kind::Text, Rect{origin.x, origin.y, 0, 0}, c);
        o.text = std::move(run);
        push(std::move(o));
    }
    void icon(Icon i, float size, Vec2 top_left, Color c) {
        DisplayOp o = op(DisplayOp::Kind::Icon, Rect{top_left.x, top_left.y, size, size}, c);
        o.icon = i;
        push(std::move(o));
    }
    void push_clip(Rect r, float radius = 0) {
        DisplayOp o = op(DisplayOp::Kind::PushClip, r, {});
        o.radius = radius;
        push(std::move(o));
    }
    void pop_clip() { push(op(DisplayOp::Kind::PopClip, {}, {})); }
    void push_opacity(float a) {
        DisplayOp o = op(DisplayOp::Kind::PushOpacity, {}, {});
        o.opacity = a;
        push(std::move(o));
    }
    void pop_opacity() { push(op(DisplayOp::Kind::PopOpacity, {}, {})); }
    void clear() { ops.clear(); }
    bool empty() const { return ops.empty(); }

    std::vector<DisplayOp> ops;

private:
    static DisplayOp op(DisplayOp::Kind k, Rect r, Color c) {
        DisplayOp o;
        o.kind = k;
        o.r = r;
        o.color = c;
        return o;
    }
    void push(DisplayOp o) { ops.push_back(std::move(o)); }
};

struct PaintContext {
    DisplayList& dl;
    const Theme& theme;
    ITextService& text;
    float scale;
    const Window* window;
    TimePoint now;
};

// Replays display lists into a render::DrawList (px), used by Window::frame and by widgets that
// paint themselves (External ops: the terminal view).
struct Compositor {
    render::DrawList& out;
    ITextService& text;
    float scale = 1;
    Vec2 origin;                         // window DIP of the current widget's top-left
    std::vector<render::IRect>* damage;  // px; widgets that paint themselves add their damage
    bool full = false;                   // the frame is full damage
    float opacity = 1;

    float px(float dip) const { return dip * scale; }
    render::IRect snap(Rect window_dip) const;
    void replay(const DisplayList&);
};

// ---------------------------------------------------------------- events

enum class EventResult : uint8_t { Ignored, Handled };

struct PointerEvent {
    enum class Type : uint8_t { Move, Press, Release, Enter, Leave };
    Type type = Type::Move;
    platform::MouseButton button = platform::MouseButton::none;
    uint8_t clicks = 0;
    uint16_t mods = 0;
    Vec2 pos;     // window DIP
    Vec2 local;   // in the receiving widget
    bool captured = false;   // the receiver holds the pointer capture
};

struct WheelEvent {
    float dx = 0, dy = 0;   // dy > 0: toward the top of the content
    bool precise = false;
    uint16_t mods = 0;
    Vec2 pos, local;
};

using KeyEvent = platform::KeyEvent;

enum class FocusChange : uint8_t { Gained, Lost };

struct ImeSpan {
    uint32_t begin = 0, end = 0;
    bool selected = false;
};

class ImeClient {
public:
    virtual ~ImeClient() = default;
    virtual void ime_preedit(std::string_view text, std::span<const ImeSpan> spans, int caret) = 0;
    virtual void ime_commit(std::string_view text) = 0;
    virtual void ime_cancel() = 0;
    virtual Rect ime_caret_rect() const = 0;   // window DIPs
};

// ---------------------------------------------------------------- animation

enum class MotionLevel : uint8_t { Full, Reduced, None };

struct Easing {
    float x1 = 0.42f, y1 = 0, x2 = 0.58f, y2 = 1;   // cubic Bezier
    static constexpr Easing ease_out() { return {0, 0, 0.58f, 1}; }
    static constexpr Easing ease_in() { return {0.42f, 0, 1, 1}; }
    static constexpr Easing ease_in_out() { return {0.42f, 0, 0.58f, 1}; }
    static constexpr Easing standard() { return {0.2f, 0, 0, 1}; }
    float eval(float t) const;
};

struct Transition {
    enum class Kind : uint8_t { Instant, Spring, Curve } kind = Kind::Instant;
    Duration duration{0};   // spring: settling time
    float damping = 1.0f;   // spring ζ
    Easing easing = Easing::ease_in_out();
    bool movement = true;   // Reduced motion lands movement in one frame; short fades stay
    static Transition spring(std::chrono::milliseconds t, float zeta = 1.0f) { return {Kind::Spring, t, zeta, {}, true}; }
    static Transition curve(std::chrono::milliseconds t, Easing e = Easing::ease_in_out(), bool movement = false) {
        return {Kind::Curve, t, 1, e, movement};
    }
};

// A float that moves toward its target over time, owned by a widget. While it moves, the window
// ticks it every frame and invalidates the owner (paint, or layout with `affects_layout`).
class Animated {
public:
    explicit Animated(Widget& owner, float value = 0, bool affects_layout = false)
        : owner_(owner), value_(value), target_(value), layout_(affects_layout) {}
    ~Animated();
    Animated(const Animated&) = delete;
    Animated& operator=(const Animated&) = delete;

    float value() const { return value_; }
    float target() const { return target_; }
    bool running() const { return running_; }
    void set(float target, const Transition& t);
    void jump(float v);
    // Advances to `now`; returns true while still moving.
    bool tick(TimePoint now);
    void finish() { jump(target_); }
    // Called with the new value whenever it changes (to drive opacity, translation, ...).
    void on_value(std::function<void(float)> f) { on_value_ = std::move(f); }

private:
    friend class Window;
    Widget& owner_;
    float value_, target_, from_ = 0, velocity_ = 0, live_velocity_ = 0;
    bool layout_;
    bool running_ = false;
    Transition tr_;
    TimePoint start_{};
    TimePoint last_{};
    std::function<void(float)> on_value_;
    void changed();
};

// ---------------------------------------------------------------- the widget

class Widget {
public:
    explicit Widget(std::string automation_id = {});
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // Tree
    Widget* parent() const { return parent_; }
    std::span<const std::unique_ptr<Widget>> children() const { return children_; }
    Widget* child(size_t i) const { return i < children_.size() ? children_[i].get() : nullptr; }
    size_t child_count() const { return children_.size(); }
    template <class T>
    T& add(std::unique_ptr<T> c, size_t index = npos) {
        T& ref = *c;
        add_child(std::move(c), index);
        return ref;
    }
    template <class T, class... Args>
    T& emplace(Args&&... args) {
        return add(std::make_unique<T>(std::forward<Args>(args)...));
    }
    Widget& add_child(std::unique_ptr<Widget> child, size_t index = npos);
    std::unique_ptr<Widget> take_child(Widget& child);
    void clear_children();
    void move_child(size_t from, size_t to);
    size_t index_in_parent() const;
    Window* window() const { return window_; }
    static constexpr size_t npos = static_cast<size_t>(-1);

    // Identity
    const std::string& automation_id() const { return automation_id_; }
    void set_automation_id(std::string id) { automation_id_ = std::move(id); }
    std::string automation_path() const;   // "main/sidebar/project[api]"
    WidgetId id() const { return id_; }

    // Layout
    LayoutStyle& style() {
        invalidate_layout();
        return style_;
    }
    const LayoutStyle& style() const { return style_; }
    const LayoutStyle& cstyle() const { return style_; }
    virtual Size measure(const Constraints& c);
    virtual void arrange(const Rect& frame);
    const Rect& frame() const { return frame_; }
    Rect local_bounds() const { return {0, 0, frame_.w, frame_.h}; }
    Rect bounds_in_window() const;
    Vec2 to_local(Vec2 window_pos) const;
    uint32_t measure_calls() const { return measure_calls_; }

    // Paint: own content in local coordinates (paint), children, then over them (paint_over).
    virtual void paint(PaintContext&) const {}
    virtual void paint_over(PaintContext&) const {}
    // Widgets that draw straight into the render list (terminal view); called between paint and
    // the children. Returns true when the widget did so.
    virtual bool paint_external(Compositor&) { return false; }
    virtual Insets paint_outset() const { return {}; }   // shadows outside the frame
    void invalidate_paint();
    void invalidate_layout();
    bool paint_dirty() const { return paint_dirty_; }

    // State
    void set_visible(bool);
    bool visible() const { return visible_; }
    void set_enabled(bool);
    bool enabled() const;
    void set_opacity(float);
    float opacity() const { return opacity_; }
    void set_translation(Vec2);
    Vec2 translation() const { return translation_; }
    void set_hit_testable(bool v) { hit_testable_ = v; }
    bool hit_testable() const { return hit_testable_; }
    bool hovered() const { return hovered_; }
    bool focused() const;
    bool focus_visible() const;   // focused after keyboard navigation (focus ring rule)
    void set_tooltip(std::string t) { tooltip_ = std::move(t); }
    const std::string& tooltip() const { return tooltip_; }

    // Events. Return Handled to stop bubbling.
    virtual EventResult on_pointer(PointerEvent&) { return EventResult::Ignored; }
    virtual EventResult on_wheel(WheelEvent&) { return EventResult::Ignored; }
    virtual EventResult on_key(const KeyEvent&) { return EventResult::Ignored; }
    virtual EventResult on_text(std::string_view) { return EventResult::Ignored; }
    virtual void on_hover(bool inside);
    virtual void on_focus(FocusChange) { invalidate_paint(); }
    virtual bool accepts_focus() const { return false; }
    virtual ImeClient* ime_client() { return nullptr; }
    virtual platform::CursorShape cursor_at(Vec2) const { return platform::CursorShape::arrow; }
    // True when the point (local) hits this widget itself; children are tested first.
    virtual bool hit_self(Vec2 local) const { return local_bounds().contains(local); }
    virtual void on_attached() {}
    virtual void on_detached() {}

    // Tests and scripts: a short state description ("selected", "checked", ...).
    virtual std::string describe() const { return {}; }

protected:
    virtual Size intrinsic_size(Size available) const;   // leaves: content size without padding
    void request_animation_frame();
    // For widgets with their own arrange(): sets the frame without laying out the children.
    void assign_frame(const Rect& f);
    bool layout_dirty() const { return layout_dirty_; }
    float snap(float v) const;

private:
    friend class Window;
    friend class Animated;
    void attach(Window*);
    void detach_window();
    Size flex_measure(const Constraints& inner);
    void flex_arrange(const Rect& content);

    Widget* parent_ = nullptr;
    Window* window_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    WidgetId id_;
    std::string automation_id_;
    std::string tooltip_;
    LayoutStyle style_;
    Rect frame_;
    bool visible_ = true, enabled_ = true, hit_testable_ = true, hovered_ = false;
    float opacity_ = 1;
    Vec2 translation_;
    bool paint_dirty_ = true, layout_dirty_ = true;
    bool has_measure_ = false;
    Constraints last_c_;
    Size last_size_;
    uint32_t measure_calls_ = 0;
    // Compositor state.
    DisplayList cache_, cache_over_;
    Rect painted_;            // window DIP incl. outset, as last painted
    float painted_opacity_ = 1;
    bool painted_visible_ = false;
};

// Thin containers.
class Box : public Widget {
public:
    explicit Box(std::string id = {}, LayoutStyle::Dir dir = LayoutStyle::Dir::Column) : Widget(std::move(id)) {
        style().dir = dir;
    }
    // Background fill and optional rounded border.
    void set_background(std::optional<Color> c, float radius = 0) {
        bg_ = c;
        radius_ = radius;
        invalidate_paint();
    }
    void set_border(std::optional<Color> c, float width = 1) {
        border_ = c;
        border_w_ = width;
        invalidate_paint();
    }
    void paint(PaintContext&) const override;

private:
    std::optional<Color> bg_, border_;
    float radius_ = 0, border_w_ = 1;
};

std::unique_ptr<Box> row(std::string id = {}, float gap = 0);
std::unique_ptr<Box> column(std::string id = {}, float gap = 0);
std::unique_ptr<Box> stack(std::string id = {});
std::unique_ptr<Widget> spacer(float grow = 1);

// Keyed reconciliation of a container's children: keeps children whose key survives
// (in the new order), creates the missing ones with `make` and removes the rest. `key_of`
// reads a child's key.
void reconcile(Widget& parent, std::span<const uint64_t> keys, const std::function<uint64_t(const Widget&)>& key_of,
               const std::function<std::unique_ptr<Widget>(uint64_t)>& make,
               const std::function<void(Widget&, uint64_t)>& update);

// UI strings: one place to translate later. English only for now.
inline std::string_view tr(std::string_view s) { return s; }

}  // namespace kt::ui
