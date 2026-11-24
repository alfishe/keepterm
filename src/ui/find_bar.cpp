#include "ui/find_bar.h"

#include "ui/window.h"

namespace kt::ui {

namespace {
constexpr float kWidth = 340, kHeight = 36;
}

FindBar::FindBar(FindBarHooks hooks, std::string initial) : Widget("find_bar"), hooks_(std::move(hooks)) {
    style().dir = LayoutStyle::Dir::Row;
    style().align_items = LayoutStyle::Align::Center;
    style().width = kWidth;
    style().height = kHeight;
    style().gap = 4;
    style().padding = Insets{10, 0, 6, 0};
    add_child(std::make_unique<IconView>(Icon::search, 14.0f, Tone::muted));
    field_ = &emplace<TextField>(std::string(tr("Find")), "field");
    field_->set_frameless(true);
    field_->style().grow = 1;
    field_->style().shrink = 1;
    field_->style().basis = 0.0f;
    field_->on_change = [this](const std::string& q) {
        if (hooks_.query) show(hooks_.query(q));
    };
    // Enter (the next older match) and Shift+Enter bubble up to on_key.
    field_->on_cancel = [this] { close(); };
    count_ = &emplace<Label>("", TextStyle::small, Tone::muted, "count");
    add(Button::icon_button(Icon::chevron_down, std::string(tr("Previous Match")), [this] { step(-1); }, "previous", 24));
    add(Button::icon_button(Icon::chevron_right, std::string(tr("Next Match")), [this] { step(1); }, "next", 24));
    add(Button::icon_button(Icon::close, std::string(tr("Close")), [this] { close(); }, "close", 24));
    if (!initial.empty()) {
        field_->set_text(initial);
        if (hooks_.query) show(hooks_.query(initial));
    }
}

EventResult FindBar::on_key(const KeyEvent& k) {
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    if (k.key == platform::Key::enter || k.key == platform::Key::kp_enter) {
        step((k.mods & platform::mod_shift) ? 1 : -1);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void FindBar::show(std::pair<size_t, std::optional<size_t>> r) {
    if (field_->text().empty()) count_->set_text({});
    else if (r.first == 0) count_->set_text(std::string(tr("No matches")));
    else count_->set_text(std::to_string(r.second ? *r.second + 1 : 0) + " " + std::string(tr("of")) + " " + std::to_string(r.first));
}

void FindBar::step(int delta) {
    if (hooks_.step) show(hooks_.step(delta));
}

void FindBar::close() {
    if (closed_) return;
    closed_ = true;
    if (hooks_.closed) hooks_.closed();   // the owner closes the overlay
}

std::string FindBar::describe() const { return field_->text() + " | " + count_->text(); }

void FindBar::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    p.dl.shadow(Rect{b.x, b.y + 3, b.w, b.h}, 8, 8, t.shadow);
    p.dl.round_rect(b, 8, over(t.fg.with_alpha(t.scheme == Scheme::dark ? 0.07f : 0.0f), t.bg), 1,
                    t.fg.with_alpha(t.scheme == Scheme::dark ? 0.14f : 0.12f));
}

WidgetId show_find_bar(Window& w, FindBarHooks hooks, Vec2 top_right, std::string initial) {
    OverlayOptions o;
    o.placement = OverlayOptions::Placement::At;
    o.anchor = Rect{top_right.x - kWidth, top_right.y, 0, 0};
    o.modal = false;
    o.transient = false;
    o.dismiss_on_outside_click = false;
    o.dismiss_on_escape = false;   // Escape goes to the field (on_cancel)
    o.take_focus = true;
    auto bar = std::make_unique<FindBar>(std::move(hooks), std::move(initial));
    TextField* f = &bar->field();
    const WidgetId id = w.show_overlay(std::move(bar), o);
    w.set_focus(f, true);
    f->select_all();
    return id;
}

}  // namespace kt::ui
