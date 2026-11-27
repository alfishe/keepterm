#include "ui/window.h"

#include <algorithm>
#include <cmath>

#include "ui/widgets.h"

namespace kt::ui {

namespace {
// Keeps a path in Window::guarded_ while handlers run along it.
struct PathGuard {
    std::vector<std::vector<Widget*>*>& list;
    std::vector<Widget*>* path;
    PathGuard(std::vector<std::vector<Widget*>*>& l, std::vector<Widget*>* p) : list(l), path(p) { list.push_back(p); }
    ~PathGuard() { list.erase(std::remove(list.begin(), list.end(), path), list.end()); }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
};
}  // namespace

namespace {

// The top layer of the window: base content and the overlay layer, both full size.
class RootStack final : public Widget {
public:
    RootStack() : Widget("") { style().dir = LayoutStyle::Dir::Stack; }
};

class BaseLayer final : public Widget {
public:
    BaseLayer() : Widget("") { style().dir = LayoutStyle::Dir::Stack; }
    void paint(PaintContext& p) const override { p.dl.rect(local_bounds(), p.theme.bg); }
};

class OverlayLayer final : public Widget {
public:
    OverlayLayer() : Widget("") {
        style().dir = LayoutStyle::Dir::Stack;
        set_hit_testable(true);
    }
    bool hit_self(Vec2) const override { return false; }   // only its overlays take input
};

// Backdrop of a modal overlay: scrim color, swallows input outside the content.
class Scrim final : public Widget {
public:
    Scrim() : Widget("scrim") { style().dir = LayoutStyle::Dir::Stack; }
    void paint(PaintContext& p) const override { p.dl.rect(local_bounds(), p.theme.scrim); }
    EventResult on_pointer(PointerEvent&) override { return EventResult::Handled; }
    EventResult on_wheel(WheelEvent&) override { return EventResult::Handled; }
};

// "tab[2]" matches exactly; "tab" also matches every "tab[...]".
bool segment_matches(const Widget& w, std::string_view seg) {
    const std::string& id = w.automation_id();
    if (id.empty()) return false;
    if (id == seg) return true;
    return seg.find('[') == std::string_view::npos && id.size() > seg.size() && id.compare(0, seg.size(), seg) == 0 &&
           id[seg.size()] == '[';
}

void collect(const Widget& w, std::vector<std::string_view>& segs, size_t at, std::vector<Widget*>& out, bool first_only) {
    if (first_only && !out.empty()) return;
    size_t next = at;
    if (segment_matches(w, segs[at])) next = at + 1;
    if (next == segs.size()) {
        out.push_back(const_cast<Widget*>(&w));
        return;
    }
    for (const auto& c : w.children()) {
        if (!c->visible()) continue;
        collect(*c, segs, next, out, first_only);
        if (first_only && !out.empty()) return;
    }
}

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> segs;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        // A '/' inside brackets belongs to the key ("project[~/src]").
        size_t br = path.find('[', i);
        if (br != std::string_view::npos && (j == std::string_view::npos || br < j)) {
            const size_t close = path.find(']', br);
            if (close != std::string_view::npos) j = path.find('/', close);
        }
        if (j == std::string_view::npos) j = path.size();
        if (j > i) segs.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    return segs;
}

}  // namespace

// ---------------------------------------------------------------- damage merge

