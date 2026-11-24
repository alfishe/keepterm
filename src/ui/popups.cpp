// Tooltip, toast, menu and dialog.
#include <algorithm>
#include <cmath>

#include "ui/widgets.h"

namespace kt::ui {

namespace {

constexpr float kMenuPad = 5, kMenuRow = 24, kMenuSep = 9;

float px_round(float v, float scale) { return std::round(v * scale) / scale; }

Color panel_fill(const Theme& t) { return over(t.fg.with_alpha(t.scheme == Scheme::dark ? 0.07f : 0.0f), t.bg); }

void paint_panel(PaintContext& p, Rect b, float radius) {
    const Theme& t = p.theme;
    p.dl.shadow(Rect{b.x, b.y + 4, b.w, b.h}, radius, 10, t.shadow);
    p.dl.round_rect(b, radius, panel_fill(t), 1, t.fg.with_alpha(t.scheme == Scheme::dark ? 0.14f : 0.12f));
}

}  // namespace

// ---------------------------------------------------------------- Tooltip

Tooltip::Tooltip(std::string text) : Widget("tooltip") {
    style().padding = Insets::hv(8, 4);
    set_hit_testable(false);
    // The text may hold several lines ("key\ndescription"); long lines wrap to the width.
    auto& l = emplace<Label>(std::move(text), TextStyle::small, Tone::normal);
    l.set_wrap(true);
    style().max_w = 360;
}

void Tooltip::paint(PaintContext& p) const { paint_panel(p, local_bounds(), 5); }

// ---------------------------------------------------------------- Toast

Toast::Toast(const ToastSpec& s) : Widget("toast"), title_(s.title), alpha_(*this, 0) {
    style().dir = LayoutStyle::Dir::Row;
    style().align_items = LayoutStyle::Align::Center;
    style().gap = 10;
    style().padding = Insets::hv(14, 10);
    style().max_w = 460;
    set_hit_testable(false);
    if (s.icon != Icon::none) emplace<IconView>(s.icon, 16.0f, s.tone == Tone::normal ? Tone::accent : s.tone);
    auto col = column({}, 2);
    col->style().shrink = 1;
    col->add(std::make_unique<Label>(s.title, TextStyle::body_bold, Tone::normal, "title"));
    if (!s.subtitle.empty()) col->add(std::make_unique<Label>(s.subtitle, TextStyle::small, Tone::muted, "subtitle"));
    add(std::move(col));
    alpha_.on_value([this](float v) {
        set_opacity(v);
        set_translation({0, (1 - v) * -6});
    });
    set_opacity(0);
}

void Toast::fade(float to, std::function<void()> done) {
    alpha_.set(to, Transition::curve(std::chrono::milliseconds(to > 0 ? 150 : 200), Easing::ease_out(), false));
    if (!done) return;
    if (!alpha_.running()) {
        done();
        return;
    }
    if (Window* w = window()) w->add_timer(std::chrono::milliseconds(220), std::move(done));
}

void Toast::paint(PaintContext& p) const { paint_panel(p, local_bounds(), 12); }

WidgetId show_toast(Window& w, const ToastSpec& spec) {
    // A new toast replaces the shown one (the overlay entry of a toast is the toast itself).
    for (int i = 0; i < 4; ++i) {
        Widget* t = w.find("toast");
        if (!t) break;
        w.close_overlay(t->id());
    }
    auto toast = std::make_unique<Toast>(spec);
    Toast* raw = toast.get();
    OverlayOptions o;
    o.placement = OverlayOptions::Placement::TopCenter;
    o.pass_through = true;
    o.take_focus = false;
    o.transient = false;
    o.dismiss_on_escape = false;
    o.dismiss_on_outside_click = false;
    o.margin = 12;
    const WidgetId id = w.show_overlay(std::move(toast), o);
    raw->fade(1);
    Window* win = &w;
    w.add_timer(spec.duration, [win, id] {
        auto* t = dynamic_cast<Toast*>(win->overlay(id));
        if (!t) return;
        t->fade(0, [win, id] { win->close_overlay(id); });
    });
    return id;
}

// ---------------------------------------------------------------- Menu

Menu::Menu(std::vector<MenuItem> items, std::string id) : Widget(std::move(id)), items_(std::move(items)) {
    for (auto& m : items_) {
        if (m.id.empty()) m.id = m.label;
    }
}

std::string Menu::describe() const {
    std::string s;
    for (const auto& m : items_) {
        if (m.separator) continue;
        if (!s.empty()) s += "|";
        s += m.label;
        if (!m.enabled) s += "(disabled)";
    }
    return s;
}

float Menu::row_y(size_t i) const {
    float y = kMenuPad;
    for (size_t k = 0; k < i && k < items_.size(); ++k) y += items_[k].separator ? kMenuSep : kMenuRow;
    return y;
}

std::optional<size_t> Menu::item_at(float y) const {
    float top = kMenuPad;
    for (size_t i = 0; i < items_.size(); ++i) {
        const float h = items_[i].separator ? kMenuSep : kMenuRow;
        if (y >= top && y < top + h) {
            if (items_[i].separator || !items_[i].enabled) return std::nullopt;
            return i;
        }
        top += h;
    }
    return std::nullopt;
}

Size Menu::intrinsic_size(Size) const {
    Window* win = window();
    float h = 2 * kMenuPad, w = 0;
    for (const auto& m : items_) {
        h += m.separator ? kMenuSep : kMenuRow;
        if (m.separator || !win) continue;
        float rw = win->text().shape(m.label, role_for(win->theme(), TextStyle::body), win->scale())->width();
        if (!m.shortcut.empty()) rw += 28 + win->text().shape(m.shortcut, role_for(win->theme(), TextStyle::body), win->scale())->width();
        if (!m.submenu.empty()) rw += 24;
        w = std::max(w, rw);
    }
    return {std::max(180.0f, std::ceil(w) + 2 * kMenuPad + 20 + 16), h};
}

void Menu::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    paint_panel(p, b, 8);
    const FontRole role = role_for(t, TextStyle::body);
    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& m = items_[i];
        const float y = row_y(i);
        if (m.separator) {
            p.dl.hairline(Rect{kMenuPad + 6, y + kMenuSep / 2, b.w - 2 * kMenuPad - 12, 1}, t.border);
            continue;
        }
        const Rect row{kMenuPad, y, b.w - 2 * kMenuPad, kMenuRow};
        const bool hl = hl_ == i && m.enabled;
        if (hl) p.dl.round_rect(row, 4, t.accent);
        const Color fg = !m.enabled ? t.fg_dim : hl ? t.on_accent : t.fg;
        const Color muted = hl ? t.on_accent.with_alpha(0.8f) : t.fg_muted;
        const float cy = y + kMenuRow / 2;
        if (m.checked) p.dl.icon(Icon::check, 12, {row.x + 5, px_round(cy - 6, p.scale)}, fg);
        else if (m.swatch) p.dl.round_rect(Rect{row.x + 6, cy - 5, 10, 10}, 5, *m.swatch, 1, t.border);
        else if (m.icon != Icon::none) p.dl.icon(m.icon, 13, {row.x + 4, px_round(cy - 6.5f, p.scale)}, hl ? fg : t.fg_muted);
        auto run = p.text.shape(m.label, role, p.scale);
        const float base = px_round(cy - run->height() / 2 + run->ascent(), p.scale);
        p.dl.text(run, {row.x + 22, base}, fg);
        if (!m.shortcut.empty()) {
            auto sr = p.text.shape(m.shortcut, role, p.scale);
            p.dl.text(sr, {px_round(row.right() - 10 - sr->width(), p.scale), base}, muted);
        }
        if (!m.submenu.empty()) p.dl.icon(Icon::chevron_right, 12, {row.right() - 18, px_round(cy - 6, p.scale)}, muted);
    }
}

