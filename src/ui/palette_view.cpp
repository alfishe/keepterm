#include "ui/palette_view.h"

#include <algorithm>
#include <cmath>

#include "ui/window.h"

namespace kt::ui {

namespace {

constexpr float kRowH = 30, kDetailRowH = 48, kHeaderH = 26, kMaxListH = 380, kWidth = 640;

float px_round(float v, float scale) { return std::round(v * scale) / scale; }

}  // namespace

// The rows: virtual (only the visible ones are shaped and painted), headers between sections.
class PaletteList final : public Widget {
public:
    PaletteList() : Widget("results") { style().clip = true; }

    struct Entry {
        bool header = false;
        size_t row = 0;          // index into rows_ (rows), unused for headers
        std::string text;        // header text
        float y = 0, h = 0;
    };

    void set(std::vector<PaletteRow> rows, std::string empty_text) {
        rows_ = std::move(rows);
        empty_ = std::move(empty_text);
        entries_.clear();
        float y = 0;
        std::string section;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (!rows_[i].section.empty() && (i == 0 || rows_[i].section != section)) {
                entries_.push_back(Entry{true, 0, rows_[i].section, y, kHeaderH});
                y += kHeaderH;
            }
            section = rows_[i].section;
            const float h = rows_[i].detail.empty() ? kRowH : kDetailRowH;
            entries_.push_back(Entry{false, i, {}, y, h});
            y += h;
        }
        content_h_ = y;
        offset_ = 0;
        selected_.reset();
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].enabled) {
                selected_ = i;
                break;
            }
        }
        if (!selected_ && !rows_.empty()) selected_ = 0;
        invalidate_layout();
        invalidate_paint();
    }
    const std::vector<PaletteRow>& rows() const { return rows_; }
    std::optional<size_t> selected() const { return selected_; }

    void select(size_t i) {
        if (i >= rows_.size()) return;
        selected_ = i;
        reveal(i);
        invalidate_paint();
        if (on_selection) on_selection(i);
    }
    void move(int delta) {
        if (rows_.empty()) return;
        const auto n = static_cast<int>(rows_.size());
        int i = selected_ ? static_cast<int>(*selected_) : (delta > 0 ? -1 : n);
        for (int step = 0; step < n; ++step) {
            i = ((i + delta) % n + n) % n;
            if (rows_[static_cast<size_t>(i)].enabled) break;
        }
        select(static_cast<size_t>(i));
    }
    void page(int dir) {
        const int per = std::max(1, static_cast<int>(frame().h / kRowH) - 1);
        if (rows_.empty()) return;
        int i = selected_ ? static_cast<int>(*selected_) : 0;
        i = std::clamp(i + dir * per, 0, static_cast<int>(rows_.size()) - 1);
        select(static_cast<size_t>(i));
    }

    std::function<void(size_t)> on_selection;
    std::function<void(size_t)> on_click;

    void paint(PaintContext& p) const override {
        const Theme& t = p.theme;
        const Rect b = local_bounds();
        if (rows_.empty()) {
            if (empty_.empty()) return;
            auto run = p.text.shape(empty_, role_for(t, TextStyle::body), p.scale);
            p.dl.text(run, {px_round((b.w - run->width()) / 2, p.scale), px_round(14 + run->ascent(), p.scale)}, t.fg_muted);
            return;
        }
        const FontRole title_role = role_for(t, TextStyle::body);
        const FontRole sub_role = role_for(t, TextStyle::small);
        const FontRole key_role = role_for(t, TextStyle::mono_small);
        const FontRole head_role = role_for(t, TextStyle::small_bold);
        for (const Entry& e : entries_) {
            const float y = e.y - offset_;
            if (y + e.h < 0 || y > b.h) continue;
            if (e.header) {
                auto run = p.text.shape(e.text, head_role, p.scale);
                p.dl.text(run, {14, px_round(y + e.h - 7 - run->height() + run->ascent(), p.scale)}, t.fg_dim);
                continue;
            }
            const PaletteRow& r = rows_[e.row];
            const Rect row{6, y, b.w - 12, e.h};
            const float lh = r.detail.empty() ? e.h : kRowH;   // the first line
            const bool sel = selected_ && *selected_ == e.row;
            if (sel) p.dl.round_rect(row, t.radius_control, t.selection);
            else if (hover_ && *hover_ == e.row) p.dl.round_rect(row, t.radius_control, t.fg.with_alpha(0.05f));
            const Color fg = r.enabled ? t.fg : t.fg_dim;
            float x = 16;
            const Icon icon = r.checked ? Icon::check : r.icon;
            if (icon != Icon::none) {
                p.dl.icon(icon, 14, {x, px_round(y + (lh - 14) / 2, p.scale)}, r.enabled ? t.fg_muted : t.fg_dim);
            }
            x += 22;
            // Right side first: key caps or the scope chevron, then the title gets the rest.
            float right = row.right() - 10;
            if (r.opens_scope) {
                p.dl.icon(Icon::chevron_right, 12, {right - 12, px_round(y + (lh - 12) / 2, p.scale)}, t.fg_dim);
                right -= 18;
            }
            if (!r.keys.empty()) {
                auto kr = p.text.shape(r.keys, key_role, p.scale);
                const float kw = kr->width() + 10, kh = 18;
                const Rect cap{right - kw, px_round(y + (lh - kh) / 2, p.scale), kw, kh};
                p.dl.round_rect(cap, t.radius_keycap, t.fg.with_alpha(0.06f), 1, t.border);
                p.dl.text(kr, {cap.x + 5, px_round(cap.y + (kh - kr->height()) / 2 + kr->ascent(), p.scale)}, t.fg_muted);
                right = cap.x - 10;
            }
            const float avail = std::max(0.0f, right - x);
            auto tr = p.text.ellipsize(r.title, title_role, p.scale, avail);
            const float base = px_round(y + (lh - tr->height()) / 2 + tr->ascent(), p.scale);
            // Matched characters: a soft accent behind the glyphs.
            if (!r.highlights.empty() && r.enabled) {
                for (uint32_t h : r.highlights) {
                    for (size_t g = 0; g < tr->glyphs.size(); ++g) {
                        if (tr->glyphs[g].cluster != h) continue;
                        const float gx = tr->glyphs[g].x / p.scale;
                        const float gx1 = g + 1 < tr->glyphs.size() ? tr->glyphs[g + 1].x / p.scale : tr->width();
                        p.dl.round_rect(Rect{x + gx - 0.5f, base - tr->ascent() - 1, gx1 - gx + 1, tr->height() + 2}, 2,
                                        t.accent.with_alpha(0.28f));
                        break;
                    }
                }
            }
            p.dl.text(tr, {x, base}, fg);
            const std::string& sub = r.enabled ? r.subtitle : (r.disabled_reason.empty() ? r.subtitle : r.disabled_reason);
            if (!sub.empty() && tr->width() + 16 < avail) {
                auto sr = p.text.ellipsize(sub, sub_role, p.scale, avail - tr->width() - 12);
                p.dl.text(sr, {x + tr->width() + 10, px_round(y + (lh - sr->height()) / 2 + sr->ascent(), p.scale)}, t.fg_dim);
            }
            if (!r.detail.empty()) {
                const FontRole mono = role_for(t, TextStyle::mono_small);
                paint_marked(p, r.detail, r.detail_marks, mono, x, px_round(y + kRowH - 6, p.scale), row.right() - 10 - x,
                             r.enabled ? t.fg_muted : t.fg_dim);
            }
        }
        // Scroll indicator.
        if (content_h_ > b.h + 1) {
            const float th = std::max(24.0f, b.h * b.h / content_h_);
            const float ty = (b.h - th) * (offset_ / (content_h_ - b.h));
            p.dl.round_rect(Rect{b.w - 5, ty + 2, 3, th - 4}, 1.5f, t.fg.with_alpha(0.25f));
        }
    }

    EventResult on_wheel(WheelEvent& e) override {
        const float max_off = std::max(0.0f, content_h_ - frame().h);
        offset_ = std::clamp(offset_ - e.dy * (e.precise ? 1.0f : kRowH), 0.0f, max_off);
        invalidate_paint();
        return EventResult::Handled;
    }
    EventResult on_pointer(PointerEvent& e) override {
        const std::optional<size_t> at = row_at(e.local.y);
        if (e.type == PointerEvent::Type::Move || e.type == PointerEvent::Type::Enter) {
            if (at != hover_) {
                hover_ = at;
                invalidate_paint();
            }
            return EventResult::Handled;
        }
        if (e.type == PointerEvent::Type::Leave) {
            hover_.reset();
            invalidate_paint();
            return EventResult::Handled;
        }
        if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left && at) {
            select(*at);
            if (on_click) on_click(*at);
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }

