#include "ui/widgets.h"

#include <algorithm>
#include <cmath>

namespace kt::ui {

namespace {

constexpr auto kHoverIn = std::chrono::milliseconds(90);
constexpr auto kHoverOut = std::chrono::milliseconds(160);

Transition hover_tr(bool in) { return Transition::curve(in ? kHoverIn : kHoverOut, Easing::ease_out(), false); }

float px_round(float v, float scale) { return std::round(v * scale) / scale; }

bool is_word(char32_t c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }

}  // namespace

FontRole role_for(const Theme& t, TextStyle s) {
    switch (s) {
        case TextStyle::body: return t.body();
        case TextStyle::body_bold: return t.body_bold();
        case TextStyle::small: return t.small();
        case TextStyle::small_bold: return t.small_bold();
        case TextStyle::title: return t.title();
        case TextStyle::large_title: return t.large_title();
        case TextStyle::mono_small: return t.mono_small();
        case TextStyle::mono_body: return {13 * t.font_scale, false, true};
    }
    return t.body();
}

Color tone_color(const Theme& t, Tone tone) {
    switch (tone) {
        case Tone::normal: return t.fg;
        case Tone::muted: return t.fg_muted;
        case Tone::dim: return t.fg_dim;
        case Tone::accent: return t.accent;
        case Tone::success: return t.success;
        case Tone::warning: return t.warning;
        case Tone::failure: return t.failure;
        case Tone::on_accent: return t.on_accent;
    }
    return t.fg;
}

void paint_focus_ring(PaintContext& p, Rect r, float radius) {
    p.dl.round_rect(r.inflate(3), radius + 3, {}, 2, p.theme.accent.with_alpha(0.75f));
}

// ---------------------------------------------------------------- Label

Label::Label(std::string text, TextStyle style, Tone tone, std::string id)
    : Widget(std::move(id)), text_(std::move(text)), style_(style), tone_(tone) {
    set_hit_testable(false);
}

void Label::set_text(std::string t) {
    if (t == text_) return;
    text_ = std::move(t);
    full_.reset();
    shown_.reset();
    invalidate_layout();
}

void Label::on_text_metrics_changed() {
    full_.reset();
    shown_.reset();
    lines_.clear();
    shown_w_ = -1;
    Widget::on_text_metrics_changed();
}

void Label::set_text_style(TextStyle s) {
    if (s == style_) return;
    style_ = s;
    full_.reset();
    shown_.reset();
    invalidate_layout();
}

void Label::set_tone(Tone t) {
    if (t == tone_) return;
    tone_ = t;
    invalidate_paint();
}

void Label::set_color(std::optional<Color> c) {
    if (c == color_) return;
    color_ = c;
    invalidate_paint();
}

void Label::set_elide(Elide e) {
    elide_ = e;
    shown_.reset();
    invalidate_paint();
}

void Label::set_align(Align a) {
    align_ = a;
    invalidate_paint();
}

void Label::set_wrap(bool w) {
    wrap_ = w;
    invalidate_layout();
}

bool Label::truncated() const { return full_ && shown_ && shown_ != full_; }

std::vector<TextRunPtr> Label::wrap_lines(float width) const {
    std::vector<TextRunPtr> out;
    Window* win = window();
    if (!win) return out;
    const FontRole role = role_for(win->theme(), style_);
    ITextService& tx = win->text();
    const float s = win->scale();
    size_t start = 0;
    while (start <= text_.size()) {
        const size_t nl = text_.find('\n', start);
        const std::string_view para(text_.data() + start, (nl == std::string::npos ? text_.size() : nl) - start);
        // Greedy over spaces.
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            if (sp == std::string_view::npos) sp = para.size();
            const std::string_view word = para.substr(i, sp - i);
            std::string cand = line.empty() ? std::string(word) : line + " " + std::string(word);
            if (!line.empty() && tx.shape(cand, role, s)->width() > width) {
                out.push_back(tx.shape(line, role, s));
                line.assign(word);
            } else {
                line = std::move(cand);
            }
            i = sp + 1;
        }
        out.push_back(tx.shape(line, role, s));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

Size Label::intrinsic_size(Size avail) const {
    Window* win = window();
    if (!win) return {};
    const FontRole role = role_for(win->theme(), style_);
    const FontMetrics m = win->text().metrics(role, win->scale());
    line_h_ = std::ceil(m.line * win->scale()) / win->scale();
    full_ = win->text().shape(text_, role, win->scale());
    if (wrap_ && avail.w < kInf) {
        lines_ = wrap_lines(avail.w);
        float w = 0;
        for (const auto& l : lines_) w = std::max(w, l->width());
        return {std::ceil(w), line_h_ * static_cast<float>(std::max<size_t>(1, lines_.size()))};
    }
    float w = full_->width();
    for (const auto& x : samples_) w = std::max(w, win->text().shape(x, role, win->scale())->width());
    return {std::ceil(w), line_h_};
}

void Label::set_width_samples(std::vector<std::string> s) {
    samples_ = std::move(s);
    invalidate_layout();
}

void Label::arrange(const Rect& f) {
    if (!(f == frame()) || layout_dirty()) shown_w_ = -1;
    Widget::arrange(f);
}

void Label::paint(PaintContext& p) const {
    if (text_.empty()) return;
    const FontRole role = role_for(p.theme, style_);
    const Rect b = local_bounds();
    const Color c = (color_ ? *color_ : tone_color(p.theme, tone_)).fade(enabled() ? 1.0f : 0.45f);
    if (!full_) full_ = p.text.shape(text_, role, p.scale);
    if (wrap_) {
        if (lines_.empty()) lines_ = wrap_lines(b.w);
        float y = 0;
        for (const auto& l : lines_) {
            const float base = px_round(y + (line_h_ - l->height()) / 2 + l->ascent(), p.scale);
            float x = 0;
            if (align_ == Align::center) x = px_round((b.w - l->width()) / 2, p.scale);
            else if (align_ == Align::end) x = b.w - l->width();
            p.dl.text(l, {x, base}, c);
            y += line_h_;
        }
        return;
    }
    if (shown_w_ != b.w || !shown_) {
        shown_w_ = b.w;
        if (elide_ == Elide::none || full_->width() <= b.w + 0.5f) shown_ = full_;
        else shown_ = p.text.ellipsize(text_, role, p.scale, b.w, elide_ == Elide::middle);
    }
    const TextRunPtr& run = shown_;
    float x = 0;
    if (align_ == Align::center) x = px_round((b.w - run->width()) / 2, p.scale);
    else if (align_ == Align::end) x = px_round(b.w - run->width(), p.scale);
    const float base = px_round((b.h - run->height()) / 2 + run->ascent(), p.scale);
    if (elide_ == Elide::none && run->width() > b.w + 0.5f) {
        p.dl.push_clip(b);
        p.dl.text(run, {x, base}, c);
        p.dl.pop_clip();
    } else {
        p.dl.text(run, {x, base}, c);
    }
}

// ---------------------------------------------------------------- IconView

IconView::IconView(Icon icon, float size, Tone tone, std::string id)
    : Widget(std::move(id)), icon_(icon), size_(size), tone_(tone) {
    set_hit_testable(false);
}

void IconView::set_icon(Icon i) {
    if (i == icon_) return;
    icon_ = i;
    invalidate_paint();
}

void IconView::set_tone(Tone t) {
    if (t == tone_) return;
    tone_ = t;
    invalidate_paint();
}

void IconView::set_color(std::optional<Color> c) {
    if (c == color_) return;
    color_ = c;
    invalidate_paint();
}

void IconView::set_color_fn(std::function<Color(const Theme&)> f) {
    color_fn_ = std::move(f);
    invalidate_paint();
}

void IconView::set_size(float s) {
    if (s == size_) return;
    size_ = s;
    invalidate_layout();
}

void IconView::paint(PaintContext& p) const {
    if (icon_ == Icon::none) return;
    const Rect b = local_bounds();
    const Color c = (color_fn_ ? color_fn_(p.theme) : color_ ? *color_ : tone_color(p.theme, tone_)).fade(enabled() ? 1.0f : 0.45f);
    p.dl.icon(icon_, size_, {px_round((b.w - size_) / 2, p.scale), px_round((b.h - size_) / 2, p.scale)}, c);
}

// ---------------------------------------------------------------- Button

Button::Button(std::string label, std::function<void()> on_click, Kind kind, std::string id)
    : Widget(id.empty() ? "button[" + label + "]" : std::move(id)),
      label_(std::move(label)),
      on_click_(std::move(on_click)),
      kind_(kind),
      hover_(*this),
      press_(*this) {}

std::unique_ptr<Button> Button::icon_button(Icon icon, std::string tooltip, std::function<void()> on_click, std::string id,
                                            float size) {
    auto b = std::make_unique<Button>("", std::move(on_click), Kind::icon, std::move(id));
    b->icon_ = icon;
    b->icon_size_ = std::round(size * 0.66f);
    b->square_ = size;
    b->set_tooltip(std::move(tooltip));
    return b;
}

void Button::set_label(std::string l) {
    if (l == label_) return;
    label_ = std::move(l);
    run_.reset();
    invalidate_layout();
}

void Button::set_icon(Icon i, float size) {
    icon_ = i;
    icon_size_ = size;
    invalidate_layout();
}

void Button::set_kind(Kind k) {
    kind_ = k;
    invalidate_paint();
}

void Button::set_checked(bool c) {
    if (c == checked_) return;
    checked_ = c;
    invalidate_paint();
}

void Button::click() {
    if (!enabled()) return;
    if (on_click_) {
        auto f = on_click_;   // the handler may destroy this button
        f();
    }
}

std::string Button::describe() const {
    std::string s = label_.empty() ? std::string(icon_name(icon_)) : label_;
    if (checked_) s += " checked";
    if (!enabled()) s += " disabled";
    return s;
}

Size Button::intrinsic_size(Size) const {
    if (square_ > 0) return {square_, square_};
    Window* win = window();
    const float h = win ? win->theme().control_height : 24;
    float w = 24;
    if (win && !label_.empty()) {
        run_ = win->text().shape(label_, role_for(win->theme(), TextStyle::body), win->scale());
        w = std::ceil(run_->width()) + 24;
    }
    if (icon_ != Icon::none) w += icon_size_ + (label_.empty() ? 0 : 6);
    if (kind_ == Kind::primary || kind_ == Kind::secondary || kind_ == Kind::danger) w = std::max(w, 72.0f);
    return {w, h};
}

void Button::on_hover(bool inside) {
    hover_.set(inside ? 1.0f : 0.0f, hover_tr(inside));
    invalidate_paint();
}

EventResult Button::on_pointer(PointerEvent& e) {
    if (e.button != platform::MouseButton::left && e.type != PointerEvent::Type::Move) return EventResult::Ignored;
    const bool inside = local_bounds().contains(e.local);
    switch (e.type) {
        case PointerEvent::Type::Press:
            pressed_ = true;
            press_.set(1, Transition::curve(std::chrono::milliseconds(60), Easing::ease_out()));
            return EventResult::Handled;
        case PointerEvent::Type::Move:
            if (e.captured && pressed_ != inside) {
                pressed_ = inside;
                press_.set(inside ? 1.0f : 0.0f, Transition::curve(std::chrono::milliseconds(60), Easing::ease_out()));
            }
            return e.captured ? EventResult::Handled : EventResult::Ignored;
        case PointerEvent::Type::Release: {
            const bool fire = pressed_ && inside;
            pressed_ = false;
            press_.set(0, Transition::curve(std::chrono::milliseconds(120), Easing::ease_out()));
            if (fire) click();
            return EventResult::Handled;
        }
        default: return EventResult::Ignored;
    }
}

EventResult Button::on_key(const KeyEvent& k) {
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    if (k.key == platform::Key::enter || k.key == platform::Key::space || k.key == platform::Key::kp_enter) {
        click();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void Button::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float h = hover_.value(), pr = press_.value();
    const float fade = enabled() ? 1.0f : 0.45f;
    const float radius = kind_ == Kind::icon ? 5 : t.radius_control;
    Color fill{}, border{}, fg = t.fg;
    switch (kind_) {
        case Kind::primary:
            fill = over(Color::rgb(0xFFFFFF, 0.10f * h), t.accent);
            if (pr > 0) fill = over(Color::rgb(0x000000, 0.12f * pr), fill);
            fg = t.on_accent;
            break;
        case Kind::danger:
            fill = over(Color::rgb(0xFFFFFF, 0.10f * h), t.failure);
            if (pr > 0) fill = over(Color::rgb(0x000000, 0.12f * pr), fill);
            fg = Color::rgb(0xFFFFFF);
            break;
        case Kind::secondary:
            fill = over(t.fg.with_alpha(0.05f * h + 0.07f * pr), t.surface_raised);
            border = t.border;
            break;
        case Kind::plain:
        case Kind::icon:
            fill = t.fg.with_alpha(0.07f * h + 0.07f * pr + (checked_ ? 0.10f : 0.0f));
            fg = checked_ ? t.fg : over(t.fg.with_alpha(0.35f * h), t.fg_muted);
            break;
    }
    if (fill.a > 0 || border.a > 0) p.dl.round_rect(b, radius, fill.fade(fade), border.a > 0 ? 1.0f : 0.0f, border.fade(fade));
    float content_w = 0;
    if (icon_ != Icon::none) content_w += icon_size_;
    if (run_ && !label_.empty()) content_w += run_->width() + (icon_ != Icon::none ? 6 : 0);
    float x = align_start_ ? 0.0f : px_round((b.w - content_w) / 2, p.scale);
    if (icon_ != Icon::none) {
        p.dl.icon(icon_, icon_size_, {x, px_round((b.h - icon_size_) / 2, p.scale)}, fg.fade(fade));
        x += icon_size_ + 6;
    }
    if (run_ && !label_.empty()) {
        p.dl.text(run_, {x, px_round((b.h - run_->height()) / 2 + run_->ascent(), p.scale)}, fg.fade(fade));
    }
    if (focus_visible()) paint_focus_ring(p, b, radius);
}

// ---------------------------------------------------------------- Toggle

Toggle::Toggle(bool on, std::function<void(bool)> on_change, std::string id)
    : Widget(std::move(id)), on_(on), on_change_(std::move(on_change)), knob_(*this, on ? 1.0f : 0.0f) {}

void Toggle::set_on(bool on, bool animate) {
    if (on == on_) return;
    on_ = on;
    if (animate) knob_.set(on ? 1.0f : 0.0f, Transition::spring(std::chrono::milliseconds(220)));
    else knob_.jump(on ? 1.0f : 0.0f);
    invalidate_paint();
}

EventResult Toggle::on_pointer(PointerEvent& e) {
    if (e.button != platform::MouseButton::left) return EventResult::Ignored;
    if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
    if (e.type == PointerEvent::Type::Release && local_bounds().contains(e.local)) {
        set_on(!on_);
        if (on_change_) on_change_(on_);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult Toggle::on_key(const KeyEvent& k) {
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    // Space flips a switch; Enter goes on to the dialog's default button (platform convention).
    if (k.key == platform::Key::space) {
        set_on(!on_);
        if (on_change_) on_change_(on_);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void Toggle::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float k = knob_.value();
    const float fade = enabled() ? 1.0f : 0.45f;
    const Color track = lerp(over(t.fg.with_alpha(0.18f), t.bg), t.accent, k);
    p.dl.round_rect(b, b.h / 2, track.fade(fade));
    const float d = b.h - 4;
    const float x = 2 + (b.w - 4 - d) * k;
    p.dl.shadow(Rect{x, 2.5f, d, d}, d / 2, 1, Color::rgb(0x000000, 0.25f * fade));
    p.dl.round_rect(Rect{x, 2, d, d}, d / 2, Color::rgb(0xFFFFFF, fade));
    if (focus_visible()) paint_focus_ring(p, b, b.h / 2);
}

// ---------------------------------------------------------------- TextField

TextField::TextField(std::string placeholder, std::string id) : Widget(std::move(id)), placeholder_(std::move(placeholder)) {}

void TextField::set_text(std::string t) {
    text_ = std::move(t);
    caret_ = anchor_ = text_.size();
    run_.reset();
    invalidate_paint();
}

void TextField::set_placeholder(std::string p) {
    placeholder_ = std::move(p);
    invalidate_paint();
}

void TextField::select_all() {
    anchor_ = 0;
    caret_ = text_.size();
    invalidate_paint();
}

std::pair<size_t, size_t> TextField::selection() const { return {std::min(anchor_, caret_), std::max(anchor_, caret_)}; }

std::string TextField::display() const {
    if (preedit_.empty()) return text_;
    std::string d = text_.substr(0, caret_);
    d += preedit_;
    d += text_.substr(caret_);
    return d;
}

void TextField::reshape() const {
    Window* win = window();
    if (!win) return;
    run_ = win->text().shape(display(), role_for(win->theme(), style_), win->scale());
}

float TextField::x_of(size_t byte) const {
    if (!run_) reshape();
    if (!run_) return 0;
    for (const ShapedGlyph& g : run_->glyphs) {
        if (g.cluster >= byte) return g.x / run_->scale;
    }
    return run_->width();
}

size_t TextField::byte_at(float x) const {
    const std::string d = display();
    size_t best = 0;
    float best_d = kInf;
    for (size_t i = 0;; i = utf8_next(d, i)) {
        const float dx = std::abs(x_of(i) - x);
        if (dx < best_d) {
            best_d = dx;
            best = i;
        }
        if (i >= d.size()) break;
    }
    return std::min(best, text_.size());
}

void TextField::changed() {
    run_.reset();
    invalidate_paint();
    if (on_change) on_change(text_);
}

void TextField::erase_selection() {
    auto [a, b] = selection();
    if (a == b) return;
    text_.erase(a, b - a);
    caret_ = anchor_ = a;
}

void TextField::insert(std::string_view s) {
    std::string clean;
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) clean.push_back(c);
    }
    if (clean.empty()) return;
    erase_selection();
    text_.insert(caret_, clean);
    caret_ += clean.size();
    anchor_ = caret_;
    changed();
}

void TextField::move_caret(size_t to, bool extend) {
    caret_ = std::min(to, text_.size());
    if (!extend) anchor_ = caret_;
    invalidate_paint();
}

Size TextField::intrinsic_size(Size) const {
    Window* win = window();
    const float h = win ? std::max(win->theme().control_height + 2, 26.0f) : 26;
    return {160, h};
}

EventResult TextField::on_pointer(PointerEvent& e) {
    const float x = e.local.x - 8 + scroll_;
    if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left) {
        if (e.clicks >= 2) {
            // Word under the pointer.
            size_t at = byte_at(x), a = at, b = at;
            while (a > 0 && is_word(utf8_decode(text_, utf8_prev(text_, a)))) a = utf8_prev(text_, a);
            while (b < text_.size() && is_word(utf8_decode(text_, b))) b = utf8_next(text_, b);
            anchor_ = a;
            caret_ = b;
            if (e.clicks >= 3) select_all();
            invalidate_paint();
        } else {
            move_caret(byte_at(x), (e.mods & platform::mod_shift) != 0);
        }
        dragging_ = true;
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Move && dragging_ && e.captured) {
        move_caret(byte_at(x), true);
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Release) {
        dragging_ = false;
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult TextField::on_key(const KeyEvent& k) {
    using K = platform::Key;
    if (k.action == platform::KeyAction::release) return EventResult::Ignored;
    const bool shift = (k.mods & platform::mod_shift) != 0;
    const bool word = (k.mods & (platform::mod_alt | (primary_mod() == platform::mod_ctrl ? platform::mod_ctrl : 0))) != 0;
    const bool prim = (k.mods & primary_mod()) != 0;
    auto prev_word = [&](size_t i) {
        while (i > 0 && !is_word(utf8_decode(text_, utf8_prev(text_, i)))) i = utf8_prev(text_, i);
        while (i > 0 && is_word(utf8_decode(text_, utf8_prev(text_, i)))) i = utf8_prev(text_, i);
        return i;
    };
    auto next_word = [&](size_t i) {
        while (i < text_.size() && !is_word(utf8_decode(text_, i))) i = utf8_next(text_, i);
        while (i < text_.size() && is_word(utf8_decode(text_, i))) i = utf8_next(text_, i);
        return i;
    };
    switch (k.key) {
        case K::left:
            if (!shift && anchor_ != caret_) move_caret(selection().first, false);
            else if (prim && primary_mod() == platform::mod_super) move_caret(0, shift);
            else move_caret(word ? prev_word(caret_) : utf8_prev(text_, caret_), shift);
            return EventResult::Handled;
        case K::right:
            if (!shift && anchor_ != caret_) move_caret(selection().second, false);
            else if (prim && primary_mod() == platform::mod_super) move_caret(text_.size(), shift);
            else move_caret(word ? next_word(caret_) : utf8_next(text_, caret_), shift);
            return EventResult::Handled;
        case K::home: move_caret(0, shift); return EventResult::Handled;
        case K::end: move_caret(text_.size(), shift); return EventResult::Handled;
        case K::backspace:
            if (text_.empty()) return EventResult::Ignored;   // nothing to delete: the parent may use it
            if (anchor_ == caret_) anchor_ = word ? prev_word(caret_) : utf8_prev(text_, caret_);
            erase_selection();
            changed();
            return EventResult::Handled;
        case K::delete_:
            if (anchor_ == caret_) anchor_ = word ? next_word(caret_) : utf8_next(text_, caret_);
            erase_selection();
            changed();
            return EventResult::Handled;
        case K::enter:
        case K::kp_enter:
            if (on_submit) {
                on_submit(text_);
                return EventResult::Handled;
            }
            return EventResult::Ignored;
        case K::escape:
            if (on_cancel) {
                on_cancel();
                return EventResult::Handled;
            }
            return EventResult::Ignored;
        default: break;
    }
    if (prim && (k.mods & (platform::mod_alt)) == 0) {
        Window* win = window();
        auto [a, b] = selection();
        switch (k.key) {
            case K::a: select_all(); return EventResult::Handled;
            case K::c:
                if (win && win->set_clipboard && a != b) win->set_clipboard(text_.substr(a, b - a));
                return EventResult::Handled;
            case K::x:
                if (win && win->set_clipboard && a != b) {
                    win->set_clipboard(text_.substr(a, b - a));
                    erase_selection();
                    changed();
                }
                return EventResult::Handled;
            case K::v:
                if (win && win->get_clipboard) {
                    std::string s = win->get_clipboard();
                    std::replace(s.begin(), s.end(), '\n', ' ');
                    insert(s);
                }
                return EventResult::Handled;
            default: break;
        }
    }
    if (on_other_key && on_other_key(k)) return EventResult::Handled;
    return EventResult::Ignored;
}

EventResult TextField::on_text(std::string_view s) {
    insert(s);
    return EventResult::Handled;
}

void TextField::ime_preedit(std::string_view text, std::span<const ImeSpan> spans, int) {
    if (preedit_.empty()) erase_selection();
    preedit_.assign(text);
    preedit_spans_.assign(spans.begin(), spans.end());
    run_.reset();
    invalidate_paint();
}

void TextField::ime_commit(std::string_view text) {
    preedit_.clear();
    preedit_spans_.clear();
    insert(text);
}

void TextField::ime_cancel() {
    preedit_.clear();
    preedit_spans_.clear();
    run_.reset();
    invalidate_paint();
}

Rect TextField::ime_caret_rect() const {
    const Rect b = bounds_in_window();
    const float x = b.x + 8 + x_of(caret_ + preedit_.size()) - scroll_;
    return {x, b.y + 4, 1, b.h - 8};
}

void TextField::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const bool f = focused();
    if (!frameless_) {
        p.dl.round_rect(b, t.radius_control, over(t.fg.with_alpha(0.04f), t.bg), 1, f ? t.accent.with_alpha(0.8f) : t.border);
    }
    reshape();
    const float inner_w = std::max(0.0f, b.w - 16);
    const float cx = x_of(caret_ + preedit_.size());
    if (cx - scroll_ > inner_w) scroll_ = cx - inner_w;
    if (cx - scroll_ < 0) scroll_ = cx;
    if (run_ && run_->width() - scroll_ < inner_w) scroll_ = std::max(0.0f, run_->width() - inner_w);
    const float x0 = 8 - scroll_;
    const float th = run_ ? run_->height() : 14;
    const float base = px_round((b.h - th) / 2 + (run_ ? run_->ascent() : 11), p.scale);
    p.dl.push_clip(Rect{4, 0, std::max(0.0f, b.w - 8), b.h});
    auto [sa, sb] = selection();
    if (sa != sb && preedit_.empty()) {
        const float xa = x_of(sa), xb = x_of(sb);
        p.dl.rect(Rect{x0 + xa, base - (run_ ? run_->ascent() : 11), xb - xa, th}, t.accent.with_alpha(f ? 0.35f : 0.18f));
    }
    if (text_.empty() && preedit_.empty()) {
        if (!placeholder_.empty()) {
            auto ph = p.text.shape(placeholder_, role_for(t, style_), p.scale);
            p.dl.text(ph, {8, base}, t.fg_dim);
        }
    } else if (run_) {
        p.dl.text(run_, {x0, base}, t.fg);
    }
    if (!preedit_.empty()) {
        const float pa = x_of(caret_), pb = x_of(caret_ + preedit_.size());
        p.dl.hairline(Rect{x0 + pa, base + 2, pb - pa, 1}, t.fg);
    }
    if (f) {
        const float x = px_round(x0 + cx, p.scale);
        p.dl.rect(Rect{x, base - (run_ ? run_->ascent() : 11), std::max(1.0f, 1.0f / p.scale * std::round(p.scale)), th},
                  t.accent);
    }
    p.dl.pop_clip();
}

// ---------------------------------------------------------------- ScrollView

ScrollView::ScrollView(std::unique_ptr<Widget> content, std::string id) : Widget(std::move(id)), bar_(*this) {
    style().clip = true;
    style().dir = LayoutStyle::Dir::Stack;
    if (content) add_child(std::move(content));
}

void ScrollView::set_content(std::unique_ptr<Widget> c) {
    clear_children();
    if (c) add_child(std::move(c));
}

void ScrollView::arrange(const Rect& f) {
    if (f == frame() && !layout_dirty()) return;
    assign_frame(f);
    Widget* c = content();
    if (!c) return;
    const Size cs = c->measure(Constraints{f.w, f.w, 0, kInf});
    content_h_ = cs.h;
    c->arrange(Rect{0, 0, f.w, std::max(cs.h, f.h)});
    scroll_to(offset_);
}

void ScrollView::scroll_to(float y) {
    const float max_off = std::max(0.0f, content_h_ - frame().h);
    const float o = std::clamp(snap(y), 0.0f, max_off);
    if (Widget* c = content()) c->set_translation({0, -o});
    if (o == offset_) return;
    offset_ = o;
    invalidate_paint();
}

void ScrollView::scroll_into_view(const Rect& r) {
    if (r.y < offset_) scroll_to(r.y);
    else if (r.bottom() > offset_ + frame().h) scroll_to(r.bottom() - frame().h);
}

Rect ScrollView::thumb() const {
    const Rect b = local_bounds();
    if (content_h_ <= b.h || b.h <= 0) return {};
    const float h = std::max(24.0f, b.h * b.h / content_h_);
    const float y = (b.h - h) * offset_ / (content_h_ - b.h);
    return {b.w - 8, y, 6, h};
}

EventResult ScrollView::on_wheel(WheelEvent& e) {
    if (content_h_ <= frame().h) return EventResult::Ignored;
    const float d = e.precise ? e.dy : e.dy * 40;
    scroll_to(offset_ - d);
    Window* win = window();
    bar_.set(1, Transition::curve(std::chrono::milliseconds(100), Easing::ease_out(), false));
    if (win) {
        if (fade_timer_) win->cancel_timer(fade_timer_);
        fade_timer_ = win->add_timer(std::chrono::milliseconds(900), [this] {
            fade_timer_ = 0;
            bar_.set(0, Transition::curve(std::chrono::milliseconds(300), Easing::ease_in_out(), false));
        });
    }
    return EventResult::Handled;
}

EventResult ScrollView::on_pointer(PointerEvent& e) {
    if (e.button != platform::MouseButton::left && !e.captured) return EventResult::Ignored;
    const Rect t = thumb();
    if (e.type == PointerEvent::Type::Press && !t.empty() && e.local.x >= t.x - 2) {
        dragging_ = true;
        drag_from_ = e.local.y;
        drag_offset_ = offset_;
        bar_.set(1, Transition::curve(std::chrono::milliseconds(100), Easing::ease_out(), false));
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Move && dragging_ && e.captured) {
        const float track = frame().h - t.h;
        if (track > 0) scroll_to(drag_offset_ + (e.local.y - drag_from_) * (content_h_ - frame().h) / track);
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Release && dragging_) {
        dragging_ = false;
        bar_.set(0, Transition::curve(std::chrono::milliseconds(300), Easing::ease_in_out(), false));
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void ScrollView::paint_over(PaintContext& p) const {
    const float a = std::max(bar_.value(), hovered() ? 0.5f : 0.0f);
    const Rect t = thumb();
    if (t.empty() || a <= 0) return;
    p.dl.round_rect(t, 3, p.theme.fg.with_alpha(0.28f * a));
}

// ---------------------------------------------------------------- ListView

ListView::ListView(PaintRow paint_row, std::string id) : Widget(std::move(id)), paint_row_(std::move(paint_row)) {
    style().clip = true;
}

void ListView::set_count(size_t n) {
    count_ = n;
    if (selected_ && *selected_ >= n) selected_ = n ? std::optional<size_t>(n - 1) : std::nullopt;
    if (hover_ && *hover_ >= n) hover_.reset();
    clamp_offset();
    invalidate_paint();
}

void ListView::set_row_height(float h) {
    row_h_ = h;
    clamp_offset();
    invalidate_paint();
}

void ListView::arrange(const Rect& f) {
    Widget::arrange(f);
    clamp_offset();
}

void ListView::clamp_offset() {
    const float max_off = std::max(0.0f, static_cast<float>(count_) * row_h_ - frame().h);
    offset_ = std::clamp(offset_, 0.0f, max_off);
}

void ListView::set_selected(std::optional<size_t> i, bool scroll) {
    if (i && *i >= count_) i.reset();
    if (i == selected_) return;
    selected_ = i;
    if (i && scroll) scroll_to_row(*i);
    invalidate_paint();
}

void ListView::scroll_to_row(size_t i) {
    const float y = static_cast<float>(i) * row_h_;
    if (y < offset_) offset_ = y;
    else if (y + row_h_ > offset_ + frame().h) offset_ = y + row_h_ - frame().h;
    clamp_offset();
    invalidate_paint();
}

std::optional<size_t> ListView::row_at(float y) const {
    if (y < 0 || y >= frame().h) return std::nullopt;
    const auto i = static_cast<size_t>(std::floor((y + offset_) / row_h_));
    if (i >= count_) return std::nullopt;
    return i;
}

std::string ListView::describe() const {
    return "rows=" + std::to_string(count_) + (selected_ ? " selected=" + std::to_string(*selected_) : "");
}

void ListView::paint(PaintContext& p) const {
    painted_ = 0;
    if (!paint_row_ || count_ == 0 || row_h_ <= 0) return;
    const Rect b = local_bounds();
    const auto first = static_cast<size_t>(std::floor(offset_ / row_h_));
    const auto last = std::min(count_, static_cast<size_t>(std::ceil((offset_ + b.h) / row_h_)) + 1);
    const bool f = focused();
    for (size_t i = first; i < last; ++i) {
        const Rect r{0, static_cast<float>(i) * row_h_ - offset_, b.w, row_h_};
        paint_row_(i, p, r, RowState{hover_ == i, selected_ == i, f});
        ++painted_;
    }
}

EventResult ListView::on_wheel(WheelEvent& e) {
    const float before = offset_;
    offset_ -= e.precise ? e.dy : e.dy * row_h_ * 1.5f;
    clamp_offset();
    if (offset_ == before) return EventResult::Ignored;
    if (window()) hover_ = row_at(to_local(window()->pointer()).y);
    invalidate_paint();
    return EventResult::Handled;
}

void ListView::on_hover(bool inside) {
    if (!inside && hover_) {
        hover_.reset();
        invalidate_paint();
    }
}

EventResult ListView::on_pointer(PointerEvent& e) {
    const auto at = row_at(e.local.y);
    if (e.type == PointerEvent::Type::Move) {
        if (at != hover_) {
            hover_ = at;
            invalidate_paint();
        }
        return EventResult::Ignored;
    }
    if (e.type == PointerEvent::Type::Press) {
        if (!at) return EventResult::Handled;
        if (e.button == platform::MouseButton::right) {
            set_selected(*at, false);
            if (on_context) on_context(*at, e.pos);
            return EventResult::Handled;
        }
        if (e.button == platform::MouseButton::left) {
            set_selected(*at, false);
            if (on_select_mods) on_select_mods(*at, e.mods);
            else if (on_select) on_select(*at);
            if (e.clicks >= 2 && on_activate) on_activate(*at);
            return EventResult::Handled;
        }
    }
    return EventResult::Ignored;
}

EventResult ListView::on_key(const KeyEvent& k) {
    using K = platform::Key;
    if (k.action == platform::KeyAction::release || count_ == 0) return EventResult::Ignored;
    const size_t page = std::max<size_t>(1, static_cast<size_t>(frame().h / row_h_) - 1);
    const size_t cur = selected_.value_or(0);
    std::optional<size_t> next;
    switch (k.key) {
        case K::up: next = selected_ ? (cur > 0 ? cur - 1 : 0) : count_ - 1; break;
        case K::down: next = selected_ ? std::min(count_ - 1, cur + 1) : 0; break;
        case K::page_up: next = cur > page ? cur - page : 0; break;
        case K::page_down: next = std::min(count_ - 1, cur + page); break;
        case K::home: next = 0; break;
        case K::end: next = count_ - 1; break;
        case K::enter:
        case K::kp_enter:
            if (selected_ && on_activate) on_activate(*selected_);
            return selected_ ? EventResult::Handled : EventResult::Ignored;
        default: return EventResult::Ignored;
    }
    set_selected(next);
    if (next && on_select_mods) on_select_mods(*next, static_cast<uint16_t>(k.mods & platform::mod_shift));
    else if (on_select && next) on_select(*next);
    return EventResult::Handled;
}

// ---------------------------------------------------------------- Badge

Badge::Badge(std::string text, Icon icon, Tone tone, std::string id)
    : Widget(std::move(id)), text_(std::move(text)), icon_(icon), tone_(tone) {
    set_hit_testable(false);
}

void Badge::set_text(std::string t) {
    if (t == text_) return;
    text_ = std::move(t);
    run_.reset();
    invalidate_layout();
}

void Badge::set_icon(Icon i) {
    if (i == icon_) return;
    icon_ = i;
    invalidate_layout();
}

void Badge::set_tone(Tone t) {
    if (t == tone_) return;
    tone_ = t;
    invalidate_paint();
}

Size Badge::intrinsic_size(Size) const {
    Window* win = window();
    if (!win) return {40, 18};
    const FontRole role = role_for(win->theme(), TextStyle::small);
    run_ = win->text().shape(text_, role, win->scale());
    float w = std::ceil(run_->width()) + 12;
    if (icon_ != Icon::none) w += 12.0f + (text_.empty() ? 0.0f : 3.0f);
    const float h = std::ceil((run_->height() + 4) * win->scale()) / win->scale();
    return {w, std::max(16.0f, h)};
}

void Badge::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    if (filled_) p.dl.round_rect(b, std::min(6.0f, b.h / 2), t.surface, 1, t.border);
    const Color c = tone_color(t, tone_);
    float x = 6;
    if (icon_ != Icon::none) {
        p.dl.icon(icon_, 12, {x, px_round((b.h - 12) / 2, p.scale)}, c);
        x += 15;
    }
    if (!run_) run_ = p.text.shape(text_, role_for(t, TextStyle::small), p.scale);
    if (!text_.empty()) p.dl.text(run_, {x, px_round((b.h - run_->height()) / 2 + run_->ascent(), p.scale)}, c);
}

// ---------------------------------------------------------------- Gauge

Gauge::Gauge(std::string id) : Widget(std::move(id)) {}

void Gauge::set_fraction(float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    if (f == f_) return;
    f_ = f;
    invalidate_paint();
}

void Gauge::set_thickness(float t) {
    t = std::clamp(t, 2.0f, 16.0f);
    if (t == thickness_) return;
    thickness_ = t;
    invalidate_layout();
}

void Gauge::set_muted(bool m) {
    if (m == muted_) return;
    muted_ = m;
    invalidate_paint();
}

std::string Gauge::describe() const { return std::to_string(static_cast<int>(std::lround(f_ * 100))) + "%"; }

void Gauge::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const float h = std::min(b.h, thickness_);
    const Rect track{0, px_round((b.h - h) / 2, p.scale), b.w, h};
    p.dl.round_rect(track, h / 2, t.fg.with_alpha(0.12f));
    if (f_ <= 0) return;
    const Color c = muted_ ? t.fg.with_alpha(0.35f) : f_ > 0.9f ? t.failure : f_ > 0.75f ? t.warning : t.accent;
    p.dl.round_rect(Rect{track.x, track.y, std::max(h, track.w * f_), h}, h / 2, c);
}

// ---------------------------------------------------------------- Sparkline

Sparkline::Sparkline(size_t capacity, std::string id) : Widget(std::move(id)), cap_(std::max<size_t>(2, capacity)) {
    set_hit_testable(false);
}

void Sparkline::push(float v) {
    v_.push_back(v);
    while (v_.size() > cap_) v_.pop_front();
    invalidate_paint();
}

void Sparkline::set_values(std::vector<float> v) {
    v_.assign(v.begin(), v.end());
    while (v_.size() > cap_) v_.pop_front();
    invalidate_paint();
}

void Sparkline::paint(PaintContext& p) const {
    const Rect b = local_bounds();
    if (v_.empty() || b.w <= 0) return;
    float mx = max_.value_or(0);
    if (!max_) {
        for (float v : v_) mx = std::max(mx, v);
    }
    if (mx <= 0) mx = 1;
    const Color c = tone_color(p.theme, tone_);
    const float col = b.w / static_cast<float>(cap_);
    const float px1 = 1.0f / p.scale;
    // Baseline, then one column per sample, newest at the right edge.
    p.dl.rect(Rect{0, b.h - px1, b.w, px1}, c.with_alpha(0.25f));
    const size_t n = v_.size();
    for (size_t i = 0; i < n; ++i) {
        const float v = std::clamp(v_[i] / mx, 0.0f, 1.0f);
        if (v <= 0) continue;
        const float x = b.w - static_cast<float>(n - i) * col;
        const float h = std::max(px1, px_round(v * b.h, p.scale));
        const float w = std::max(px1, col - (col >= 3 ? px1 : 0));
        p.dl.rect(Rect{px_round(x, p.scale), b.h - h, w, h}, c.with_alpha(i + 1 == n ? 0.95f : 0.6f));
    }
}

// Draws a text with its matches on a highlight, at most max_w wide; a long text is cut in front
// so that the first match shows. Returns the drawn width.
float paint_marked(PaintContext& p, std::string text, std::vector<std::pair<size_t, size_t>> marks, FontRole role, float x,
                   float top, float max_w, Color fg) {
    for (char& ch : text)
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    if (max_w < 10) return 0;
    if (!marks.empty() && marks.front().first > 24 && p.text.shape(text, role, p.scale)->width() > max_w) {
        size_t start = marks.front().first - 20;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
        static const std::string kEllipsis = "\xE2\x80\xA6";
        text = kEllipsis + text.substr(start);
        std::vector<std::pair<size_t, size_t>> moved;
        for (auto [off, len] : marks)
            if (off >= start) moved.emplace_back(off - start + kEllipsis.size(), len);
        marks = std::move(moved);
    }
    auto run = p.text.ellipsize(text, role, p.scale, max_w);
    const float y = std::round(top + run->ascent());
    for (auto [off, len] : marks) {
        if (off + len > text.size()) continue;
        const float x0 = p.text.shape(std::string_view(text).substr(0, off), role, p.scale)->width();
        if (x0 >= run->width()) break;
        const float w = std::min(p.text.shape(std::string_view(text).substr(off, len), role, p.scale)->width(), run->width() - x0);
        p.dl.round_rect(Rect{x + x0 - 1, top, w + 2, run->height()}, 2, p.theme.warning.with_alpha(0.35f));
    }
    p.dl.text(run, {x, y}, fg);
    return run->width();
}

}  // namespace kt::ui