std::vector<render::IRect> merge_damage(std::vector<render::IRect> rects, render::PxSize surface, bool& full) {
    std::vector<render::IRect> out;
    const render::IRect bounds{0, 0, surface.w, surface.h};
    for (render::IRect r : rects) {
        r = render::intersect(r, bounds);
        if (r.empty()) continue;
        // Outward to the 8 px grid.
        const int32_t x0 = (r.x / 8) * 8, y0 = (r.y / 8) * 8;
        const int32_t x1 = std::min(surface.w, ((r.right() + 7) / 8) * 8), y1 = std::min(surface.h, ((r.bottom() + 7) / 8) * 8);
        out.push_back(render::IRect{x0, y0, x1 - x0, y1 - y0});
    }
    auto area = [](const render::IRect& r) { return static_cast<int64_t>(r.w) * r.h; };
    // Merge pairs whose union is less than 1.3 x their summed area.
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < out.size() && !merged; ++i) {
            for (size_t j = i + 1; j < out.size(); ++j) {
                const render::IRect u = render::unite(out[i], out[j]);
                if (static_cast<double>(area(u)) < 1.3 * static_cast<double>(area(out[i]) + area(out[j]))) {
                    out[i] = u;
                    out.erase(out.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }
    while (out.size() > 8) {   // merge the pair with the smallest growth
        size_t bi = 0, bj = 1;
        int64_t best = INT64_MAX;
        for (size_t i = 0; i < out.size(); ++i) {
            for (size_t j = i + 1; j < out.size(); ++j) {
                const int64_t g = area(render::unite(out[i], out[j])) - area(out[i]) - area(out[j]);
                if (g < best) {
                    best = g;
                    bi = i;
                    bj = j;
                }
            }
        }
        out[bi] = render::unite(out[bi], out[bj]);
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(bj));
    }
    int64_t total = 0;
    for (const auto& r : out) total += area(r);
    if (static_cast<double>(total) > 0.6 * static_cast<double>(area(bounds))) full = true;
    if (full) out.assign(1, bounds);
    return out;
}

// ---------------------------------------------------------------- Window

Window::Window(ITextService& text, Theme theme) : text_(text), theme_(std::move(theme)), clock_([] { return Clock::now(); }) {
    root_ = std::make_unique<RootStack>();
    root_->attach(this);
    base_ = &root_->add_child(std::make_unique<BaseLayer>());
    overlay_root_ = &root_->add_child(std::make_unique<OverlayLayer>());
}

Window::~Window() {
    overlays_.clear();
    focus_ = nullptr;
    capture_ = nullptr;
    hover_path_.clear();
    tip_owner_ = nullptr;
    anims_.clear();
    root_.reset();
}

void Window::set_root(std::unique_ptr<Widget> w) {
    base_->clear_children();
    if (w) base_->add_child(std::move(w));
    invalidate_all();
}

void Window::resize(Size dip, float scale) {
    scale = scale > 0 ? scale : 1;
    if (dip == size_ && scale == scale_) return;
    const bool rescaled = scale != scale_;
    size_ = dip;
    scale_ = scale;
    if (rescaled) text_metrics_changed();   // a new display, the window's zoom
    else root_->invalidate_layout();
    invalidate_all();
}

void Window::text_metrics_changed() {
    std::function<void(Widget&)> all = [&](Widget& w) {
        w.on_text_metrics_changed();
        for (const auto& c : w.children()) all(*c);
    };
    all(*root_);
}

render::PxSize Window::px_size() const {
    return {static_cast<int32_t>(std::lround(size_.w * scale_)), static_cast<int32_t>(std::lround(size_.h * scale_))};
}

void Window::set_theme(Theme t) {
    theme_ = std::move(t);
    text_metrics_changed();   // the fonts and sizes of the roles
    invalidate_all();
}

void Window::invalidate_all() {
    full_damage_ = true;
    ping();
}

void Window::add_damage(Rect r) {
    if (!r.empty()) damage_.push_back(r);
    ping();
}

void Window::ping() {
    if (request_frame) request_frame();
}

bool Window::needs_frame() const {
    return full_damage_ || !damage_.empty() || !anims_.empty() || !frame_requests_.empty() || layout_dirty_ ||
           root_->layout_dirty_ || root_->paint_dirty_;
}

void Window::register_anim(Animated* a) {
    if (std::find(anims_.begin(), anims_.end(), a) == anims_.end()) anims_.push_back(a);
    ping();
}

void Window::unregister_anim(Animated* a) { anims_.erase(std::remove(anims_.begin(), anims_.end(), a), anims_.end()); }

void Window::finish_animations() {
    auto copy = anims_;
    for (Animated* a : copy) {
        if (std::find(anims_.begin(), anims_.end(), a) != anims_.end()) a->finish();
    }
    anims_.clear();
}

void Window::request_animation_frame(Widget* w) {
    if (std::find(frame_requests_.begin(), frame_requests_.end(), w) == frame_requests_.end()) frame_requests_.push_back(w);
    ping();
}

void Window::forget(Widget* w) {
    if (focus_ == w) focus_ = nullptr;
    if (capture_ == w) capture_ = nullptr;
    if (pressed_ == w) pressed_ = nullptr;
    for (auto* path : guarded_) std::replace(path->begin(), path->end(), w, static_cast<Widget*>(nullptr));
    if (tip_owner_ == w) {
        tip_owner_ = nullptr;
        if (tip_timer_) cancel_timer(tip_timer_);
        tip_timer_ = 0;
    }
    hover_path_.erase(std::remove(hover_path_.begin(), hover_path_.end(), w), hover_path_.end());
    frame_requests_.erase(std::remove(frame_requests_.begin(), frame_requests_.end(), w), frame_requests_.end());
    for (auto& e : overlays_) {
        if (e.prev_focus == w) e.prev_focus = nullptr;
    }
    overlays_.erase(std::remove_if(overlays_.begin(), overlays_.end(), [&](const OverlayEntry& e) { return e.w == w; }),
                    overlays_.end());
    // A widget that leaves the window (detached, or being destroyed) takes its animations along:
    // they stop at their targets, silently (the owner may be half destroyed).
    anims_.erase(std::remove_if(anims_.begin(), anims_.end(),
                                [&](Animated* a) {
                                    if (&a->owner_ != w) return false;
                                    a->value_ = a->target_;
                                    a->running_ = false;
                                    return true;
                                }),
                 anims_.end());
}

// ---------------------------------------------------------------- timers

uint64_t Window::add_timer(Duration delay, std::function<void()> fn) {
    const uint64_t id = next_timer_++;
    timers_[id] = Timer{now() + delay, std::move(fn)};
    if (timer_changed) timer_changed(*next_deadline());
    return id;
}

void Window::cancel_timer(uint64_t id) { timers_.erase(id); }

void Window::run_timers() {
    for (int guard = 0; guard < 1000; ++guard) {
        const TimePoint t = now();
        auto due = timers_.end();
        for (auto it = timers_.begin(); it != timers_.end(); ++it) {
            if (it->second.at <= t && (due == timers_.end() || it->second.at < due->second.at)) due = it;
        }
        if (due == timers_.end()) break;
        auto fn = std::move(due->second.fn);
        timers_.erase(due);
        if (fn) fn();
    }
    if (timer_changed) {
        if (auto d = next_deadline()) timer_changed(*d);
    }
}

std::optional<TimePoint> Window::next_deadline() const {
    std::optional<TimePoint> best;
    for (const auto& [id, t] : timers_) {
        if (!best || t.at < *best) best = t.at;
    }
    return best;
}

// ---------------------------------------------------------------- overlays

WidgetId Window::show_overlay(std::unique_ptr<Widget> content, OverlayOptions o) {
    std::unique_ptr<Widget> entry;
    if (o.modal) {
        auto s = std::make_unique<Scrim>();
        content->style_.position = LayoutStyle::Position::Absolute;
        s->add_child(std::move(content));
        entry = std::move(s);
    } else {
        entry = std::move(content);
    }
    entry->style_.position = LayoutStyle::Position::Absolute;
    if (o.pass_through) entry->set_hit_testable(false);
    Widget& w = overlay_root_->add_child(std::move(entry));
    const WidgetId id = w.id();
    Widget* prev = focus_;
    const bool take = o.take_focus && !o.pass_through;
    overlays_.push_back(OverlayEntry{id, &w, std::move(o), prev});
    if (take) {
        std::function<Widget*(Widget&)> first = [&](Widget& x) -> Widget* {
            if (!x.visible()) return nullptr;
            if (x.accepts_focus() && x.enabled()) return &x;
            for (const auto& c : x.children()) {
                if (Widget* f = first(*c)) return f;
            }
            return nullptr;
        };
        if (Widget* f = first(w)) set_focus(f, false);
    }
    layout_changed();
    return id;
}

Widget* Window::overlay(WidgetId id) const {
    for (const auto& e : overlays_) {
        if (e.id == id) return e.o.modal && e.w->child_count() ? e.w->child(0) : e.w;
    }
    return nullptr;
}

void Window::close_overlay(WidgetId id) {
    for (size_t i = 0; i < overlays_.size(); ++i) {
        if (overlays_[i].id != id) continue;
        OverlayEntry e = std::move(overlays_[i]);
        overlays_.erase(overlays_.begin() + static_cast<std::ptrdiff_t>(i));
        // Focus goes back to where it was before the overlay took it.
        bool focus_inside = false;
        for (Widget* f = focus_; f; f = f->parent()) focus_inside |= f == e.w;
        auto keep = overlay_root_->take_child(*e.w);
        if (focus_inside || !focus_) {
            focus_ = nullptr;
            if (e.prev_focus) set_focus(e.prev_focus, false);
        }
        if (e.o.on_close) e.o.on_close();
        layout_changed();
        update_hover();
        return;
    }
}

void Window::close_transient() {
    std::vector<WidgetId> ids;
    for (const auto& e : overlays_) {
        if (e.o.transient) ids.push_back(e.id);
    }
    for (WidgetId id : ids) close_overlay(id);
}

bool Window::has_modal() const {
    return std::any_of(overlays_.begin(), overlays_.end(), [](const OverlayEntry& e) { return e.o.modal; });
}

void Window::place_overlays() {
    for (auto& e : overlays_) {
        Widget* content = e.o.modal && e.w->child_count() ? e.w->child(0) : e.w;
        const float m = e.o.margin;
        const Size avail{std::max(0.0f, size_.w - 2 * m), std::max(0.0f, size_.h - 2 * m)};
        if (!e.own_saved) {
            e.own_w = content->style_.width;
            e.own_h = content->style_.height;
            e.own_saved = true;
        }
        content->style_.width = e.own_w;
        content->style_.height = e.own_h;
        content->has_measure_ = false;
        const Size cs = content->measure(Constraints{0, avail.w, 0, avail.h});
        const Rect a = e.o.anchor;
        float x = a.x, y = a.y;
        using P = OverlayOptions::Placement;
        switch (e.o.placement) {
            case P::Below:
                y = a.bottom() + 4;
                if (y + cs.h > size_.h - m && a.y - 4 - cs.h >= m) y = a.y - 4 - cs.h;   // flip
                break;
            case P::Above:
                y = a.y - 4 - cs.h;
                if (y < m && a.bottom() + 4 + cs.h <= size_.h - m) y = a.bottom() + 4;
                break;
            case P::Right:
                x = a.right() + 4;
                if (x + cs.w > size_.w - m && a.x - 4 - cs.w >= m) x = a.x - 4 - cs.w;
                break;
            case P::Left:
                x = a.x - 4 - cs.w;
                if (x < m) x = a.right() + 4;
                break;
            case P::Center:
                x = (size_.w - cs.w) / 2;
                y = (size_.h - cs.h) / 2;
                break;
            case P::TopCenter: x = (size_.w - cs.w) / 2; break;
            case P::At:
                if (x + cs.w > size_.w - m) x = std::max(m, x - cs.w);
                if (y + cs.h > size_.h - m) y = std::max(m, y - cs.h);
                break;
        }
        // Shift inside the window.
        x = std::clamp(x, m, std::max(m, size_.w - m - cs.w));
        y = std::clamp(y, m, std::max(m, size_.h - m - cs.h));
        LayoutStyle& s = content->style_;
        s.position = LayoutStyle::Position::Absolute;
        s.inset = Insets{x, y, 0, 0};
        s.inset_r = s.inset_b = false;
        s.width = cs.w;
        s.height = cs.h;
        content->has_measure_ = false;
        content->layout_dirty_ = true;
        if (e.o.modal) {
            LayoutStyle& ws = e.w->style_;
            ws.inset = Insets{};
            ws.width = size_.w;
            ws.height = size_.h;
            e.w->layout_dirty_ = true;
        }
    }
    overlay_root_->layout_dirty_ = true;
}

// ---------------------------------------------------------------- layout and paint

void Window::layout() {
    place_overlays();
    root_->has_measure_ = false;
    root_->layout_dirty_ = true;
    root_->arrange(Rect{0, 0, size_.w, size_.h});
    layout_dirty_ = false;
    if (focus_ && set_ime_rect) {
        if (ImeClient* c = focus_->ime_client()) set_ime_rect(c->ime_caret_rect());
    }
}

void Window::paint_tree(Widget& w, Compositor& c, PaintContext& pc, Vec2 origin, float opacity, bool parent_hidden) {
    const Rect wr0{origin.x + w.frame_.x + w.translation_.x, origin.y + w.frame_.y + w.translation_.y, w.frame_.w, w.frame_.h};
    const Insets out = w.paint_outset();
    const Rect wr{wr0.x - out.l, wr0.y - out.t, wr0.w + out.hsum(), wr0.h + out.vsum()};
    const float op = opacity * w.opacity_;
    const bool shown = w.visible_ && !parent_hidden && op > 0.002f;
    if (!shown) {
        if (w.painted_visible_) add_damage(w.painted_);
        w.painted_visible_ = false;
        for (const auto& ch : w.children_) paint_tree(*ch, c, pc, {wr0.x, wr0.y}, op, true);
        return;
    }
    if (w.paint_dirty_) {
        w.cache_.clear();
        w.cache_over_.clear();
        PaintContext p1{w.cache_, theme_, text_, scale_, this, pc.now};
        w.paint(p1);
        PaintContext p2{w.cache_over_, theme_, text_, scale_, this, pc.now};
        w.paint_over(p2);
        if (w.painted_visible_) add_damage(w.painted_);
        add_damage(wr);
        w.paint_dirty_ = false;
    } else if (!w.painted_visible_ || !(wr == w.painted_) || op != w.painted_opacity_) {
        if (w.painted_visible_) add_damage(w.painted_);
        add_damage(wr);
    }
    w.painted_ = wr;
    w.painted_opacity_ = op;
    w.painted_visible_ = true;

    const bool layer = w.opacity_ < 0.999f;
    if (layer) c.out.push_layer(c.snap(wr), w.opacity_);
    c.origin = {wr0.x, wr0.y};
    c.replay(w.cache_);
    c.origin = {wr0.x, wr0.y};
    w.paint_external(c);
    const bool clip = w.style_.clip;
    if (clip) c.out.push_clip(c.snap(wr0));
    for (const auto& ch : w.children_) paint_tree(*ch, c, pc, {wr0.x, wr0.y}, op, false);
    if (clip) c.out.pop_clip();
    c.origin = {wr0.x, wr0.y};
    c.replay(w.cache_over_);
    if (layer) c.out.pop_layer();
}

FrameResult Window::frame(render::DrawList& out) {
    FrameResult res;
    run_timers();
    const TimePoint t = now();
    {
        // A tick may destroy widgets (an on_value callback, a finished fade): skip animations
        // that left the list meanwhile, their memory may be gone.
        auto copy = anims_;
        for (Animated* a : copy) {
            if (std::find(anims_.begin(), anims_.end(), a) == anims_.end()) continue;
            if (!a->tick(t)) unregister_anim(a);
        }
    }
    for (Widget* w : frame_requests_) w->invalidate_paint();
    frame_requests_.clear();
    const bool relayout = layout_dirty_ || root_->layout_dirty_;
    if (relayout) {
        layout();
        update_hover();
    }
    out.clear();
    std::vector<render::IRect> px_damage;
    Compositor c{out, text_, scale_, {}, &px_damage, full_damage_, 1};
    DisplayList scratch;
    PaintContext pc{scratch, theme_, text_, scale_, this, t};
    text_.begin_frame();
    paint_tree(*root_, c, pc, {0, 0}, 1, false);
    text_.end_frame();
    for (const Rect& r : damage_) {
        const Rect g = r.inflate(1);   // antialiased edges
        const int32_t x0 = static_cast<int32_t>(std::floor(g.x * scale_)), y0 = static_cast<int32_t>(std::floor(g.y * scale_));
        const int32_t x1 = static_cast<int32_t>(std::ceil(g.right() * scale_)), y1 = static_cast<int32_t>(std::ceil(g.bottom() * scale_));
        px_damage.push_back(render::IRect{x0, y0, x1 - x0, y1 - y0});
    }
    bool full = full_damage_;
    res.damage = merge_damage(std::move(px_damage), px_size(), full);
    res.full = full;
    res.drew = full || !res.damage.empty();
    damage_.clear();
    full_damage_ = false;
    ++frames_;
    if (!anims_.empty() || !frame_requests_.empty()) ping();
    return res;
}

// ---------------------------------------------------------------- hit testing and hover

Widget* Window::hit_in(Widget& w, Vec2 local) const {
    if (!w.visible_ || !w.hit_testable_) return nullptr;
    const bool inside = w.local_bounds().contains(local);
    if (w.style_.clip && !inside) return nullptr;
    for (size_t i = w.children_.size(); i-- > 0;) {
        Widget& ch = *w.children_[i];
        const Vec2 cl{local.x - ch.frame_.x - ch.translation_.x, local.y - ch.frame_.y - ch.translation_.y};
        if (Widget* h = hit_in(ch, cl)) return h;
    }
    if (inside && w.hit_self(local)) return &w;
    return nullptr;
}

Widget* Window::hit_test(Vec2 p) const { return hit_in(*root_, p); }

void Window::update_hover() {
    std::vector<Widget*> path;
    if (pointer_inside_ && !capture_) {
        for (Widget* w = hit_test(pointer_); w; w = w->parent()) path.push_back(w);
    } else if (capture_) {
        for (Widget* w = capture_; w; w = w->parent()) path.push_back(w);
    }
    // on_hover may remove widgets: both paths are guarded while it runs.
    std::vector<Widget*> old = hover_path_;
    {
        PathGuard g1(guarded_, &old), g2(guarded_, &path);
        for (Widget*& w : old) {
            if (w && std::find(path.begin(), path.end(), w) == path.end()) {
                w->hovered_ = false;
                w->on_hover(false);
            }
        }
        for (Widget*& w : path) {
            if (w && std::find(old.begin(), old.end(), w) == old.end()) {
                w->hovered_ = true;
                w->on_hover(true);
            }
        }
    }
    path.erase(std::remove(path.begin(), path.end(), nullptr), path.end());
    const bool changed = path != hover_path_;
    hover_path_ = std::move(path);
    // Cursor of the deepest widget that names one.
    platform::CursorShape cur = platform::CursorShape::arrow;
    for (Widget* w : hover_path_) {
        const platform::CursorShape c = w->cursor_at(w->to_local(pointer_));
        if (c != platform::CursorShape::arrow) {
            cur = c;
            break;
        }
    }
    if (cur != cursor_) {
        cursor_ = cur;
        if (set_cursor) set_cursor(cur);
    }
    if (changed) {
        Widget* tipped = nullptr;
        for (Widget* w : hover_path_) {
            if (!w->tooltip().empty()) {
                tipped = w;
                break;
            }
        }
        if (tipped != tip_owner_) show_tooltip_for(tipped);
    }
}

void Window::hide_tooltip() {
    if (tip_timer_) cancel_timer(tip_timer_);
    tip_timer_ = 0;
    if (tip_overlay_) {
        const WidgetId id = tip_overlay_;
        tip_overlay_ = 0;
        close_overlay(id);
        tip_hidden_at_ = now();
    }
}

void Window::show_tooltip_for(Widget* w) {
    hide_tooltip();
    tip_owner_ = w;
    if (!w) return;
    // 600 ms of rest; 100 ms when moving between tooltipped items within 1 s.
    const bool quick = tip_hidden_at_ != TimePoint{} && now() - tip_hidden_at_ < std::chrono::seconds(1);
    tip_timer_ = add_timer(quick ? std::chrono::milliseconds(100) : std::chrono::milliseconds(600), [this] {
        tip_timer_ = 0;
        if (!tip_owner_) return;
        OverlayOptions o;
        o.anchor = tip_owner_->bounds_in_window();
        o.placement = OverlayOptions::Placement::Below;
        o.pass_through = true;
        o.take_focus = false;
        tip_overlay_ = show_overlay(std::make_unique<Tooltip>(tip_owner_->tooltip()), o);
    });
}

// ---------------------------------------------------------------- pointer


template <typename F> bool Window::bubble(Widget* start, F fn) {
    std::vector<Widget*> path;
    for (Widget* w = start; w; w = w->parent()) path.push_back(w);
    PathGuard g(guarded_, &path);
    for (std::size_t i = 0; i < path.size(); ++i) {
        Widget* w = path[i];
        if (!w || !w->enabled()) continue;
        if (fn(w, path, i)) return true;
    }
    return false;
}

void Window::dispatch_pointer(PointerEvent& e, Widget* target) {
    (void)bubble(target, [&](Widget* w, const std::vector<Widget*>& path, std::size_t i) {
        e.local = w->to_local(e.pos);
        e.captured = w == capture_;
        if (w->on_pointer(e) == EventResult::Handled) {
            // Not a widget the press removed.
            if (e.type == PointerEvent::Type::Press && !capture_ && path[i]) capture_ = w;
            return true;
        }
        return e.captured;   // capture does not bubble
    });
}

void Window::on_mouse(const platform::MouseEvent& m) {
    const Vec2 p{m.pos.x, m.pos.y};
    PointerEvent e;
    e.button = m.button;
    e.clicks = m.click_count;
    e.mods = m.mods;
    e.pos = p;
    switch (m.type) {
        case platform::MouseEvent::Type::enter:
        case platform::MouseEvent::Type::move: {
            pointer_ = p;
            pointer_inside_ = true;
            e.type = PointerEvent::Type::Move;
            if (capture_) {
                dispatch_pointer(e, capture_);
            } else {
                update_hover();
                if (!hover_path_.empty()) dispatch_pointer(e, hover_path_.front());
            }
            return;
        }
        case platform::MouseEvent::Type::leave:
            pointer_inside_ = false;
            if (!capture_) {
                update_hover();
                show_tooltip_for(nullptr);
            }
            return;
        case platform::MouseEvent::Type::press: {
            pointer_ = p;
            pointer_inside_ = true;
            show_tooltip_for(nullptr);
            // Outside clicks close dismissable overlays; menus and popovers consume the press.
            if (!overlays_.empty()) {
                Widget* hit = hit_test(p);
                const OverlayEntry& top = overlays_.back();
                bool inside = false;
                for (Widget* w = hit; w; w = w->parent()) inside |= w == top.w;
                if (!inside && top.o.dismiss_on_outside_click && !top.o.pass_through) {
                    const bool consume = !top.o.modal;
                    close_overlay(top.id);
                    if (consume) return;
                }
            }
            e.type = PointerEvent::Type::Press;
            Widget* target = hit_test(p);
            // Focus goes to the first focusable widget on the path.
            for (Widget* w = target; w; w = w->parent()) {
                if (w->accepts_focus() && w->enabled()) {
                    set_focus(w, false);
                    break;
                }
            }
            capture_ = nullptr;
            pressed_ = target;
            dispatch_pointer(e, target);
            if (!capture_) capture_ = pressed_;   // null when the press removed it
            pressed_ = nullptr;
            update_hover();
            return;
        }
        case platform::MouseEvent::Type::release: {
            pointer_ = p;
            e.type = PointerEvent::Type::Release;
            Widget* target = capture_ ? capture_ : hit_test(p);
            dispatch_pointer(e, target);
            capture_ = nullptr;
            update_hover();
            return;
        }
    }
}

void Window::pointer_move(Vec2 p, uint16_t mods) {
    platform::MouseEvent m;
    m.type = platform::MouseEvent::Type::move;
    m.pos = {p.x, p.y};
    m.mods = mods;
    on_mouse(m);
}

void Window::pointer_down(Vec2 p, platform::MouseButton b, uint8_t clicks, uint16_t mods) {
    platform::MouseEvent m;
    m.type = platform::MouseEvent::Type::press;
    m.button = b;
    m.click_count = clicks;
    m.pos = {p.x, p.y};
    m.mods = mods;
    on_mouse(m);
}

void Window::pointer_up(Vec2 p, platform::MouseButton b, uint16_t mods) {
    platform::MouseEvent m;
    m.type = platform::MouseEvent::Type::release;
    m.button = b;
    m.pos = {p.x, p.y};
    m.mods = mods;
    on_mouse(m);
}

void Window::on_scroll(const platform::ScrollEvent& s) {
    WheelEvent e;
    e.dx = s.dx;
    e.dy = s.dy;
    e.precise = s.precise;
    e.mods = s.mods;
    e.pos = {s.pos.x, s.pos.y};
    show_tooltip_for(nullptr);
    (void)bubble(hit_test(e.pos), [&](Widget* w, const std::vector<Widget*>&, std::size_t) {
        e.local = w->to_local(e.pos);
        return w->on_wheel(e) == EventResult::Handled;
    });
}

// ---------------------------------------------------------------- keyboard, text, IME, focus

void Window::set_focus(Widget* w, bool by_keyboard) {
    if (w == focus_) {
        if (focus_visible_ != by_keyboard && w) w->invalidate_paint();
        focus_visible_ = by_keyboard;
        return;
    }
    Widget* old = focus_;
    focus_ = w;
    focus_visible_ = by_keyboard;
    if (old) old->on_focus(FocusChange::Lost);
    if (w) w->on_focus(FocusChange::Gained);
    if (w && set_ime_rect) {
        if (ImeClient* c = w->ime_client()) set_ime_rect(c->ime_caret_rect());
    }
}

void Window::focus_next(bool backward) {
    Widget* scope = base_;
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
        if (it->o.modal || it->o.take_focus) {
            scope = it->w;
            break;
        }
    }
    std::vector<Widget*> order;
    std::function<void(Widget&)> walk = [&](Widget& w) {
        if (!w.visible() || !w.enabled()) return;
        if (w.accepts_focus()) order.push_back(&w);
        for (const auto& c : w.children()) walk(*c);
    };
    walk(*scope);
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), focus_);
    size_t i = 0;
    if (it == order.end()) i = backward ? order.size() - 1 : 0;
    else {
        const size_t cur = static_cast<size_t>(it - order.begin());
        i = backward ? (cur + order.size() - 1) % order.size() : (cur + 1) % order.size();
    }
    set_focus(order[i], true);
}