protected:
    Size intrinsic_size(Size) const override {
        return {kWidth, rows_.empty() ? (empty_.empty() ? 0.0f : 44.0f) : std::min(kMaxListH, content_h_ + 4)};
    }

private:
    std::optional<size_t> row_at(float y) const {
        for (const Entry& e : entries_) {
            if (!e.header && y + offset_ >= e.y && y + offset_ < e.y + e.h) return e.row;
        }
        return std::nullopt;
    }
    void reveal(size_t row) {
        for (const Entry& e : entries_) {
            if (e.header || e.row != row) continue;
            const float h = frame().h > 0 ? frame().h : kMaxListH;
            // Keep the section header of the first row visible.
            const float top = e.row == 0 ? 0.0f : e.y;
            if (top < offset_) offset_ = top;
            if (e.y + e.h > offset_ + h) offset_ = e.y + e.h - h;
            break;
        }
    }

    std::vector<PaletteRow> rows_;
    std::vector<Entry> entries_;
    std::string empty_;
    std::optional<size_t> selected_, hover_;
    float offset_ = 0, content_h_ = 0;
};

PaletteView::PaletteView(std::string placeholder) : Widget("palette") {
    style().width = kWidth;
    style().padding = Insets{0, 0, 0, 6};
    auto top = row("top", 8);
    top->style().height = 46.0f;
    top->style().padding = Insets{14, 0, 14, 0};
    top->style().align_items = LayoutStyle::Align::Center;
    top->emplace<IconView>(Icon::search, 16.0f, Tone::dim);
    scope_ = &top->emplace<Label>("", TextStyle::small_bold, Tone::accent, "scope");
    scope_->set_visible(false);
    field_ = &top->emplace<TextField>(std::move(placeholder), "field");
    field_->set_frameless(true);
    field_->set_text_style(TextStyle::title);
    field_->style().grow = 1;
    field_->style().shrink = 1;
    field_->style().basis = 0.0f;
    field_->on_change = [this](const std::string& q) {
        if (on_query) on_query(q);
    };
    add(std::move(top));
    auto sep = std::make_unique<Box>("sep");
    sep->style().height = 1.0f;
    sep->style().margin = Insets{0, 0, 0, 4};
    sep->set_background(Color{});
    add(std::move(sep));
    list_ = &emplace<PaletteList>();
    list_->on_selection = [this](size_t i) {
        if (on_selection) on_selection(i);
    };
    list_->on_click = [this](size_t i) {
        if (on_accept) on_accept(i, alt_);
    };
}