void Menu::on_hover(bool inside) {
    if (!inside && !sub_) {
        hl_.reset();
        invalidate_paint();
    }
}

void Menu::open_submenu(size_t i) {
    Window* win = window();
    if (!win || items_[i].submenu.empty()) return;
    if (sub_index_ == i && sub_ && win->overlay(sub_)) return;
    close_submenu();
    const Rect b = bounds_in_window();
    OverlayOptions o;
    o.anchor = Rect{b.x + kMenuPad, b.y + row_y(i) - kMenuPad, b.w - 2 * kMenuPad, kMenuRow};
    o.placement = OverlayOptions::Placement::Right;
    o.take_focus = false;
    auto m = std::make_unique<Menu>(items_[i].submenu, "submenu");
    m->set_parent_menu(this);
    sub_ = win->show_overlay(std::move(m), o);
    sub_index_ = i;
}

void Menu::close_submenu() {
    Window* win = window();
    if (sub_ && win) {
        const WidgetId id = sub_;
        sub_ = 0;
        win->close_overlay(id);
    }
    sub_index_.reset();
}

void Menu::close_all() {
    Menu* top = this;
    while (top->parent_menu_) top = top->parent_menu_;
    Window* win = window();
    if (win) win->close_transient();
}

void Menu::activate(size_t i) {
    if (i >= items_.size() || items_[i].separator || !items_[i].enabled) return;
    if (!items_[i].submenu.empty()) {
        open_submenu(i);
        if (Window* win = window()) {
            if (auto* m = dynamic_cast<Menu*>(win->overlay(sub_))) {
                win->set_focus(m, true);
                m->hl_ = 0;
                m->invalidate_paint();
            }
        }
        return;
    }
    auto action = items_[i].action;
    close_all();   // destroys this menu
    if (action) action();
}

