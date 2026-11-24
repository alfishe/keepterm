#include "ui/widget.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

#include "ui/window.h"

namespace kt::ui {

namespace {

std::atomic<WidgetId> g_next_id{1};

bool flow(const Widget& w) { return w.visible() && w.cstyle().position == LayoutStyle::Position::Flow; }

}  // namespace

// ---------------------------------------------------------------- easing

float Easing::eval(float t) const {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= 0 || t >= 1) return t;
    auto bez = [](float a, float b, float s) {   // 1D cubic Bezier with endpoints 0 and 1
        const float u = 1 - s;
        return 3 * u * u * s * a + 3 * u * s * s * b + s * s * s;
    };
    float lo = 0, hi = 1, s = t;
    for (int i = 0; i < 24; ++i) {   // bisection on x(s) = t
        s = (lo + hi) / 2;
        const float x = bez(x1, x2, s);
        if (std::abs(x - t) < 1e-5f) break;
        if (x < t) lo = s;
        else hi = s;
    }
    return bez(y1, y2, s);
}

// ---------------------------------------------------------------- Animated

Animated::~Animated() {
    if (running_ && owner_.window()) owner_.window()->unregister_anim(this);
}

void Animated::jump(float v) {
    const bool was = running_;
    running_ = false;
    velocity_ = 0;
    const bool differs = v != value_;
    value_ = target_ = v;
    if (was && owner_.window()) owner_.window()->unregister_anim(this);
    if (differs || was) changed();
}

void Animated::changed() {
    if (layout_) owner_.invalidate_layout();
    else owner_.invalidate_paint();
    if (on_value_) on_value_(value_);
}

void Animated::set(float target, const Transition& t) {
    Window* w = owner_.window();
    if (target == target_ && (running_ || value_ == target)) return;
    bool instant = !w || t.kind == Transition::Kind::Instant || t.duration.count() <= 0 || w->motion() == MotionLevel::None;
    if (w && w->motion() == MotionLevel::Reduced && (t.movement || t.duration > std::chrono::milliseconds(150))) instant = true;
    if (instant) {
        jump(target);
        return;
    }
    const TimePoint now = w->now();
    if (running_ && tr_.kind == Transition::Kind::Spring && t.kind == Transition::Kind::Spring) {
        tick(now);   // keeps the current value and velocity: interrupted motion stays continuous
        velocity_ = live_velocity_;
    } else {
        velocity_ = 0;
    }
    from_ = value_;
    target_ = target;
    tr_ = t;
    start_ = last_ = now;
    if (!running_) {
        running_ = true;
        w->register_anim(this);
    }
}

bool Animated::tick(TimePoint now) {
    if (!running_) return false;
    const float t = std::chrono::duration<float>(now - start_).count();
    const float d = std::max(1e-3f, std::chrono::duration<float>(tr_.duration).count());
    bool done = false;
    if (tr_.kind == Transition::Kind::Curve) {
        const float p = std::min(1.0f, t / d);
        value_ = from_ + (target_ - from_) * tr_.easing.eval(p);
        done = p >= 1;
    } else {
        const float zeta = std::clamp(tr_.damping, 0.05f, 1.0f);
        const float omega = 9.2f / (zeta * d);
        if (zeta >= 0.95f) {
            // Critically damped, closed form from (from_, velocity at start).
            const float x0 = from_ - target_;
            const float v0 = tr_.kind == Transition::Kind::Spring ? velocity_ : 0;
            const float e = std::exp(-omega * t);
            value_ = target_ + (x0 + (v0 + omega * x0) * t) * e;
            live_velocity_ = e * (v0 - omega * (v0 + omega * x0) * t);
            done = t >= d;
        } else {
            // Under-damped: semi-implicit Euler at 1/240 s.
            constexpr float h = 1.0f / 240.0f;
            float steps = std::chrono::duration<float>(now - last_).count() / h;
            float x = value_, v = velocity_;
            for (int i = 0; i < static_cast<int>(steps) && i < 2400; ++i) {
                v += (-omega * omega * (x - target_) - 2 * zeta * omega * v) * h;
                x += v * h;
            }
            value_ = x;
            velocity_ = live_velocity_ = v;
            done = t >= d * 1.5f;
        }
    }
    last_ = now;
    if (done) {
        value_ = target_;
        velocity_ = live_velocity_ = 0;
    }
    changed();
    if (done) running_ = false;
    return !done;
}