void PaletteView::set_rows(std::vector<PaletteRow> rows, std::string empty_text) {
    list_->set(std::move(rows), std::move(empty_text));
    if (on_selection && list_->selected()) on_selection(*list_->selected());
}

const std::vector<PaletteRow>& PaletteView::rows() const { return list_->rows(); }

void PaletteView::set_scope(std::string name) {
    scope_->set_visible(!name.empty());
    scope_->set_text(std::move(name));
}

std::optional<size_t> PaletteView::selected() const { return list_->selected(); }
void PaletteView::select(size_t row) { list_->select(row); }
void PaletteView::move(int delta) { list_->move(delta); }
void PaletteView::page(int dir) { list_->page(dir); }

void PaletteView::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    p.dl.shadow(Rect{b.x, b.y + 8, b.w, b.h}, t.radius_panel, 18, t.shadow);
    p.dl.round_rect(b, t.radius_panel, over(t.fg.with_alpha(t.scheme == Scheme::dark ? 0.07f : 0.0f), t.bg), 1,
                    t.fg.with_alpha(t.scheme == Scheme::dark ? 0.16f : 0.14f));
    if (child_count() > 1) {
        const Rect s = child(1)->frame();
        p.dl.hairline(Rect{0, s.y, b.w, 1}, t.border);
    }
}

EventResult PaletteView::on_key(const KeyEvent& k) {
    using K = platform::Key;
    alt_ = (k.mods & platform::mod_alt) != 0;
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    const bool ctrl = (k.mods & platform::mod_ctrl) != 0;
    switch (k.key) {
        case K::down: move(1); return EventResult::Handled;
        case K::up: move(-1); return EventResult::Handled;
        case K::n:
            if (!ctrl) return EventResult::Ignored;
            move(1);
            return EventResult::Handled;
        case K::p:
            if (!ctrl) return EventResult::Ignored;
            move(-1);
            return EventResult::Handled;
        case K::page_down: page(1); return EventResult::Handled;
        case K::page_up: page(-1); return EventResult::Handled;
        case K::enter:
        case K::kp_enter:
            if (selected() && on_accept) on_accept(*selected(), alt_);
            return EventResult::Handled;
        case K::tab:
            if (selected() && *selected() < rows().size() && rows()[*selected()].opens_scope && on_enter_scope)
                on_enter_scope(*selected());
            return EventResult::Handled;
        case K::escape:
            if (on_close) on_close();
            return EventResult::Handled;
        case K::backspace:
            if (field_->text().empty() && k.action == platform::KeyAction::press && on_pop_scope) {
                (void)on_pop_scope();
                return EventResult::Handled;
            }
            return EventResult::Ignored;
        default: return EventResult::Ignored;
    }
}

std::string PaletteView::describe() const {
    std::string s;
    for (const auto& r : rows()) {
        if (!s.empty()) s += "|";
        s += r.title;
    }
    return s;
}

WidgetId show_palette(Window& w, std::unique_ptr<PaletteView> v, std::function<void()> closed) {
    w.close_transient();
    PaletteView* raw = v.get();
    OverlayOptions o;
    o.placement = OverlayOptions::Placement::TopCenter;
    o.anchor = Rect{0, 52, 0, 0};
    o.own_keys = true;
    o.dismiss_on_escape = false;
    o.transient = true;
    o.margin = 12;
    o.on_close = std::move(closed);
    const WidgetId id = w.show_overlay(std::move(v), o);
    w.set_focus(&raw->field(), true);
    return id;
}

}  // namespace kt::ui