EventResult Menu::on_pointer(PointerEvent& e) {
    const auto at = local_bounds().contains(e.local) ? item_at(e.local.y) : std::nullopt;
    switch (e.type) {
        case PointerEvent::Type::Move:
            if (at != hl_) {
                hl_ = at;
                invalidate_paint();
                if (at && !items_[*at].submenu.empty()) open_submenu(*at);
                else if (at) close_submenu();
            }
            return EventResult::Handled;
        case PointerEvent::Type::Press: return EventResult::Handled;
        case PointerEvent::Type::Release:
            if (at) activate(*at);
            return EventResult::Handled;
        default: return EventResult::Ignored;
    }
}

EventResult Menu::on_key(const KeyEvent& k) {
    using K = platform::Key;
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    auto step = [&](int dir) {
        const auto n = static_cast<int>(items_.size());
        int i = hl_ ? static_cast<int>(*hl_) : (dir > 0 ? -1 : n);
        for (int guard = 0; guard < n; ++guard) {
            i = (i + dir + n) % n;
            const auto& m = items_[static_cast<size_t>(i)];
            if (!m.separator && m.enabled) {
                hl_ = static_cast<size_t>(i);
                invalidate_paint();
                return;
            }
        }
    };
    switch (k.key) {
        case K::up: step(-1); return EventResult::Handled;
        case K::down: step(1); return EventResult::Handled;
        case K::enter:
        case K::kp_enter:
        case K::space:
            if (hl_) activate(*hl_);
            return EventResult::Handled;
        case K::right:
            if (hl_ && !items_[*hl_].submenu.empty()) activate(*hl_);
            return EventResult::Handled;
        case K::left:
            if (parent_menu_ && window()) {
                Menu* parent = parent_menu_;
                Window* win = window();
                win->set_focus(parent, true);
                parent->close_submenu();   // destroys this menu
            }
            return EventResult::Handled;
        default: return EventResult::Ignored;
    }
}

WidgetId open_menu(Window& w, std::vector<MenuItem> items, Vec2 at) {
    w.close_transient();
    OverlayOptions o;
    o.anchor = Rect{at.x, at.y, 0, 0};
    o.placement = OverlayOptions::Placement::At;
    return w.show_overlay(std::make_unique<Menu>(std::move(items)), o);
}