void Window::key_to_focus(const KeyEvent& k) {
    (void)bubble(focus_, [&](Widget* w, const std::vector<Widget*>&, std::size_t) { return w->on_key(k) == EventResult::Handled; });
}

void Window::on_key(const KeyEvent& k) {
    if (k.composing || composing_) return;   // 1. the IME owns the key
    if (k.action != platform::KeyAction::release) show_tooltip_for(nullptr);
    using K = platform::Key;
    const bool nav = k.key == K::escape || k.key == K::up || k.key == K::down || k.key == K::page_up ||
                     k.key == K::page_down || k.key == K::home || k.key == K::end || k.key == K::enter || k.key == K::tab;
    // 2. Overlay pre-dispatch.
    if (!overlays_.empty() && k.action != platform::KeyAction::release) {
        const OverlayEntry& top = overlays_.back();
        if (top.o.own_keys && nav) {
            key_to_focus(k);
            return;
        }
        if (k.key == K::escape && top.o.dismiss_on_escape && !top.o.pass_through) {
            close_overlay(top.id);
            return;
        }
        if (nav && (top.o.modal || top.o.take_focus) && !top.o.pass_through) {
            if (k.key == K::tab) focus_next((k.mods & platform::mod_shift) != 0);
            else key_to_focus(k);
            return;
        }
    }
    // 3. The keymap.
    if (keymap && !has_modal() && keymap(k)) return;
    // 4. The focused widget, bubbling.
    if (k.key == K::tab && k.action != platform::KeyAction::release && (k.mods & (platform::mod_ctrl | platform::mod_super)) == 0) {
        // Tab goes to disabled widgets too: they may still take it.
        std::vector<Widget*> path;
        for (Widget* w = focus_; w; w = w->parent()) path.push_back(w);
        PathGuard g(guarded_, &path);
        bool handled = false;
        for (std::size_t i = 0; i < path.size() && !handled; ++i) handled = path[i] && path[i]->on_key(k) == EventResult::Handled;
        if (!handled) focus_next((k.mods & platform::mod_shift) != 0);
        return;
    }
    key_to_focus(k);
}

