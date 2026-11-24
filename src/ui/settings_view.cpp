#include "ui/settings_view.h"

namespace kt::ui {

SettingsView::SettingsView(std::vector<SectionItem> sections) : Widget("settings") {
    style().dir = LayoutStyle::Dir::Row;
    style().width = 820.0f;
    style().height = 600.0f;

    auto side = column("sidebar", 8);
    side->style().width = 200.0f;
    side->style().padding = Insets{10, 14, 10, 10};
    auto sf = std::make_unique<SearchField>("Search settings", "search");
    sf->style().shrink = 0;
    search_ = &side->add(std::move(sf));
    search_->on_change = [this](const std::string& q) {
        if (on_search) on_search(q);
    };
    auto list = std::make_unique<SectionList>(std::move(sections), "sections");
    list->style().grow = 1;
    list->style().basis = 0.0f;
    list_ = &side->add(std::move(list));
    list_->on_select = [this](size_t i) {
        if (on_section) on_section(i);
    };
    list_->on_activate = list_->on_select;
    add_child(std::move(side));

    auto main = column("main", 0);
    main->style().grow = 1;
    main->style().basis = 0.0f;
    auto head = row("header", 8);
    head->style().padding = Insets{24, 14, 12, 10};
    head->style().align_items = LayoutStyle::Align::Center;
    head->style().shrink = 0;
    title_ = &head->emplace<Label>("", TextStyle::title, Tone::normal, "title");
    title_->style().grow = 1;
    close_ = &head->add(Button::icon_button(Icon::close, "Close Settings", [this] {
        if (on_close) on_close();
    }, "close"));
    main->add_child(std::move(head));

    auto banner = std::make_unique<Box>("banner", LayoutStyle::Dir::Row);
    banner->style().margin = Insets{24, 0, 24, 8};
    banner->style().padding = Insets::hv(10, 8);
    banner->style().gap = 8;
    banner->style().align_items = LayoutStyle::Align::Center;
    banner->style().shrink = 0;
    banner->emplace<IconView>(Icon::alert, 14.0f, Tone::warning, "icon");
    banner_label_ = &banner->emplace<Label>("", TextStyle::small, Tone::normal, "text");
    banner_label_->set_wrap(true);
    banner_label_->style().grow = 1;
    banner_label_->style().basis = 0.0f;
    banner_ = &main->add(std::move(banner));
    banner_->set_visible(false);

    auto sc = std::make_unique<ScrollView>(nullptr, "scroll");
    sc->style().grow = 1;
    sc->style().basis = 0.0f;
    scroll_ = &main->add(std::move(sc));
    add_child(std::move(main));
}

void SettingsView::set_windowed(bool w) {
    windowed_ = w;
    if (w) {
        style().width.reset();
        style().height.reset();
    } else {
        style().width = 820.0f;
        style().height = 600.0f;
    }
    close_->set_visible(!w);
    invalidate_layout();
}

void SettingsView::select(size_t i) {
    list_->set_selected(i);
    if (on_section) on_section(i);
}

void SettingsView::set_page(std::string t, std::unique_ptr<Widget> content, bool keep_scroll) {
    title_->set_text(std::move(t));
    const float off = scroll_->offset();
    if (content) {
        content->style().padding = Insets{24, 0, 24, 24};
    }
    scroll_->set_content(std::move(content));
    if (keep_scroll) {
        if (Window* w = window()) w->layout();
        scroll_->scroll_to(off);
    } else {
        scroll_->scroll_to(0);
    }
}

void SettingsView::set_banner(std::string text, Tone tone) {
    banner_text_ = text;
    banner_label_->set_text(std::move(text));
    banner_->set_visible(!banner_text_.empty());
    if (Window* w = window()) {
        const Color c = tone_color(w->theme(), tone);
        banner_->set_background(c.with_alpha(0.14f), w->theme().radius_control);
        banner_->set_border(c.with_alpha(0.5f));
    }
}

std::string SettingsView::describe() const { return title_->text(); }

EventResult SettingsView::on_key(const KeyEvent& k) {
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    using K = platform::Key;
    if (k.key == K::escape) {
        // A search query is cleared first, as in Macterm's search fields.
        if (!search_->text().empty() && search_->field().focused()) {
            search_->set_text("");
            if (on_search) on_search("");
            return EventResult::Handled;
        }
        // A window of its own stays open on Escape, like other settings windows.
        if (windowed_) return EventResult::Ignored;
        if (on_close) on_close();
        return EventResult::Handled;
    }
    // Cmd+W / Ctrl+W: closes the Settings window.
    if (windowed_ && (k.mods & primary_mod()) && k.key == K::w) {
        if (on_close) on_close();
        return EventResult::Handled;
    }
    if (k.key == K::tab && (k.mods & (platform::mod_ctrl | platform::mod_super)) == 0) {
        if (Window* w = window()) w->focus_next((k.mods & platform::mod_shift) != 0);
        return EventResult::Handled;
    }
    // Cmd/Ctrl+F: the search field.
    if ((k.mods & primary_mod()) && k.key == K::f) {
        if (Window* w = window()) {
            w->set_focus(&search_->field(), true);
            search_->field().select_all();
        }
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void SettingsView::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    if (windowed_) {
        p.dl.rect(b, t.bg);
        p.dl.rect(Rect{0, 0, 200, b.h}, t.sidebar_bg);
        p.dl.hairline(Rect{200, 0, 1, b.h}, t.border);
        return;
    }
    p.dl.shadow(b.translated({0, 6}), t.radius_panel, 18, t.shadow);
    p.dl.round_rect(b, t.radius_panel, t.bg, 1, t.border);
    // The sidebar: Macterm's sidebar material, a touch lighter than the window.
    p.dl.push_clip(b, t.radius_panel);
    p.dl.rect(Rect{0, 0, 200, b.h}, t.sidebar_bg);
    p.dl.hairline(Rect{200, 0, 1, b.h}, t.border);
    p.dl.pop_clip();
}

WidgetId show_settings(Window& w, std::unique_ptr<SettingsView> v, std::function<void()> closed) {
    w.close_transient();
    SettingsView* raw = v.get();
    OverlayOptions o;
    o.modal = true;
    o.placement = OverlayOptions::Placement::Center;
    o.dismiss_on_outside_click = false;
    o.dismiss_on_escape = false;
    o.transient = false;
    o.own_keys = true;
    o.margin = 16;
    o.on_close = std::move(closed);
    const WidgetId id = w.show_overlay(std::move(v), o);
    w.set_focus(&raw->sections(), false);
    return id;
}

}  // namespace kt::ui