WidgetId open_menu_below(Window& w, std::vector<MenuItem> items, Rect anchor) {
    w.close_transient();
    OverlayOptions o;
    o.anchor = anchor;
    o.placement = OverlayOptions::Placement::Below;
    return w.show_overlay(std::make_unique<Menu>(std::move(items)), o);
}

// ---------------------------------------------------------------- Dialog

Dialog::Dialog(DialogSpec spec) : Widget("dialog"), buttons_(std::move(spec.buttons)) {
    style().padding = Insets::all(20);
    style().gap = 12;
    style().width = spec.width;
    if (!spec.title.empty()) emplace<Label>(spec.title, TextStyle::body_bold, Tone::normal, "title");
    if (!spec.message.empty()) {
        auto& m = emplace<Label>(spec.message, TextStyle::body, Tone::muted, "message");
        m.set_wrap(true);
    }
    if (spec.body) add_child(std::move(spec.body));
    auto row_box = row("buttons", 8);
    row_box->style().justify = LayoutStyle::Justify::End;
    row_box->style().margin.t = 4;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const auto& b = buttons_[i];
        auto btn = std::make_unique<Button>(b.label, [this, i] { press(i); }, b.kind);
        row_box->add(std::move(btn));
    }
    add(std::move(row_box));
}

void Dialog::paint(PaintContext& p) const { paint_panel(p, local_bounds(), p.theme.radius_panel); }

void Dialog::press(size_t i) {
    if (i >= buttons_.size()) return;
    auto action = buttons_[i].action;
    *pressed_ = true;
    Window* win = window();
    if (win && overlay_) win->close_overlay(overlay_);   // destroys this dialog
    if (action) action();
}

EventResult Dialog::on_key(const KeyEvent& k) {
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    if (k.key == platform::Key::enter || k.key == platform::Key::kp_enter) {
        for (size_t i = 0; i < buttons_.size(); ++i) {
            if (buttons_[i].is_default) {
                press(i);
                return EventResult::Handled;
            }
        }
    }
    return EventResult::Ignored;
}

WidgetId show_dialog(Window& w, DialogSpec spec) {
    w.close_transient();
    std::function<void()> cancel;
    for (const auto& b : spec.buttons) {
        if (b.is_cancel) cancel = b.action;
    }
    size_t def = spec.buttons.size();
    for (size_t i = 0; i < spec.buttons.size(); ++i) {
        if (spec.buttons[i].is_default) def = i;
    }
    // A body with fields keeps the focus for itself (its caller focuses one); a body that only
    // shows things (a table) leaves Enter to the default button.
    std::function<bool(const Widget&)> takes_focus = [&](const Widget& x) {
        if (x.accepts_focus()) return true;
        for (size_t i = 0; i < x.child_count(); ++i) {
            if (const Widget* c = x.child(i); c && takes_focus(*c)) return true;
        }
        return false;
    };
    const bool has_body = spec.body != nullptr && takes_focus(*spec.body);
    auto d = std::make_unique<Dialog>(std::move(spec));
    Dialog* raw = d.get();
    OverlayOptions o;
    o.modal = true;
    o.placement = OverlayOptions::Placement::Center;
    o.dismiss_on_outside_click = false;
    o.transient = false;
    // Escape closes the overlay through the window: that runs the cancel button's action.
    auto pressed = raw->pressed_flag();
    o.on_close = [pressed, cancel] {
        if (!*pressed && cancel) cancel();
    };
    const WidgetId id = w.show_overlay(std::move(d), o);
    raw->set_overlay(id);
    if (!has_body) {
        if (Widget* buttons = raw->child(raw->child_count() - 1); buttons && def < buttons->child_count()) {
            w.set_focus(buttons->child(def), false);
        }
    }
    return id;
}

}  // namespace kt::ui