// ---------------------------------------------------------------- Widget

Widget::Widget(std::string automation_id) : id_(g_next_id.fetch_add(1)), automation_id_(std::move(automation_id)) {}

Widget::~Widget() {
    // Children first, so that each forgets itself while the window pointer is still valid.
    for (auto& c : children_) c.reset();
    children_.clear();
    if (window_) {
        if (painted_visible_) window_->add_damage(painted_);
        window_->forget(this);
    }
}

void Widget::attach(Window* w) {
    window_ = w;
    painted_visible_ = false;
    paint_dirty_ = true;
    for (auto& c : children_) c->attach(w);
    if (w) on_attached();
}

void Widget::detach_window() {
    if (!window_) return;
    for (auto& c : children_) c->detach_window();
    on_detached();
    if (painted_visible_) window_->add_damage(painted_);
    window_->forget(this);
    window_ = nullptr;
    painted_visible_ = false;
}

Widget& Widget::add_child(std::unique_ptr<Widget> child, size_t index) {
    Widget& ref = *child;
    child->parent_ = this;
    if (index >= children_.size()) children_.push_back(std::move(child));
    else children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
    if (window_) ref.attach(window_);
    invalidate_layout();
    return ref;
}

std::unique_ptr<Widget> Widget::take_child(Widget& child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == &child) {
            std::unique_ptr<Widget> out = std::move(*it);
            children_.erase(it);
            out->detach_window();
            out->parent_ = nullptr;
            invalidate_layout();
            return out;
        }
    }
    return nullptr;
}

void Widget::clear_children() {
    while (!children_.empty()) (void)take_child(*children_.back());
}

void Widget::move_child(size_t from, size_t to) {
    if (from >= children_.size() || to >= children_.size() || from == to) return;
    auto c = std::move(children_[from]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(from));
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(to), std::move(c));
    invalidate_layout();
}

size_t Widget::index_in_parent() const {
    if (!parent_) return npos;
    for (size_t i = 0; i < parent_->children_.size(); ++i) {
        if (parent_->children_[i].get() == this) return i;
    }
    return npos;
}

std::string Widget::automation_path() const {
    std::string out;
    for (const Widget* w = this; w; w = w->parent_) {
        if (w->automation_id_.empty()) continue;
        out = out.empty() ? w->automation_id_ : w->automation_id_ + "/" + out;
    }
    return out;
}

Rect Widget::bounds_in_window() const {
    Vec2 o{frame_.x + translation_.x, frame_.y + translation_.y};
    for (const Widget* p = parent_; p; p = p->parent_) {
        o.x += p->frame_.x + p->translation_.x;
        o.y += p->frame_.y + p->translation_.y;
    }
    return {o.x, o.y, frame_.w, frame_.h};
}

Vec2 Widget::to_local(Vec2 p) const {
    const Rect b = bounds_in_window();
    return {p.x - b.x, p.y - b.y};
}

void Widget::invalidate_paint() {
    paint_dirty_ = true;
    if (window_) window_->ping();
}

void Widget::invalidate_layout() {
    for (Widget* w = this; w; w = w->parent_) {
        w->has_measure_ = false;
        w->layout_dirty_ = true;
    }
    paint_dirty_ = true;
    if (window_) window_->layout_changed();
}

void Widget::set_visible(bool v) {
    if (v == visible_) return;
    visible_ = v;
    if (parent_) parent_->invalidate_layout();
    invalidate_layout();
}

void Widget::set_enabled(bool v) {
    if (v == enabled_) return;
    enabled_ = v;
    invalidate_paint();
    for (auto& c : children_) c->invalidate_paint();
}

bool Widget::enabled() const { return enabled_ && (!parent_ || parent_->enabled()); }

void Widget::set_opacity(float o) {
    o = std::clamp(o, 0.0f, 1.0f);
    if (o == opacity_) return;
    opacity_ = o;
    if (window_) window_->ping();
}

void Widget::set_translation(Vec2 t) {
    if (t == translation_) return;
    translation_ = t;
    if (window_) window_->ping();
}

