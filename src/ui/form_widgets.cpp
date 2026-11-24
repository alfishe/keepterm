#include "ui/form_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kt::ui {

namespace {

float px_round(float v, float scale) { return std::round(v * scale) / scale; }

Transition hover_tr(bool in) {
    return Transition::curve(std::chrono::milliseconds(in ? 90 : 160), Easing::ease_out(), false);
}

int decimals_of(double step) {
    int d = 0;
    while (d < 4 && std::fabs(step * std::pow(10.0, d) - std::round(step * std::pow(10.0, d))) > 1e-6) ++d;
    return d;
}

std::string fixed(double v, int decimals) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

bool pressed(const KeyEvent& k) { return k.action != platform::KeyAction::release; }

// Text vertically centered in `r`, starting at x.
void text_in(PaintContext& p, const TextRunPtr& run, float x, const Rect& r, Color c) {
    if (!run) return;
    p.dl.text(run, {x, px_round(r.y + (r.h - run->height()) / 2 + run->ascent(), p.scale)}, c);
}

TextRunPtr shape_cached(const Window* w, const std::string& s, TextStyle st, TextRunPtr& cache, std::string& key) {
    if (!w) return nullptr;
    if (!cache || key != s) {
        cache = const_cast<Window*>(w)->text().shape(s, role_for(w->theme(), st), w->scale());
        key = s;
    }
    return cache;
}

}  // namespace

// ---------------------------------------------------------------- Slider

Slider::Slider(double lo, double hi, double step, double value, std::string id)
    : Widget(std::move(id)), lo_(lo), hi_(std::max(lo, hi)), step_(step), value_(std::clamp(value, lo, std::max(lo, hi))) {}

void Slider::set_value(double v) {
    v = std::clamp(v, lo_, hi_);
    if (v == value_) return;
    value_ = v;
    invalidate_paint();
}

void Slider::set_range(double lo, double hi, double step) {
    lo_ = lo;
    hi_ = std::max(lo, hi);
    step_ = step;
    value_ = std::clamp(value_, lo_, hi_);
    invalidate_paint();
}

void Slider::set_format(std::function<std::string(double)> f) {
    format_ = std::move(f);
    invalidate_paint();
}

std::string Slider::readout() const { return format_ ? format_(value_) : fixed(value_, decimals_of(step_)); }

Rect Slider::track() const {
    const Rect b = local_bounds();
    const float w = std::max(20.0f, b.w - 60);
    return {8, b.h / 2 - 2, w - 16, 4};
}

double Slider::snap_value(double v) const {
    v = std::clamp(v, lo_, hi_);
    if (step_ > 0) v = lo_ + std::round((v - lo_) / step_) * step_;
    return std::clamp(v, lo_, hi_);
}

double Slider::value_at(float x) const {
    const Rect t = track();
    const double f = t.w > 0 ? std::clamp(static_cast<double>((x - t.x) / t.w), 0.0, 1.0) : 0.0;
    return snap_value(lo_ + f * (hi_ - lo_));
}

void Slider::user_set(double v, bool commit) {
    v = snap_value(v);
    const bool changed = v != value_;
    value_ = v;
    invalidate_paint();
    if (changed && on_change) on_change(value_);
    if (commit && on_commit) on_commit(value_);
}