void Window::on_text(std::string_view text) {
    if (text.empty()) return;
    if (composing_) {
        composing_ = false;
        if (focus_) {
            if (ImeClient* c = focus_->ime_client()) {
                c->ime_commit(text);
                return;
            }
        }
    }
    (void)bubble(focus_, [&](Widget* w, const std::vector<Widget*>&, std::size_t) { return w->on_text(text) == EventResult::Handled; });
}

void Window::on_composition(const platform::Composition& c) {
    ImeClient* client = focus_ ? focus_->ime_client() : nullptr;
    if (c.text.empty()) {
        if (composing_ && client) client->ime_cancel();
        composing_ = false;
        return;
    }
    composing_ = true;
    if (!client) return;
    std::vector<ImeSpan> spans;
    for (const auto& s : c.segments) spans.push_back(ImeSpan{s.begin, s.end, s.selected});
    client->ime_preedit(std::string_view(reinterpret_cast<const char*>(c.text.data()), c.text.size()), spans,
                        static_cast<int>(c.caret));
    if (set_ime_rect) set_ime_rect(client->ime_caret_rect());
}

void Window::on_focus(bool f) {
    window_focused_ = f;
    if (!f) {
        close_transient();
        show_tooltip_for(nullptr);
    }
    std::function<void(Widget&)> all = [&](Widget& w) {
        w.invalidate_paint();
        for (const auto& c : w.children()) all(*c);
    };
    all(*root_);
}

// ---------------------------------------------------------------- lookup

Widget* Window::find(std::string_view path) const {
    auto segs = split_path(path);
    if (segs.empty()) return nullptr;
    std::vector<Widget*> out;
    collect(*overlay_root_, segs, 0, out, true);
    if (out.empty()) collect(*base_, segs, 0, out, true);
    return out.empty() ? nullptr : out.front();
}

std::vector<Widget*> Window::find_all(std::string_view path) const {
    auto segs = split_path(path);
    std::vector<Widget*> out;
    if (segs.empty()) return out;
    collect(*overlay_root_, segs, 0, out, false);
    collect(*base_, segs, 0, out, false);
    return out;
}

}  // namespace kt::ui