bool Widget::focused() const { return window_ && window_->focused() == this; }
bool Widget::focus_visible() const { return focused() && window_->focus_visible(); }

void Widget::on_hover(bool inside) {
    (void)inside;
    invalidate_paint();
}

void Widget::request_animation_frame() {
    if (window_) window_->request_animation_frame(this);
}

Size Widget::intrinsic_size(Size) const { return {}; }

void Widget::assign_frame(const Rect& f) {
    frame_ = f;
    layout_dirty_ = false;
    paint_dirty_ = true;
}

float Widget::snap(float v) const {
    const float s = window_ ? window_->scale() : 1.0f;
    return std::round(v * s) / s;
}

// ---------------------------------------------------------------- layout

Size Widget::measure(const Constraints& c) {
    if (has_measure_ && last_c_ == c) return last_size_;
    ++measure_calls_;
    const LayoutStyle& s = style_;
    const float max_w = std::min(c.max_w, s.max_w), max_h = std::min(c.max_h, s.max_h);
    const float avail_w = std::max(0.0f, (s.width ? *s.width : max_w) - s.padding.hsum());
    const float avail_h = std::max(0.0f, (s.height ? *s.height : max_h) - s.padding.vsum());
    bool any_flow = false;
    for (const auto& ch : children_) any_flow |= flow(*ch);
    const Size content = any_flow ? flex_measure(Constraints{0, avail_w, 0, avail_h}) : intrinsic_size({avail_w, avail_h});
    float w = s.width ? *s.width : content.w + s.padding.hsum();
    float h = s.height ? *s.height : content.h + s.padding.vsum();
    const float lo_w = std::max(s.min_w, c.min_w), lo_h = std::max(s.min_h, c.min_h);
    w = std::max(lo_w, std::min(w, max_w));
    h = std::max(lo_h, std::min(h, max_h));
    last_c_ = c;
    last_size_ = {w, h};
    has_measure_ = true;
    return last_size_;
}

Size Widget::flex_measure(const Constraints& inner) {
    const LayoutStyle& s = style_;
    const bool row = s.dir == LayoutStyle::Dir::Row, stack = s.dir == LayoutStyle::Dir::Stack;
    float main = 0, cross = 0;
    int n = 0;
    for (const auto& chp : children_) {
        Widget& ch = *chp;
        if (!flow(ch)) continue;
        const Insets& m = ch.style_.margin;
        Constraints cc;
        if (stack) cc = {0, std::max(0.0f, inner.max_w - m.hsum()), 0, std::max(0.0f, inner.max_h - m.vsum())};
        else if (row) cc = {0, kInf, 0, std::max(0.0f, inner.max_h - m.vsum())};
        else cc = {0, std::max(0.0f, inner.max_w - m.hsum()), 0, kInf};
        Size cs = ch.measure(cc);
        if (ch.style_.basis) (row ? cs.w : cs.h) = *ch.style_.basis;
        if (stack) {
            main = std::max(main, cs.w + m.hsum());
            cross = std::max(cross, cs.h + m.vsum());
        } else if (row) {
            main += cs.w + m.hsum();
            cross = std::max(cross, cs.h + m.vsum());
        } else {
            main += cs.h + m.vsum();
            cross = std::max(cross, cs.w + m.hsum());
        }
        ++n;
    }
    if (stack) return {main, cross};
    if (n > 1) main += s.gap * static_cast<float>(n - 1);
    return row ? Size{main, cross} : Size{cross, main};
}

void Widget::arrange(const Rect& f) {
    if (f == frame_ && !layout_dirty_) return;
    frame_ = f;
    layout_dirty_ = false;
    paint_dirty_ = true;
    flex_arrange(local_bounds().inset(style_.padding));
}