EventResult Slider::on_pointer(PointerEvent& e) {
    if (!enabled()) return EventResult::Ignored;
    if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left) {
        dragging_ = true;
        user_set(value_at(e.local.x), false);
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Move && dragging_ && e.captured) {
        user_set(value_at(e.local.x), false);
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Release && dragging_) {
        dragging_ = false;
        user_set(value_at(e.local.x), true);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult Slider::on_key(const KeyEvent& k) {
    if (!pressed(k) || !enabled()) return EventResult::Ignored;
    const double s = step_ > 0 ? step_ : (hi_ - lo_) / 100;
    using K = platform::Key;
    switch (k.key) {
        case K::left:
        case K::down: user_set(value_ - s, true); return EventResult::Handled;
        case K::right:
        case K::up: user_set(value_ + s, true); return EventResult::Handled;
        case K::page_down: user_set(value_ - 10 * s, true); return EventResult::Handled;
        case K::page_up: user_set(value_ + 10 * s, true); return EventResult::Handled;
        case K::home: user_set(lo_, true); return EventResult::Handled;
        case K::end: user_set(hi_, true); return EventResult::Handled;
        default: return EventResult::Ignored;
    }
}

void Slider::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const float fade = enabled() ? 1.0f : 0.45f;
    const Rect tr = track();
    const float f = hi_ > lo_ ? static_cast<float>((value_ - lo_) / (hi_ - lo_)) : 0.0f;
    p.dl.round_rect(tr, 2, over(t.fg.with_alpha(0.18f), t.bg).fade(fade));
    p.dl.round_rect(Rect{tr.x, tr.y, tr.w * f, tr.h}, 2, t.accent.fade(fade));
    const float d = 16;
    const float x = px_round(tr.x + tr.w * f - d / 2, p.scale);
    const Rect knob{x, px_round(local_bounds().h / 2 - d / 2, p.scale), d, d};
    p.dl.shadow(knob.translated({0, 0.5f}), d / 2, 1.5f, Color::rgb(0x000000, 0.3f * fade));
    p.dl.round_rect(knob, d / 2, Color::rgb(0xFFFFFF, fade), 1, t.border.fade(fade));
    if (focus_visible()) paint_focus_ring(p, knob, d / 2);
    auto run = shape_cached(window(), readout(), TextStyle::mono_small, run_, run_text_);
    if (run) {
        const float rx = local_bounds().w - run->width();
        text_in(p, run, px_round(rx, p.scale), local_bounds(), t.fg_muted.fade(fade));
    }
}

// ---------------------------------------------------------------- Stepper

Stepper::Stepper(double lo, double hi, double step, double value, std::string id)
    : Widget(std::move(id)), lo_(lo), hi_(std::max(lo, hi)), step_(step > 0 ? step : 1), value_(std::clamp(value, lo, std::max(lo, hi))) {}

void Stepper::set_value(double v) {
    v = std::clamp(v, lo_, hi_);
    if (v == value_) return;
    value_ = v;
    invalidate_paint();
}

void Stepper::set_format(std::function<std::string(double)> f) {
    format_ = std::move(f);
    invalidate_paint();
}

std::string Stepper::text() const { return format_ ? format_(value_) : fixed(value_, decimals_of(step_)); }

void Stepper::step_by(int n) {
    const double v = std::clamp(value_ + n * step_, lo_, hi_);
    if (v == value_) return;
    value_ = v;
    invalidate_paint();
    if (on_change) on_change(value_);
}

Size Stepper::intrinsic_size(Size) const {
    float w = 40;
    if (auto run = shape_cached(window(), text(), TextStyle::body, run_, run_text_)) w = std::max(w, std::ceil(run->width()) + 16);
    return {w + 2 * 24, 24};
}