void Widget::flex_arrange(const Rect& content) {
    const LayoutStyle& s = style_;
    const bool row = s.dir == LayoutStyle::Dir::Row, stack = s.dir == LayoutStyle::Dir::Stack;
    auto place = [&](Widget& ch, float x, float y, float w, float h) {
        const float x0 = snap(x), y0 = snap(y);
        ch.arrange(Rect{x0, y0, std::max(0.0f, snap(x + w) - x0), std::max(0.0f, snap(y + h) - y0)});
    };

    struct Item {
        Widget* w;
        float size, lo, hi, grow, shrink, hyp;
        bool frozen;
    };
    std::vector<Item> items;
    if (!stack) {
        const float content_main = row ? content.w : content.h;
        const float content_cross = row ? content.h : content.w;
        float used = 0;
        for (const auto& chp : children_) {
            Widget& ch = *chp;
            if (!flow(ch)) continue;
            const LayoutStyle& cs = ch.style_;
            const Insets& m = cs.margin;
            const float cross_avail = std::max(0.0f, content_cross - (row ? m.vsum() : m.hsum()));
            const LayoutStyle::Align al = cs.align_self.value_or(s.align_items);
            const std::optional<float> fixed_cross = row ? cs.height : cs.width;
            const float min_cross = (al == LayoutStyle::Align::Stretch && !fixed_cross) ? cross_avail : 0;
            Constraints cc = row ? Constraints{0, kInf, min_cross, cross_avail} : Constraints{min_cross, cross_avail, 0, kInf};
            float hyp;
            const std::optional<float> fixed_main = row ? cs.width : cs.height;
            if (fixed_main) hyp = *fixed_main;
            else if (cs.basis) hyp = *cs.basis;
            else {
                const Size m2 = ch.measure(cc);
                hyp = row ? m2.w : m2.h;
            }
            float lo = row ? cs.min_w : cs.min_h, hi = row ? cs.max_w : cs.max_h;
            if (fixed_main) lo = hi = *fixed_main;
            hyp = std::clamp(hyp, lo, std::max(lo, hi));
            items.push_back(Item{&ch, hyp, lo, std::max(lo, hi), cs.grow, cs.shrink, hyp, false});
            used += hyp + (row ? m.hsum() : m.vsum());
        }
        const float gaps = items.size() > 1 ? s.gap * static_cast<float>(items.size() - 1) : 0;
        float free = content_main - used - gaps;
        // Distribute, clamping and repeating for the unclamped items (at most n rounds).
        for (size_t round = 0; round <= items.size() && std::abs(free) > 0.01f; ++round) {
            float weight = 0;
            for (const Item& it : items) {
                if (it.frozen) continue;
                weight += free > 0 ? it.grow : it.shrink * it.hyp;
            }
            if (weight <= 0) break;
            float spent = 0;
            bool clamped = false;
            for (Item& it : items) {
                if (it.frozen) continue;
                const float wgt = free > 0 ? it.grow : it.shrink * it.hyp;
                if (wgt <= 0) continue;
                const float want = it.size + free * wgt / weight;
                const float got = std::clamp(want, it.lo, it.hi);
                if (got != want) {
                    it.frozen = true;
                    clamped = true;
                }
                spent += got - it.size;
                it.size = got;
            }
            free -= spent;
            if (!clamped) break;
        }
        float total = gaps;
        for (const Item& it : items) total += it.size + (row ? it.w->style_.margin.hsum() : it.w->style_.margin.vsum());
        const float rest = std::max(0.0f, content_main - total);
        float cursor = row ? content.x : content.y;
        float between = s.gap;
        switch (s.justify) {
            case LayoutStyle::Justify::Start: break;
            case LayoutStyle::Justify::Center: cursor += rest / 2; break;
            case LayoutStyle::Justify::End: cursor += rest; break;
            case LayoutStyle::Justify::SpaceBetween:
                if (items.size() > 1) between += rest / static_cast<float>(items.size() - 1);
                break;
        }
        for (const Item& it : items) {
            Widget& ch = *it.w;
            const LayoutStyle& cs = ch.style_;
            const Insets& m = cs.margin;
            const LayoutStyle::Align al = cs.align_self.value_or(s.align_items);
            const float cross_start = (row ? content.y + m.t : content.x + m.l);
            const float cross_avail = std::max(0.0f, content_cross - (row ? m.vsum() : m.hsum()));
            const std::optional<float> fixed_cross = row ? cs.height : cs.width;
            float csize;
            if (fixed_cross) csize = *fixed_cross;
            else if (al == LayoutStyle::Align::Stretch) csize = cross_avail;
            else {
                const Size m2 = row ? ch.measure(Constraints{it.size, it.size, 0, cross_avail})
                                    : ch.measure(Constraints{0, cross_avail, it.size, it.size});
                csize = std::min(row ? m2.h : m2.w, cross_avail);
            }
            float cpos = cross_start;
            if (al == LayoutStyle::Align::Center) cpos += (cross_avail - csize) / 2;
            else if (al == LayoutStyle::Align::End) cpos += cross_avail - csize;
            cursor += row ? m.l : m.t;
            if (row) place(ch, cursor, cpos, it.size, csize);
            else place(ch, cpos, cursor, csize, it.size);
            cursor += it.size + (row ? m.r : m.b) + between;
        }
    } else {
        for (const auto& chp : children_) {
            Widget& ch = *chp;
            if (!flow(ch)) continue;
            const LayoutStyle& cs = ch.style_;
            const Rect area = content.inset(cs.margin);
            const LayoutStyle::Align al = cs.align_self.value_or(s.align_items);
            if (al == LayoutStyle::Align::Stretch) {
                place(ch, area.x, area.y, cs.width.value_or(area.w), cs.height.value_or(area.h));
                continue;
            }
            const Size m2 = ch.measure(Constraints{0, area.w, 0, area.h});
            float x = area.x, y = area.y;
            if (al == LayoutStyle::Align::Center) {
                x += (area.w - m2.w) / 2;
                y += (area.h - m2.h) / 2;
            } else if (al == LayoutStyle::Align::End) {
                x += area.w - m2.w;
                y += area.h - m2.h;
            }
            place(ch, x, y, m2.w, m2.h);
        }
    }
    // Absolute children against the content box.
    for (const auto& chp : children_) {
        Widget& ch = *chp;
        if (!ch.visible_ || ch.style_.position != LayoutStyle::Position::Absolute) continue;
        const LayoutStyle& cs = ch.style_;
        const Insets& in = cs.inset;
        Size m2{};
        if (!cs.width || !cs.height) m2 = ch.measure(Constraints{0, std::max(0.0f, content.w - in.hsum()), 0, std::max(0.0f, content.h - in.vsum())});
        float w = cs.width ? *cs.width : (cs.inset_r || cs.fit_w ? m2.w : std::max(0.0f, content.w - in.hsum()));
        float h = cs.height ? *cs.height : (cs.inset_b ? m2.h : std::max(0.0f, content.h - in.vsum()));
        const float x = cs.inset_r ? content.right() - in.r - w : content.x + in.l;
        const float y = cs.inset_b ? content.bottom() - in.b - h : content.y + in.t;
        place(ch, x, y, w, h);
    }
}

// ---------------------------------------------------------------- Box and helpers

void Box::paint(PaintContext& p) const {
    const Rect b = local_bounds();
    if (bg_ || border_) {
        if (radius_ > 0 || border_) {
            p.dl.round_rect(b, radius_, bg_.value_or(Color{}), border_ ? border_w_ : 0, border_.value_or(Color{}));
        } else {
            p.dl.rect(b, *bg_);
        }
    }
}

std::unique_ptr<Box> row(std::string id, float gap) {
    auto b = std::make_unique<Box>(std::move(id), LayoutStyle::Dir::Row);
    b->style().gap = gap;
    b->style().align_items = LayoutStyle::Align::Center;
    return b;
}

std::unique_ptr<Box> column(std::string id, float gap) {
    auto b = std::make_unique<Box>(std::move(id), LayoutStyle::Dir::Column);
    b->style().gap = gap;
    return b;
}

std::unique_ptr<Box> stack(std::string id) { return std::make_unique<Box>(std::move(id), LayoutStyle::Dir::Stack); }

std::unique_ptr<Widget> spacer(float grow) {
    auto w = std::make_unique<Widget>();
    w->style().grow = grow;
    w->set_hit_testable(false);
    return w;
}

void reconcile(Widget& parent, std::span<const uint64_t> keys, const std::function<uint64_t(const Widget&)>& key_of,
               const std::function<std::unique_ptr<Widget>(uint64_t)>& make,
               const std::function<void(Widget&, uint64_t)>& update) {
    // Fast path: same keys in the same order.
    bool same = parent.child_count() == keys.size();
    for (size_t i = 0; same && i < keys.size(); ++i) same = key_of(*parent.child(i)) == keys[i];
    if (same) {
        for (size_t i = 0; i < keys.size(); ++i) update(*parent.child(i), keys[i]);
        return;
    }
    std::unordered_map<uint64_t, std::unique_ptr<Widget>> old;
    while (parent.child_count() > 0) {
        Widget* c = parent.child(parent.child_count() - 1);
        const uint64_t k = key_of(*c);
        auto taken = parent.take_child(*c);
        old.emplace(k, std::move(taken));
    }
    for (uint64_t k : keys) {
        auto it = old.find(k);
        std::unique_ptr<Widget> w;
        if (it != old.end()) {
            w = std::move(it->second);
            old.erase(it);
        } else {
            w = make(k);
        }
        Widget& ref = parent.add_child(std::move(w));
        update(ref, k);
    }
}