EventResult Stepper::on_pointer(PointerEvent& e) {
    if (!enabled() || e.button != platform::MouseButton::left) return EventResult::Ignored;
    const Rect b = local_bounds();
    if (e.type == PointerEvent::Type::Press) {
        pressed_ = e.local.x < 24 ? -1 : e.local.x >= b.w - 24 ? 1 : 0;
        invalidate_paint();
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Release) {
        const int which = e.local.x < 24 ? -1 : e.local.x >= b.w - 24 ? 1 : 0;
        if (which != 0 && which == pressed_) step_by(which);
        pressed_ = 0;
        invalidate_paint();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult Stepper::on_key(const KeyEvent& k) {
    if (!pressed(k) || !enabled()) return EventResult::Ignored;
    using K = platform::Key;
    if (k.key == K::up || k.key == K::right) {
        step_by(1);
        return EventResult::Handled;
    }
    if (k.key == K::down || k.key == K::left) {
        step_by(-1);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void Stepper::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float fade = enabled() ? 1.0f : 0.45f;
    p.dl.round_rect(b, t.radius_control, t.surface_raised.fade(fade), 1, t.border.fade(fade));
    p.dl.hairline(Rect{24, 4, 1, b.h - 8}, t.border.fade(fade));
    p.dl.hairline(Rect{b.w - 25, 4, 1, b.h - 8}, t.border.fade(fade));
    if (pressed_ < 0) p.dl.round_rect(Rect{1, 1, 23, b.h - 2}, t.radius_control - 1, t.fg.with_alpha(0.08f));
    if (pressed_ > 0) p.dl.round_rect(Rect{b.w - 24, 1, 23, b.h - 2}, t.radius_control - 1, t.fg.with_alpha(0.08f));
    const Color c = t.fg.fade(fade);
    const float cy = px_round(b.h / 2, p.scale);
    p.dl.rect(Rect{8, cy - 0.75f, 8, 1.5f}, value_ > lo_ ? c : c.fade(0.35f));
    const Color pc = value_ < hi_ ? c : c.fade(0.35f);
    p.dl.rect(Rect{b.w - 16, cy - 0.75f, 8, 1.5f}, pc);
    p.dl.rect(Rect{b.w - 12.75f, cy - 4, 1.5f, 8}, pc);
    if (auto run = shape_cached(window(), text(), TextStyle::body, run_, run_text_)) {
        text_in(p, run, px_round((b.w - run->width()) / 2, p.scale), b, c);
    }
    if (focus_visible()) paint_focus_ring(p, b, t.radius_control);
}

// ---------------------------------------------------------------- Dropdown

Dropdown::Dropdown(std::vector<DropdownOption> options, size_t selected, std::string id)
    : Widget(std::move(id)), options_(std::move(options)), selected_(selected), hover_(*this) {}

void Dropdown::set_options(std::vector<DropdownOption> options, size_t selected) {
    options_ = std::move(options);
    selected_ = selected;
    invalidate_layout();
    invalidate_paint();
}

void Dropdown::set_selected(size_t i) {
    if (i == selected_) return;
    selected_ = i;
    invalidate_layout();
    invalidate_paint();
}

void Dropdown::set_placeholder(std::string s) {
    placeholder_ = std::move(s);
    invalidate_paint();
}

std::string Dropdown::shown() const { return selected_ < options_.size() ? options_[selected_].label : placeholder_; }

std::string Dropdown::describe() const {
    std::string s = shown();
    if (!enabled()) s += " disabled";
    return s;
}

void Dropdown::choose(size_t i) {
    if (i >= options_.size()) return;
    const bool changed = i != selected_;
    set_selected(i);
    if (changed && on_change) on_change(i);
}

void Dropdown::open() {
    Window* w = window();
    if (!w || !enabled()) return;
    std::vector<MenuItem> items;
    for (size_t i = 0; i < options_.size(); ++i) {
        MenuItem m;
        m.label = options_[i].label;
        m.shortcut = options_[i].detail;
        m.checked = i == selected_;
        m.swatch = options_[i].swatch;
        m.id = "option[" + std::to_string(i) + "]";
        m.action = [this, i] { choose(i); };
        items.push_back(std::move(m));
    }
    Rect a = bounds_in_window();
    menu_ = open_menu_below(*w, std::move(items), a);
}

Size Dropdown::intrinsic_size(Size) const {
    float w = 60;
    if (Window* win = window()) {
        for (const auto& o : options_) {
            w = std::max(w, win->text().shape(o.label, role_for(win->theme(), TextStyle::body), win->scale())->width());
        }
        if (!placeholder_.empty())
            w = std::max(w, win->text().shape(placeholder_, role_for(win->theme(), TextStyle::body), win->scale())->width());
    }
    return {std::min(320.0f, std::ceil(w) + 40), 24};
}

void Dropdown::on_hover(bool inside) {
    hover_.set(inside ? 1.0f : 0.0f, hover_tr(inside));
    invalidate_paint();
}

EventResult Dropdown::on_pointer(PointerEvent& e) {
    if (!enabled() || e.button != platform::MouseButton::left) return EventResult::Ignored;
    if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
    if (e.type == PointerEvent::Type::Release && local_bounds().contains(e.local)) {
        open();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult Dropdown::on_key(const KeyEvent& k) {
    if (!pressed(k) || !enabled()) return EventResult::Ignored;
    using K = platform::Key;
    if (k.key == K::enter || k.key == K::space || k.key == K::kp_enter) {
        open();
        return EventResult::Handled;
    }
    if ((k.key == K::down || k.key == K::up) && !options_.empty()) {
        size_t i = selected_ < options_.size() ? selected_ : 0;
        if (k.key == K::down && i + 1 < options_.size()) ++i;
        if (k.key == K::up && i > 0) --i;
        choose(i);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void Dropdown::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float fade = enabled() ? 1.0f : 0.45f;
    p.dl.round_rect(b, t.radius_control, over(t.fg.with_alpha(0.05f * hover_.value()), t.surface_raised).fade(fade), 1,
                    t.border.fade(fade));
    float x = 8;
    if (selected_ < options_.size() && options_[selected_].swatch) {
        p.dl.round_rect(Rect{x, px_round(b.h / 2 - 5, p.scale), 10, 10}, 5, options_[selected_].swatch->fade(fade), 1,
                        t.border);
        x += 16;
    }
    if (Window* w = window()) {
        auto run = w->text().ellipsize(shown(), role_for(t, TextStyle::body), p.scale, std::max(0.0f, b.w - x - 26));
        text_in(p, run, x, b, (selected_ < options_.size() ? t.fg : t.fg_muted).fade(fade));
    }
    p.dl.icon(Icon::chevron_down, 12, {b.w - 20, px_round(b.h / 2 - 6, p.scale)}, t.fg_muted.fade(fade));
    if (focus_visible()) paint_focus_ring(p, b, t.radius_control);
}

// ---------------------------------------------------------------- SegmentedControl

SegmentedControl::SegmentedControl(std::vector<std::string> labels, size_t selected, std::string id)
    : Widget(std::move(id)), labels_(std::move(labels)), selected_(selected) {}

void SegmentedControl::set_selected(size_t i) {
    if (i == selected_) return;
    selected_ = i;
    invalidate_paint();
}

void SegmentedControl::choose(size_t i) {
    if (i >= labels_.size()) return;
    const bool changed = i != selected_;
    set_selected(i);
    if (changed && on_change) on_change(i);
}

std::string SegmentedControl::describe() const {
    std::string s = selected_ < labels_.size() ? labels_[selected_] : std::string("none");
    if (!enabled()) s += " disabled";
    return s;
}

std::vector<float> SegmentedControl::widths() const {
    std::vector<float> w;
    runs_.clear();
    Window* win = window();
    for (const auto& l : labels_) {
        TextRunPtr r = win ? win->text().shape(l, role_for(win->theme(), TextStyle::body), win->scale()) : nullptr;
        runs_.push_back(r);
        w.push_back(std::ceil(r ? r->width() : 8.0f * static_cast<float>(l.size())) + 20);
    }
    return w;
}

Size SegmentedControl::intrinsic_size(Size) const {
    float w = 0;
    for (float x : widths()) w += x;
    return {w + 4, 24};
}

std::optional<size_t> SegmentedControl::segment_at(float x) const {
    const std::vector<float> ws = widths();
    float total = 0;
    for (float v : ws) total += v;
    const float k = total > 0 ? (frame().w - 4) / total : 1;
    float cx = 2;
    for (size_t i = 0; i < ws.size(); ++i) {
        if (x >= cx && x < cx + ws[i] * k) return i;
        cx += ws[i] * k;
    }
    return std::nullopt;
}

EventResult SegmentedControl::on_pointer(PointerEvent& e) {
    if (!enabled() || e.button != platform::MouseButton::left) return EventResult::Ignored;
    if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
    if (e.type == PointerEvent::Type::Release) {
        if (auto i = segment_at(e.local.x)) choose(*i);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult SegmentedControl::on_key(const KeyEvent& k) {
    if (!pressed(k) || !enabled() || labels_.empty()) return EventResult::Ignored;
    using K = platform::Key;
    if (k.key == K::left && selected_ > 0 && selected_ < labels_.size()) {
        choose(selected_ - 1);
        return EventResult::Handled;
    }
    if (k.key == K::right) {
        choose(selected_ < labels_.size() ? std::min(selected_ + 1, labels_.size() - 1) : 0);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void SegmentedControl::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float fade = enabled() ? 1.0f : 0.45f;
    p.dl.round_rect(b, t.radius_control, over(t.fg.with_alpha(0.08f), t.bg).fade(fade));
    const std::vector<float> ws = widths();
    float total = 0;
    for (float v : ws) total += v;
    const float k = total > 0 ? (b.w - 4) / total : 1;
    float x = 2;
    for (size_t i = 0; i < ws.size(); ++i) {
        const Rect seg{px_round(x, p.scale), 2, px_round(ws[i] * k, p.scale), b.h - 4};
        const bool sel = i == selected_;
        if (sel) {
            p.dl.shadow(seg.translated({0, 0.5f}), t.radius_control - 1, 1, Color::rgb(0x000000, 0.18f * fade));
            p.dl.round_rect(seg, t.radius_control - 1, t.surface_raised.fade(fade), 1, t.border.fade(fade));
        } else if (i > 0 && i != selected_ + 1) {
            p.dl.hairline(Rect{seg.x, 6, 1, b.h - 12}, t.border.fade(fade));
        }
        if (runs_[i]) text_in(p, runs_[i], px_round(seg.x + (seg.w - runs_[i]->width()) / 2, p.scale), seg, (sel ? t.fg : t.fg_muted).fade(fade));
        x += ws[i] * k;
    }
    if (focus_visible()) paint_focus_ring(p, b, t.radius_control);
}

// ---------------------------------------------------------------- ColorWell

std::optional<Color> ColorWell::parse(std::string_view s) {
    if (s.size() != 7 && s.size() != 9) return std::nullopt;
    if (s[0] != '#') return std::nullopt;
    uint32_t v = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return std::nullopt;
        v = v * 16 + d;
    }
    if (s.size() == 9) return Color::rgb(v >> 8, static_cast<float>(v & 0xFF) / 255.0f);
    return Color::rgb(v);
}

ColorWell::ColorWell(std::string value, std::string id) : Widget(std::move(id)), value_(std::move(value)) {
    style().dir = LayoutStyle::Dir::Row;
    style().gap = 8;
    style().align_items = LayoutStyle::Align::Center;
    style().padding = Insets{36, 0, 0, 0};
    auto f = std::make_unique<TextField>("theme", "hex");
    f->style().width = 96.0f;
    f->set_text_style(TextStyle::mono_small);
    f->set_text(value_);
    field_ = &add(std::move(f));
    field_->on_submit = [this](const std::string& s) {
        if (s == value_) return;
        if (on_change) on_change(s);
    };
}

void ColorWell::set_value(std::string hex) {
    if (hex == value_) return;
    value_ = std::move(hex);
    if (!field_->focused()) field_->set_text(value_);
    invalidate_paint();
}

void ColorWell::set_fallback(std::optional<Color> c) {
    fallback_ = c;
    invalidate_paint();
}

Rect ColorWell::swatch() const { return {0, std::max(0.0f, frame().h / 2 - 12), 28, 24}; }

void ColorWell::open_presets() {
    Window* w = window();
    if (!w) return;
    std::vector<MenuItem> items;
    if (allow_empty_) {
        MenuItem m;
        m.label = "Use Theme Color";
        m.checked = value_.empty();
        m.swatch = fallback_;
        m.id = "theme";
        m.action = [this] {
            if (on_change) on_change("");
        };
        items.push_back(std::move(m));
        items.push_back(MenuItem::sep());
    }
    for (const auto& [name, c] : presets_) {
        MenuItem m;
        m.label = name;
        m.swatch = c;
        char buf[8];
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", static_cast<unsigned>(std::lround(c.r * 255)),
                      static_cast<unsigned>(std::lround(c.g * 255)), static_cast<unsigned>(std::lround(c.b * 255)));
        m.shortcut = buf;
        m.checked = value_ == buf;
        m.id = name;
        m.action = [this, hex = std::string(buf)] {
            if (on_change) on_change(hex);
        };
        items.push_back(std::move(m));
    }
    const Rect b = bounds_in_window();
    open_menu_below(*w, std::move(items), Rect{b.x, b.y, swatch().w, b.h});
}

EventResult ColorWell::on_pointer(PointerEvent& e) {
    if (!enabled() || e.button != platform::MouseButton::left) return EventResult::Ignored;
    if (!swatch().contains(e.local)) return EventResult::Ignored;
    if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
    if (e.type == PointerEvent::Type::Release) {
        open_presets();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void ColorWell::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect s = swatch();
    const auto c = parse(value_);
    p.dl.round_rect(s, t.radius_control, t.surface_raised, 1, t.border);
    const Rect in = s.inset(Insets::all(3));
    if (c) {
        p.dl.round_rect(in, t.radius_control - 2, *c);
    } else if (fallback_) {
        p.dl.round_rect(in, t.radius_control - 2, *fallback_);
        // A diagonal mark: the value comes from the theme.
        p.dl.rect(Rect{in.x + 2, in.bottom() - 4, in.w - 4, 1.5f}, t.fg_muted);
    } else {
        p.dl.round_rect(in, t.radius_control - 2, over(t.fg.with_alpha(0.06f), t.bg));
    }
}

// ---------------------------------------------------------------- KeyRecorder

KeyRecorder::KeyRecorder(std::string caps, std::string id) : Widget(std::move(id)), caps_(std::move(caps)), hover_(*this) {}

void KeyRecorder::set_caps(std::string caps) {
    caps_ = std::move(caps);
    invalidate_paint();
}

bool KeyRecorder::is_modifier(platform::Key k) {
    using K = platform::Key;
    return (k >= K::left_ctrl && k <= K::right_super) || k == K::caps_lock || k == K::num_lock;
}

void KeyRecorder::start() {
    if (recording_ || !enabled()) return;
    recording_ = true;
    if (Window* w = window(); w && !focused()) w->set_focus(this);
    invalidate_paint();
    if (on_recording) on_recording(true);
}

void KeyRecorder::cancel() {
    if (!recording_) return;
    recording_ = false;
    invalidate_paint();
    if (on_recording) on_recording(false);
}

std::string KeyRecorder::describe() const {
    if (recording_) return "recording";
    std::string s = caps_.empty() ? placeholder_ : caps_;
    if (conflict_) s += " conflict";
    return s;
}

void KeyRecorder::on_hover(bool inside) {
    hover_.set(inside ? 1.0f : 0.0f, hover_tr(inside));
    invalidate_paint();
}

void KeyRecorder::on_focus(FocusChange c) {
    if (c == FocusChange::Lost) cancel();
    invalidate_paint();
}

EventResult KeyRecorder::on_pointer(PointerEvent& e) {
    if (!enabled() || e.button != platform::MouseButton::left) return EventResult::Ignored;
    if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
    if (e.type == PointerEvent::Type::Release && local_bounds().contains(e.local)) {
        if (recording_) cancel();
        else start();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult KeyRecorder::on_key(const KeyEvent& k) {
    using K = platform::Key;
    if (!recording_) {
        if (pressed(k) && (k.key == K::enter || k.key == K::space || k.key == K::kp_enter)) {
            start();
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }
    // While recording every key belongs to the recorder (releases too).
    if (!pressed(k) || is_modifier(k.key) || k.composing) return EventResult::Handled;
    const uint16_t mods = k.mods & (platform::mod_shift | platform::mod_ctrl | platform::mod_alt | platform::mod_super);
    if (k.key == K::escape && mods == 0) {
        cancel();
        return EventResult::Handled;
    }
    if ((k.key == K::backspace || k.key == K::delete_) && mods == 0) {
        recording_ = false;
        invalidate_paint();
        if (on_recording) on_recording(false);
        if (on_clear) on_clear();
        return EventResult::Handled;
    }
    recording_ = false;
    invalidate_paint();
    if (on_recording) on_recording(false);
    if (on_record) on_record(k);
    return EventResult::Handled;
}

void KeyRecorder::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float fade = enabled() ? 1.0f : 0.45f;
    Color border = conflict_ ? t.warning : t.border;
    if (recording_) border = t.accent;
    p.dl.round_rect(b, t.radius_control,
                    over(t.fg.with_alpha(0.05f * hover_.value() + (recording_ ? 0.04f : 0.0f)), t.surface_raised).fade(fade),
                    recording_ ? 1.5f : 1.0f, border.fade(fade));
    Window* w = window();
    if (!w) return;
    const std::string text = recording_ ? std::string("Press keys\xE2\x80\xA6") : caps_.empty() ? placeholder_ : caps_;
    const TextStyle st = recording_ || caps_.empty() ? TextStyle::body : TextStyle::mono_body;
    const Color c = recording_ ? t.accent : caps_.empty() ? t.fg_dim : t.fg;
    auto run = w->text().ellipsize(text, role_for(t, st), p.scale, std::max(0.0f, b.w - 16));
    if (run) text_in(p, run, px_round((b.w - run->width()) / 2, p.scale), b, c.fade(fade));
    if (focus_visible() && !recording_) paint_focus_ring(p, b, t.radius_control);
}

// ---------------------------------------------------------------- SearchField

SearchField::SearchField(std::string placeholder, std::string id) : Widget(std::move(id)) {
    style().dir = LayoutStyle::Dir::Row;
    style().align_items = LayoutStyle::Align::Center;
    style().padding = Insets{26, 0, 2, 0};
    style().height = 28.0f;
    auto f = std::make_unique<TextField>(std::move(placeholder), "field");
    f->set_frameless(true);
    f->style().grow = 1;
    field_ = &add(std::move(f));
    field_->on_change = [this](const std::string& s) {
        clear_->set_visible(!s.empty());
        if (on_change) on_change(s);
    };
    auto c = Button::icon_button(Icon::close, "Clear", [this] {
        field_->set_text("");
        clear_->set_visible(false);
        if (on_change) on_change("");
        if (Window* w = window()) w->set_focus(field_);
    }, "clear", 20);
    clear_ = &add(std::move(c));
    clear_->set_visible(false);
}

void SearchField::set_text(std::string t) {
    clear_->set_visible(!t.empty());
    field_->set_text(std::move(t));
}

EventResult SearchField::on_pointer(PointerEvent& e) {
    if (e.type == PointerEvent::Type::Press) {
        if (Window* w = window()) w->set_focus(field_);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void SearchField::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    p.dl.round_rect(b, t.radius_control + 1, over(t.fg.with_alpha(0.06f), t.bg), 1,
                    field_->focused() ? t.accent.with_alpha(0.8f) : t.border);
    p.dl.icon(Icon::search, 14, {8, px_round(b.h / 2 - 7, p.scale)}, t.fg_muted);
}

// ---------------------------------------------------------------- SectionList

SectionList::SectionList(std::vector<SectionItem> items, std::string id) : ListView({}, std::move(id)), items_(std::move(items)) {
    set_row_height(30);
    set_count(items_.size());
    set_paint_row([this](size_t i, PaintContext& p, Rect r, RowState st) {
        if (i >= items_.size()) return;
        const Theme& t = p.theme;
        const SectionItem& it = items_[i];
        const Rect pill = r.inset(Insets::hv(6, 2));
        if (st.selected) p.dl.round_rect(pill, t.radius_control, st.focused ? t.accent.with_alpha(0.9f) : t.selection);
        else if (st.hovered) p.dl.round_rect(pill, t.radius_control, t.hover);
        const Color fg = st.selected && st.focused ? t.on_accent : t.fg;
        float x = pill.x + 8;
        if (it.icon != Icon::none) {
            p.dl.icon(it.icon, 16, {x, px_round(pill.y + (pill.h - 16) / 2, p.scale)}, st.selected && st.focused ? fg : t.accent);
            x += 24;
        }
        float right = pill.right() - 8;
        if (!it.badge.empty()) {
            auto br = p.text.shape(it.badge, role_for(t, TextStyle::small_bold), p.scale);
            right -= br->width();
            text_in(p, br, px_round(right, p.scale), pill, st.selected && st.focused ? fg : t.fg_muted);
            right -= 6;
        }
        auto run = p.text.ellipsize(it.title, role_for(t, TextStyle::body), p.scale, std::max(0.0f, right - x));
        text_in(p, run, x, pill, fg);
    });
}

void SectionList::set_badge(size_t i, std::string b) {
    if (i >= items_.size() || items_[i].badge == b) return;
    items_[i].badge = std::move(b);
    invalidate_paint();
}

std::optional<size_t> SectionList::index_of(std::string_view id) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) return i;
    }
    return std::nullopt;
}

std::string SectionList::describe() const {
    auto s = selected();
    return s && *s < items_.size() ? items_[*s].title : std::string();
}

// ---------------------------------------------------------------- SchemePreview

SchemePreview::SchemePreview(std::string id) : Widget(std::move(id)) {}

void SchemePreview::set_scheme(SchemeColors s) {
    s_ = std::move(s);
    invalidate_paint();
}

void SchemePreview::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    p.dl.round_rect(b, t.radius_panel - 2, s_.background, 1, t.border);
    const FontRole mono = role_for(t, TextStyle::mono_small);
    const float line = 16;
    float y = 10;
    auto put = [&](float& x, std::string_view s, Color c, float baseline) {
        auto run = p.text.shape(s, mono, p.scale);
        p.dl.text(run, {px_round(x, p.scale), px_round(baseline, p.scale)}, c);
        x += run->width();
    };
    const float asc = p.text.metrics(mono, p.scale).ascent;
    {
        float x = 12;
        put(x, "~/src ", s_.palette[4], y + asc);
        put(x, "main ", s_.palette[5], y + asc);
        put(x, "$ ", s_.foreground, y + asc);
        put(x, "ls -l", s_.foreground, y + asc);
    }
    y += line;
    {
        float x = 12;
        put(x, "drwxr-xr-x ", s_.palette[8], y + asc);
        put(x, "docs  ", s_.palette[12], y + asc);
        put(x, "build.sh  ", s_.palette[2], y + asc);
        put(x, "error.log", s_.palette[1], y + asc);
    }
    y += line;
    {
        float x = 12;
        const Rect sel{x - 1, y + 1, 70, line - 2};
        p.dl.rect(sel, s_.selection);
        put(x, "selected", s_.foreground, y + asc);
        x += 8;
        put(x, "$ ", s_.foreground, y + asc);
        p.dl.rect(Rect{px_round(x, p.scale), y + 1, 7, line - 2}, s_.cursor);
    }
    // 16 swatches in two rows at the bottom.
    const float sw = std::min(22.0f, (b.w - 24) / 8 - 4);
    const float sy = b.h - 2 * (sw * 0.6f + 4) - 6;
    for (size_t i = 0; i < 16; ++i) {
        const float x = 12 + static_cast<float>(i % 8) * (sw + 4);
        const float yy = sy + static_cast<float>(i / 8) * (sw * 0.6f + 4);
        p.dl.round_rect(Rect{x, yy, sw, sw * 0.6f}, 2, s_.palette[i]);
    }
}

// ---------------------------------------------------------------- form layout

namespace {

// The body of a group: rows with hairline separators between them.
class GroupBody final : public Box {
public:
    GroupBody() : Box("rows", LayoutStyle::Dir::Column) {}
    void paint(PaintContext& p) const override {
        const Theme& t = p.theme;
        p.dl.round_rect(local_bounds(), t.radius_panel - 2, t.surface_raised, 1, t.border);
        bool first = true;
        for (const auto& c : children()) {
            if (!c->visible()) continue;
            if (!first) p.dl.hairline(Rect{12, c->frame().y, frame().w - 24, 1}, t.border);
            first = false;
        }
    }
};

}  // namespace

FormRow::FormRow(std::string label, std::string description, std::unique_ptr<Widget> control, std::string id, bool wide)
    : Widget(std::move(id)) {
    style().dir = LayoutStyle::Dir::Column;
    style().padding = Insets::hv(12, 8);
    style().gap = 4;
    auto top = row("line", 12);
    top->style().align_items = LayoutStyle::Align::Center;
    auto left = column("text", 2);
    left->style().grow = 1;
    left->style().shrink = 1;
    left->style().basis = 0.0f;
    label_ = &left->emplace<Label>(std::move(label), TextStyle::body, Tone::normal, "label");
    label_->set_wrap(true);
    if (!description.empty()) {
        desc_ = &left->emplace<Label>(std::move(description), TextStyle::small, Tone::muted, "description");
        desc_->set_wrap(true);
    }
    top->add_child(std::move(left));
    if (control) {
        control_ = control.get();
        if (wide) {
            add_child(std::move(top));
            control->style().align_self = LayoutStyle::Align::Stretch;
            add_child(std::move(control));
        } else {
            control->style().shrink = 0;
            top->add_child(std::move(control));
            add_child(std::move(top));
        }
    } else {
        add_child(std::move(top));
    }
    note_ = &emplace<Label>("", TextStyle::small, Tone::muted, "note");
    note_->set_wrap(true);
    note_->set_visible(false);
    error_label_ = &emplace<Label>("", TextStyle::small, Tone::failure, "error");
    error_label_->set_wrap(true);
    error_label_->set_visible(false);
}

void FormRow::set_error(std::string e) {
    if (e == error_) return;
    error_ = e;
    error_label_->set_text(std::move(e));
    error_label_->set_visible(!error_.empty());
    invalidate_layout();
}

const std::string& FormRow::note() const { return note_->text(); }

void FormRow::set_note(std::string n, Tone tone) {
    note_->set_tone(tone);
    note_->set_visible(!n.empty());
    note_->set_text(std::move(n));
    invalidate_layout();
}

void FormRow::set_dimmed(bool d) {
    label_->set_tone(d ? Tone::dim : Tone::normal);
    if (control_) control_->set_enabled(!d);
}

EventResult FormRow::on_pointer(PointerEvent& e) {
    if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::right && on_context) {
        on_context(e.pos);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

std::string FormRow::describe() const {
    std::string s = label_->text();
    if (control_) s += ": " + control_->describe();
    if (!error_.empty()) s += " [" + error_ + "]";
    return s;
}

FormGroup::FormGroup(std::string title, std::string id) : Widget(std::move(id)) {
    style().dir = LayoutStyle::Dir::Column;
    style().gap = 6;
    if (!title.empty()) {
        title_ = &emplace<Label>(std::move(title), TextStyle::body_bold, Tone::normal, "title");
        title_->style().margin = Insets{4, 0, 0, 0};
    }
    body_ = &add(std::make_unique<GroupBody>());
    foot_ = &emplace<Label>("", TextStyle::small, Tone::muted, "footnote");
    foot_->set_wrap(true);
    foot_->style().margin = Insets{4, 0, 4, 0};
    foot_->set_visible(false);
}

void FormGroup::set_footnote(std::string f) {
    foot_->set_visible(!f.empty());
    foot_->set_text(std::move(f));
}

void FormGroup::paint(PaintContext&) const {}

}  // namespace kt::ui