// ---------------------------------------------------------------- Compositor

render::IRect Compositor::snap(Rect r) const {
    const int32_t x0 = static_cast<int32_t>(std::lround(r.x * scale)), y0 = static_cast<int32_t>(std::lround(r.y * scale));
    const int32_t x1 = static_cast<int32_t>(std::lround(r.right() * scale)), y1 = static_cast<int32_t>(std::lround(r.bottom() * scale));
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

void Compositor::replay(const DisplayList& dl) {
    for (const DisplayOp& op : dl.ops) {
        const Rect r = op.r.translated(origin);
        switch (op.kind) {
            case DisplayOp::Kind::Rect: {
                if (op.color.a <= 0) break;
                render::IRect p = snap(r);
                if (op.hairline) {
                    if (r.w <= r.h) {
                        p.x = static_cast<int32_t>(std::floor(r.x * scale));
                        p.w = 1;
                    } else {
                        p.y = static_cast<int32_t>(std::floor(r.y * scale));
                        p.h = 1;
                    }
                }
                if (p.empty()) break;
                out.quad(render::solid_quad(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.w),
                                            static_cast<float>(p.h), to_render_color(op.color, opacity)));
                break;
            }
            case DisplayOp::Kind::RoundRect: {
                const render::IRect p = snap(r);
                if (p.empty()) break;
                render::Quad q{};
                q.x = static_cast<float>(p.x);
                q.y = static_cast<float>(p.y);
                q.w = static_cast<float>(p.w);
                q.h = static_cast<float>(p.h);
                q.kind = static_cast<uint8_t>(render::QuadKind::rounded);
                q.color = to_render_color(op.color, opacity);
                q.color2 = to_render_color(op.color2, opacity);
                q.radius = op.radius * scale;
                q.param = op.border > 0 ? std::max(1.0f, std::round(op.border * scale)) : 0;
                if (op.color.a <= 0) q.flags |= render::kQuadBorderOnly;
                if (op.color.a <= 0 && (op.border <= 0 || op.color2.a <= 0)) break;
                out.quad(q);
                break;
            }
            case DisplayOp::Kind::Shadow: {
                render::Quad q{};
                q.x = r.x * scale;
                q.y = r.y * scale;
                q.w = r.w * scale;
                q.h = r.h * scale;
                q.kind = static_cast<uint8_t>(render::QuadKind::shadow);
                q.color = to_render_color(op.color, opacity);
                q.radius = op.radius * scale;
                q.param = op.sigma * scale;
                out.quad(q);
                break;
            }
            case DisplayOp::Kind::Text:
                if (op.text) {
                    text.draw_text(*op.text, std::round(r.x * scale), std::round(r.y * scale), to_render_color(op.color, opacity), out);
                }
                break;
            case DisplayOp::Kind::Icon: {
                const int px = std::max(1, static_cast<int>(std::lround(op.r.w * scale)));
                text.draw_icon(op.icon, px, std::round(r.x * scale), std::round(r.y * scale), to_render_color(op.color, opacity), out);
                break;
            }
            case DisplayOp::Kind::PushClip: out.push_clip(snap(r), op.radius * scale); break;
            case DisplayOp::Kind::PopClip: out.pop_clip(); break;
            case DisplayOp::Kind::PushOpacity: out.push_layer(render::kNoScissor, op.opacity); break;
            case DisplayOp::Kind::PopOpacity: out.pop_layer(); break;
            case DisplayOp::Kind::External: break;
        }
    }
}

}  // namespace kt::ui
