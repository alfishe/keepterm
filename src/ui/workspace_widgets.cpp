#include "ui/workspace_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kt::ui {

namespace {

constexpr float kRowH = 26;
constexpr float kDragThreshold = 4;
constexpr float kDividerBand = 8;
constexpr auto kFade = std::chrono::milliseconds(120);

float px_round(float v, float scale) { return std::round(v * scale) / scale; }

Transition fade_tr() { return Transition::curve(kFade, Easing::ease_out(), false); }

Color attention_color(const Theme& t, Attention a) {
    switch (a) {
        case Attention::done: return t.success;
        case Attention::failed: return t.failure;
        case Attention::bell: return t.warning;
        case Attention::running: return t.accent;
        case Attention::idle: break;
    }
    return {};
}

// The 5 px status dot inside a 7 px circle of the row background, at an icon's bottom-right
// (Macterm's done-status badge).
void status_dot(PaintContext& p, Vec2 icon_tl, float icon, Color c, Color under) {
    const float cx = icon_tl.x + icon - 1.5f, cy = icon_tl.y + icon - 1.5f;
    p.dl.round_rect(Rect{cx - 3.5f, cy - 3.5f, 7, 7}, 3.5f, under);
    p.dl.round_rect(Rect{cx - 2.5f, cy - 2.5f, 5, 5}, 2.5f, c);
}

Attention strongest(const WorkspaceSnapshot& s, const std::vector<PaneKey>& panes) {
    Attention best = Attention::idle;
    auto rank = [](Attention a) {
        switch (a) {
            case Attention::failed: return 4;
            case Attention::bell: return 3;
            case Attention::done: return 2;
            case Attention::running: return 1;
            case Attention::idle: break;
        }
        return 0;
    };
    for (PaneKey k : panes) {
        if (const PaneItem* p = s.pane(k); p && rank(p->attention) > rank(best)) best = p->attention;
    }
    return best;
}

std::string agent_of(const WorkspaceSnapshot& s, const TabItem& t) {
    if (const PaneItem* p = s.pane(t.focused); p && !p->agent.empty()) return p->agent;
    for (PaneKey k : t.panes) {
        if (const PaneItem* p = s.pane(k); p && !p->agent.empty()) return p->agent;
    }
    return {};
}

std::string compact_cpu(const Resources& r) {
    if (!r.valid) return "\xE2\x80\x94";
    if (r.cpu < 999.5f) return format_cpu(r.cpu);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1fk%%", static_cast<double>(std::min(r.cpu, 9999.0f)) / 1000.0);
    return buf;
}

void fixed_column(Label& l, std::vector<std::string> samples) {
    l.set_width_samples(std::move(samples));
    l.set_align(Label::Align::end);
    l.set_elide(Label::Elide::none);
    l.style().shrink = 0;
}

// "390M 9%": the compact figures of tabs and sidebar rows (the tooltip has the long form).
std::string stats_line(const Resources& r) {
    if (!r.valid) return {};
    char buf[32];
    const double mb = static_cast<double>(r.memory) / (1024.0 * 1024.0);
    if (mb < 1000) std::snprintf(buf, sizeof buf, "%.0fM", mb);
    else std::snprintf(buf, sizeof buf, "%.1fG", mb / 1024.0);
    return std::string(buf) + " " + format_cpu(r.cpu);
}

// "~/src" for a folder under home. '/' and '\\' match each other (Windows paths come both ways),
// ASCII case is folded for Windows paths. Never a bare "~": the home folder itself stays a full
// path. A Windows path (drive letter or backslash) is never abbreviated: "~" means nothing to
// Windows tools. Decided by the path, not the build, so the output is the same everywhere.
std::string home_relative(const std::string& path, const std::string& home) {
    const bool win_path = (path.size() >= 2 && path[1] == ':') || path.find('\\') != std::string::npos;
    if (win_path) return path;
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    auto same = [&](char a, char b) { return (sep(a) && sep(b)) || a == b; };
    std::size_t n = home.size();
    while (n > 1 && sep(home[n - 1])) --n;
    if (n == 0 || path.size() < n) return path;
    for (std::size_t i = 0; i < n; ++i)
        if (!same(path[i], home[i])) return path;
    if (path.size() != n && !sep(path[n])) return path;
    std::size_t rest = n;
    while (rest < path.size() && sep(path[rest])) ++rest;
    if (rest >= path.size()) return path;   // the home folder itself
    return "~" + path.substr(n);
}


}  // namespace

// The last component of a path ("keepterm", "me" for C:\Users\me); a root stays as it is
// ("C:\", "/").
std::string folder_label(const std::string& path) {
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    std::size_t end = path.size();
    while (end > 1 && sep(path[end - 1])) {
        if (end == 3 && path[1] == ':') break;   // "C:\"
        --end;
    }
    if (end == 3 && path.size() >= 3 && path[1] == ':' && sep(path[2])) return path.substr(0, 3);
    if (end == 2 && path[1] == ':') return path.substr(0, 2) + "\\";
    if (end == 1 && sep(path[0])) return path.substr(0, 1);
    std::size_t start = end;
    while (start > 0 && !sep(path[start - 1])) --start;
    return path.substr(start, end - start);
}

std::string format_bytes(uint64_t b) {
    char buf[32];
    const double v = static_cast<double>(b);
    if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.0f KB", v / 1024);
    else if (b < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.0f MB", v / (1024.0 * 1024));
    else std::snprintf(buf, sizeof buf, "%.1f GB", v / (1024.0 * 1024 * 1024));
    return buf;
}

StorageUsage storage_usage(uint64_t used, uint64_t limit) {
    StorageUsage u;
    u.valid = true;
    u.used = used;
    u.limit = limit == UINT64_MAX ? 0 : limit;
    u.label = u.limit ? format_bytes(used) + " of " + format_bytes(u.limit) : format_bytes(used);
    return u;
}

Icon agent_icon(std::string_view agent) { return agent.empty() ? Icon::terminal : Icon::agent; }

Color agent_color(const Theme& t, std::string_view agent) {
    if (agent == "claude") return Color::rgb(0xD97757);
    if (agent == "codex") return t.scheme == Scheme::dark ? Color::rgb(0xE5E7EB) : Color::rgb(0x111827);
    if (agent == "gemini") return Color::rgb(0x4285F4);
    if (agent.empty()) return t.fg_muted;
    return t.accent;
}

// ---------------------------------------------------------------- TerminalWidget

TerminalWidget::TerminalWidget(ITerminalHost& host, PaneKey pane) : Widget("terminal"), host_(host), pane_(pane) {
    style().grow = 1;
}

TerminalWidget::~TerminalWidget() {
    if (flash_timer_ && window()) window()->cancel_timer(flash_timer_);
}

ITerminalSurface* TerminalWidget::surface() const { return host_.terminal(pane_); }

bool TerminalWidget::paint_external(Compositor& c) {
    ITerminalSurface* s = surface();
    if (!s) return false;
    // Keyboard focus as the terminal sees it: this widget focused in a focused window. Synced on
    // every paint, so a surface bound after the widget took focus (startup, restore) and the
    // window losing or regaining activation both reach it (filled blinking block vs. hollow box).
    if (Window* w = window()) s->set_focused(w->window_focused() && w->focused() == this);
    const Rect b = bounds_in_window();
    s->set_bounds(c.snap(b));
    c.out.push_clip(c.snap(b));
    s->paint(c.out, c.full, *c.damage);
    c.out.pop_clip();
    // The scrollbar lives in paint_over: repaint it when the scroll position moved (wheel, keys,
    // prompt jumps, new output while scrolled up).
    const ScrollMetrics m = s->scroll_metrics();
    if (m.history != painted_.history || m.offset != painted_.offset || m.rows != painted_.rows || m.marks != painted_.marks ||
        m.current_mark != painted_.current_mark) {
        if (m.offset != painted_.offset && m.history == painted_.history) bar_flash();
        invalidate_paint();
    }
    return true;
}

namespace {
constexpr float kBarHit = 14;      // the strip at the right edge that takes the pointer
constexpr float kBarInset = 2, kBarThin = 6, kBarWide = 9, kThumbMin = 24;
constexpr auto kFlash = std::chrono::milliseconds(1200);
}  // namespace

TerminalWidget::Bar TerminalWidget::bar_geometry(const ScrollMetrics& m) const {
    Bar g;
    const Rect b = local_bounds();
    if (m.history == 0 || m.rows == 0 || b.h < 3 * kThumbMin) return g;
    const float w = (bar_hover_ || bar_drag_) ? kBarWide : kBarThin;
    g.track = Rect{b.w - w - kBarInset, kBarInset, w, b.h - 2 * kBarInset};
    const double total = static_cast<double>(m.history) + m.rows;
    const float th = std::clamp(static_cast<float>(static_cast<double>(g.track.h) * m.rows / total), kThumbMin, g.track.h);
    const double top_line = static_cast<double>(m.history - std::min<uint64_t>(m.offset, m.history));
    const float y = g.track.y + static_cast<float>((g.track.h - th) * (top_line / static_cast<double>(m.history)));
    g.thumb = Rect{g.track.x, y, w, th};
    g.valid = true;
    return g;
}

bool TerminalWidget::bar_shown(const ScrollMetrics& m) const {
    if (m.history == 0) return false;
    const Window* w = window();
    return m.offset > 0 || !m.marks.empty() || bar_hover_ || bar_drag_ || (w && w->now() < flash_until_);
}

void TerminalWidget::bar_flash() {
    Window* w = window();
    if (!w) return;
    flash_until_ = w->now() + kFlash;
    if (flash_timer_) w->cancel_timer(flash_timer_);
    flash_timer_ = w->add_timer(kFlash + std::chrono::milliseconds(50), [this, alive = std::weak_ptr<bool>(alive_)] {
        if (alive.expired()) return;
        flash_timer_ = 0;
        invalidate_paint();
    });
}

bool TerminalWidget::bar_pointer(PointerEvent& e) {
    ITerminalSurface* s = surface();
    if (!s) return false;
    const ScrollMetrics m = s->scroll_metrics();
    using T = PointerEvent::Type;
    const bool near = m.history > 0 && e.local.x >= local_bounds().w - kBarHit && e.local.x <= local_bounds().w;
    if (!bar_drag_ && near != bar_hover_ && (e.type == T::Move || e.type == T::Enter || e.type == T::Leave)) {
        bar_hover_ = near && e.type != T::Leave;
        invalidate_paint();
    }
    auto offset_for = [&](const Bar& g, float thumb_top) {
        const float span = g.track.h - g.thumb.h;
        const double f = span > 0 ? std::clamp(static_cast<double>(thumb_top - g.track.y) / span, 0.0, 1.0) : 1.0;
        return static_cast<uint32_t>(std::llround(static_cast<double>(m.history) * (1.0 - f)));
    };
    if (bar_drag_) {
        const Bar g = bar_geometry(m);
        if (g.valid && (e.type == T::Move || e.type == T::Release)) s->scroll_to_offset(offset_for(g, e.local.y - drag_grab_));
        if (e.type == T::Release) {
            bar_drag_ = false;
            bar_hover_ = near;
            bar_flash();
        }
        invalidate_paint();
        return true;
    }
    if (e.type != T::Press || e.button != platform::MouseButton::left || !near) return false;
    const Bar g = bar_geometry(m);
    if (!g.valid) return false;
    if (e.local.y >= g.thumb.y && e.local.y < g.thumb.bottom()) {
        bar_drag_ = true;
        drag_grab_ = e.local.y - g.thumb.y;
    } else {   // a page toward the click
        const uint32_t page = std::max<uint32_t>(1, m.rows > 1 ? m.rows - 1 : 1);
        const uint64_t want = e.local.y < g.thumb.y ? std::min<uint64_t>(m.history, uint64_t{m.offset} + page)
                                                    : (m.offset > page ? m.offset - page : 0);
        s->scroll_to_offset(static_cast<uint32_t>(want));
        bar_flash();
    }
    invalidate_paint();
    return true;
}

EventResult TerminalWidget::on_pointer(PointerEvent& e) {
    if (ITerminalSurface* s = surface()) {
        if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left && !focused() && window()) {
            window()->set_focus(this, false);
        }
        if (bar_pointer(e)) return EventResult::Handled;
        // The pane menu, unless the program takes mouse reports (Shift forces the menu).
        if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::right &&
            (!s->wants_mouse() || (e.mods & platform::mod_shift)) && host_.terminal_menu(pane_, e.pos)) {
            return EventResult::Handled;
        }
        s->pointer(e, e.local);
        sync_link();
        return e.type == PointerEvent::Type::Move && !e.captured ? EventResult::Ignored : EventResult::Handled;
    }
    return EventResult::Ignored;
}

void TerminalWidget::sync_link() {
    ITerminalSurface* s = surface();
    std::string l = s && hovered() ? s->hover_link() : std::string();
    if (l == link_) return;
    link_ = std::move(l);
    invalidate_paint();   // the cursor follows on the next pointer move
}

void TerminalWidget::on_hover(bool inside) {
    Widget::on_hover(inside);
    if (!inside) sync_link();
}

platform::CursorShape TerminalWidget::cursor_at(Vec2) const {
    if (bar_hover_ || bar_drag_) return platform::CursorShape::arrow;
    return link_.empty() ? platform::CursorShape::ibeam : platform::CursorShape::hand;
}

void TerminalWidget::paint_over(PaintContext& p) const {
    if (ITerminalSurface* s = surface()) {
        const ScrollMetrics m = s->scroll_metrics();
        painted_ = m;
        if (bar_shown(m)) {
            const Bar g = bar_geometry(m);
            if (g.valid) {
                const Color fg = Color::rgb(m.fg);
                if (bar_hover_ || bar_drag_) p.dl.round_rect(g.track, g.track.w / 2, fg.with_alpha(0.10f));
                p.dl.round_rect(g.thumb, g.thumb.w / 2, fg.with_alpha(bar_drag_ ? 0.65f : bar_hover_ ? 0.5f : 0.35f));
                // Find matches: ticks over the thumb, the current one taller and opaque.
                const Color tick = Color::rgb(0xF5A623);
                auto tick_at = [&](uint16_t v, float h, float a) {
                    const float y = g.track.y + (static_cast<float>(v) + 0.5f) / kScrollMarks * g.track.h;
                    p.dl.rect(Rect{g.track.x, std::round(y - h / 2), g.track.w, h}, tick.with_alpha(a));
                };
                for (uint16_t v : m.marks) tick_at(v, 2, 0.75f);
                if (m.current_mark) tick_at(*m.current_mark, 4, 1.0f);
            }
        }
    }
    if (link_.empty()) return;
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    const FontRole f = role_for(t, TextStyle::small);
    const FontMetrics m = p.text.metrics(f, p.scale);
    const float pad_x = 8, h = std::ceil(m.ascent + m.descent) + 8;
    const float max_w = std::max(40.0f, std::min(b.w * 0.75f, b.w - 2 * pad_x - 12));
    TextRunPtr run = p.text.ellipsize(link_, f, p.scale, max_w, true);
    if (!run) return;
    const Rect pill{6, b.h - h - 6, std::ceil(run->width()) + 2 * pad_x, h};
    if (pill.y < 0) return;
    p.dl.round_rect(pill, t.radius_control, t.surface_raised, 1.0f, t.border);
    p.dl.text(run, {px_round(pill.x + pad_x, p.scale), px_round(pill.y + 4 + m.ascent, p.scale)}, t.fg_muted);
}

EventResult TerminalWidget::on_wheel(WheelEvent& e) {
    if (ITerminalSurface* s = surface()) return s->wheel(e, e.local) ? EventResult::Handled : EventResult::Ignored;
    return EventResult::Ignored;
}

EventResult TerminalWidget::on_key(const KeyEvent& k) {
    if (ITerminalSurface* s = surface()) {
        s->key(k);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

EventResult TerminalWidget::on_text(std::string_view t) {
    if (ITerminalSurface* s = surface()) {
        s->text(t);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void TerminalWidget::on_focus(FocusChange f) {
    if (ITerminalSurface* s = surface()) s->set_focused(f == FocusChange::Gained);
    if (f == FocusChange::Gained) host_.terminal_focused(pane_);
    invalidate_paint();
}

void TerminalWidget::ime_preedit(std::string_view text, std::span<const ImeSpan>, int caret) {
    if (ITerminalSurface* s = surface()) s->preedit(text, caret);
}

void TerminalWidget::ime_commit(std::string_view text) {
    if (ITerminalSurface* s = surface()) {
        s->preedit({}, 0);
        s->text(text);
    }
}

void TerminalWidget::ime_cancel() {
    if (ITerminalSurface* s = surface()) s->preedit({}, 0);
}

Rect TerminalWidget::ime_caret_rect() const {
    const Rect b = bounds_in_window();
    if (ITerminalSurface* s = surface()) return s->caret_rect_dip().translated(b.origin());
    return {b.x, b.y, 1, 16};
}

// ---------------------------------------------------------------- PaneFrame

PaneFrame::PaneFrame(MainView& mv, PaneKey pane) : Widget("pane"), mv_(mv), pane_(pane), dim_(*this, 0) {
    style().dir = LayoutStyle::Dir::Column;
    style().clip = true;
    // With splits only: a slim stats line (what runs, where, memory and CPU), not a tab.
    auto h = row("header", 8);
    h->style().height = 20.0f;
    h->style().padding = Insets{8, 0, 4, 0};
    h->style().shrink = 0;
    h->style().align_items = LayoutStyle::Align::Center;
    icon_ = &h->emplace<IconView>(Icon::terminal, 12.0f, Tone::muted);
    title_ = &h->emplace<Label>("", TextStyle::small, Tone::normal, "title");
    title_->set_elide(Label::Elide::end);
    title_->style().shrink = 0;       // the folder gives way first
    title_->style().max_w = 180;
    cwd_ = &h->emplace<Label>("", TextStyle::small, Tone::muted, "cwd");
    cwd_->set_elide(Label::Elide::middle);
    cwd_->style().shrink = 4;
    h->add(spacer());
    agent_ = &h->emplace<Badge>("", Icon::none, Tone::muted, "process");
    agent_->set_filled(false);
    mem_ = &h->emplace<Badge>("", Icon::none, Tone::muted, "memory");
    mem_->set_filled(false);
    auto zb = Button::icon_button(Icon::zoom, std::string(tr("Zoom Pane")), [this] { mv_.view().toggle_zoom(pane_); }, "zoom", 16);
    zoom_ = &h->add(std::move(zb));
    auto cb = Button::icon_button(Icon::close, std::string(tr("Close Pane")), [this] { mv_.view().close_pane(pane_); }, "close", 16);
    close_ = &h->add(std::move(cb));
    header_ = &add(std::move(h));
    term_ = &emplace<TerminalWidget>(mv, pane);
    // Corner pill (Macterm's "Zoomed" badge) and the exit notice.
    auto pill = std::make_unique<Badge>(std::string(tr("Zoomed")), Icon::zoom, Tone::muted, "zoomed");
    pill->style().position = LayoutStyle::Position::Absolute;
    pill->style().inset = Insets{0, 8, 10, 0};
    pill->style().inset_r = true;
    zoom_pill_ = &add(std::move(pill));
    zoom_pill_->set_visible(false);
    auto ro = std::make_unique<Badge>(std::string(tr("Read-Only")), Icon::keyboard, Tone::warning, "read_only");
    ro->style().position = LayoutStyle::Position::Absolute;
    ro->style().inset = Insets{12, 0, 0, 12};
    ro->style().inset_b = ro->style().fit_w = true;
    ro_pill_ = &add(std::move(ro));
    ro_pill_->set_visible(false);
    ro_pill_->set_tooltip(std::string(tr("Typing is ignored in this pane. Pane > Read-Only Mode turns it off.")));
    auto ex = row("exited", 8);
    ex->style().position = LayoutStyle::Position::Absolute;
    ex->style().inset = Insets{0, 0, 12, 12};
    ex->style().inset_r = ex->style().inset_b = true;
    ex->style().padding = Insets::hv(10, 5);
    ex->set_background(Color{}, 6);
    exit_label_ = &ex->emplace<Label>("", TextStyle::small, Tone::muted);
    exit_box_ = &add(std::move(ex));
    exit_box_->set_visible(false);
}

void PaneFrame::update(const PaneItem& p, bool focused, bool multi, bool zoomed) {
    const bool show_header = multi || zoomed;
    header_->set_visible(show_header);
    std::string title = p.title.empty() ? (p.process.empty() ? std::string(tr("Shell")) : p.process) : p.title;
    title_->set_text(title);
    cwd_->set_text(home_relative(p.cwd, mv_.view().snapshot().home));
    icon_->set_icon(agent_icon(p.agent));
    if (!p.agent.empty()) {
        agent_->set_text(p.agent);
        agent_->set_icon(Icon::agent);
        agent_->set_visible(true);
    } else if (!p.process.empty() && p.process != title) {
        agent_->set_text(p.process);
        agent_->set_icon(Icon::none);
        agent_->set_visible(true);
    } else {
        agent_->set_visible(false);
    }
    mem_->set_visible(p.res.valid);
    if (p.res.valid) mem_->set_text(stats_line(p.res));
    mem_->set_tooltip(std::string(tr("Show the processes of this pane")));
    zoom_->set_icon(zoomed ? Icon::unzoom : Icon::zoom, 12);
    zoom_->set_checked(zoomed);
    zoom_->set_tooltip(std::string(zoomed ? tr("Unzoom Pane") : tr("Zoom Pane")));
    zoom_pill_->set_visible(zoomed && !show_header);
    ro_pill_->set_visible(p.read_only);
    exit_box_->set_visible(p.exited);
    if (p.exited) {
        exit_label_->set_text(p.exit_code ? std::string(tr("Process exited with code ")) + std::to_string(*p.exit_code)
                                          : std::string(tr("Process exited")));
    }
    if (focused != focused_ || multi != multi_) {
        focused_ = focused;
        multi_ = multi;
        dim_.set(multi && !focused ? 1.0f : 0.0f, Transition::curve(std::chrono::milliseconds(150), Easing::ease_out(), false));
        invalidate_paint();
    }
}

EventResult PaneFrame::on_pointer(PointerEvent& e) {
    using T = PointerEvent::Type;
    if (e.type == T::Press && e.button == platform::MouseButton::left && mem_->visible() &&
        mem_->bounds_in_window().contains(e.pos) && mv_.hooks().show_resources) {
        mv_.hooks().show_resources(ResourceScope{ResourceScope::Kind::pane, pane_, 1}, mem_->bounds_in_window(), false);
        return EventResult::Handled;
    }
    // The header is the pane's grab handle: a drag moves the pane next to another one.
    if (e.type == T::Press && e.button == platform::MouseButton::left && multi_ && header_->visible() &&
        header_->bounds_in_window().contains(e.pos)) {
        grab_ = true;
        dragging_ = false;
        grab_at_ = e.pos;
        return EventResult::Handled;
    }
    if (!grab_) return EventResult::Ignored;
    if (e.type == T::Move) {
        if (!dragging_ && std::hypot(e.pos.x - grab_at_.x, e.pos.y - grab_at_.y) >= 5) dragging_ = true;
        if (dragging_) mv_.set_pane_drop(mv_.pane_drop_at(pane_, e.pos));
        return EventResult::Handled;
    }
    if (e.type == T::Release) {
        const MainView::PaneDrop d = dragging_ ? mv_.pane_drop_at(pane_, e.pos) : MainView::PaneDrop{};
        const bool clicked = !dragging_;
        grab_ = dragging_ = false;
        mv_.set_pane_drop({});
        if (d.target) {
            mv_.view().dock_pane(pane_, d.target, d.side);
            mv_.sync();
            mv_.focus_active_pane();
        } else if (clicked) {
            mv_.view().focus_pane(pane_);
            mv_.sync();
            mv_.focus_active_pane();
        }
        return EventResult::Handled;
    }
    return EventResult::Handled;
}

void PaneFrame::paint_over(PaintContext& p) const {
    const Rect b = local_bounds();
    const float d = dim_.value();
    if (d > 0) {
        const Rect tb = term_->frame();
        p.dl.rect(tb, p.theme.dim_overlay.fade(d));
    }
    if (header_->visible()) p.dl.hairline(Rect{0, header_->frame().bottom() - 1, b.w, 1}, p.theme.border);
    if (multi_ && focused_) p.dl.round_rect(b, 0, {}, 1, p.theme.accent.with_alpha(0.45f));
}

// ---------------------------------------------------------------- dividers and splits

DividerHandle::DividerHandle(MainView& mv, TabKey tab) : Widget("divider"), mv_(mv), tab_(tab) {}

void DividerHandle::paint(PaintContext& p) const {
    const Rect b = local_bounds();
    const Color c = dragging_ || hovered() ? p.theme.accent.with_alpha(0.6f) : p.theme.border;
    if (d_.vertical) p.dl.hairline(Rect{std::floor(b.w / 2), 0, 1, b.h}, c);
    else p.dl.hairline(Rect{0, std::floor(b.h / 2), b.w, 1}, c);
}

EventResult DividerHandle::on_pointer(PointerEvent& e) {
    if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left) {
        dragging_ = true;
        invalidate_paint();
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Move && dragging_ && e.captured) {
        const Rect par = d_.parent.translated(parent() ? parent()->bounds_in_window().origin() : Vec2{});
        float r = d_.vertical ? (e.pos.x - par.x) / std::max(1.0f, par.w) : (e.pos.y - par.y) / std::max(1.0f, par.h);
        r = std::clamp(r, 0.15f, 0.85f);
        mv_.view().set_divider_ratio(tab_, d_.ref, r);
        mv_.sync();
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Release && dragging_) {
        dragging_ = false;
        invalidate_paint();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

SplitContainer::SplitContainer(MainView& mv) : Widget("panes"), mv_(mv) {
    style().grow = 1;
    style().clip = true;
}

PaneFrame* SplitContainer::frame_for(PaneKey k) const {
    for (PaneFrame* f : frames_) {
        if (f->pane() == k) return f;
    }
    return nullptr;
}

void SplitContainer::update(const WorkspaceSnapshot& s, const TabItem* tab) {
    const TabKey new_tab = tab ? tab->id : 0;
    if (new_tab != tab_) {
        // Another tab: the pane frames of the old one go (their terminals stay with the host).
        clear_children();
        frames_.clear();
        dividers_.clear();
        tab_ = new_tab;
    }
    if (!tab) return;
    std::vector<uint64_t> keys(tab->panes.begin(), tab->panes.end());
    // Frames first (reconciled by pane), dividers after them (hit first).
    std::vector<std::unique_ptr<Widget>> divs;
    for (DividerHandle* d : dividers_) divs.push_back(take_child(*d));
    dividers_.clear();
    reconcile(
        *this, keys,
        [](const Widget& w) {
            auto* f = dynamic_cast<const PaneFrame*>(&w);
            return f ? f->pane() : 0;
        },
        [this](uint64_t k) { return std::make_unique<PaneFrame>(mv_, k); },
        [&](Widget& w, uint64_t k) {
            auto& f = static_cast<PaneFrame&>(w);
            const PaneItem* p = s.pane(k);
            const bool zoomed = tab->zoomed && *tab->zoomed == k;
            if (p) f.update(*p, tab->focused == k, tab->panes.size() > 1 && !tab->zoomed, zoomed);
            f.set_visible(!tab->zoomed || zoomed);
        });
    frames_.clear();
    for (const auto& c : children()) frames_.push_back(static_cast<PaneFrame*>(c.get()));
    zoomed_ = tab->zoomed;
    for (auto& d : divs) dividers_.push_back(static_cast<DividerHandle*>(&add_child(std::move(d))));
    invalidate_layout();
}

void SplitContainer::arrange(const Rect& f) {
    assign_frame(f);
    if (!tab_) return;
    std::vector<PaneRect> panes;
    std::vector<DividerRect> divs;
    const Rect area{0, 0, f.w, f.h};
    if (zoomed_) {
        panes.push_back({*zoomed_, area});
    } else {
        mv_.view().layout(tab_, area, 1.0f, panes, divs);
    }
    for (PaneFrame* fr : frames_) {
        auto it = std::find_if(panes.begin(), panes.end(), [&](const PaneRect& r) { return r.pane == fr->pane(); });
        if (it == panes.end()) continue;
        const float x0 = snap(it->r.x), y0 = snap(it->r.y);
        fr->arrange(Rect{x0, y0, snap(it->r.right()) - x0, snap(it->r.bottom()) - y0});
    }
    // Divider handles: one per divider, reused in order.
    while (dividers_.size() < divs.size()) dividers_.push_back(&emplace<DividerHandle>(mv_, tab_));
    while (dividers_.size() > divs.size()) {
        Widget* d = dividers_.back();
        dividers_.pop_back();
        (void)take_child(*d);
    }
    for (size_t i = 0; i < divs.size(); ++i) {
        const DividerRect& d = divs[i];
        dividers_[i]->set(d);
        Rect band = d.r;
        if (d.vertical) band = Rect{d.r.x + d.r.w / 2 - kDividerBand / 2, d.r.y, kDividerBand, d.r.h};
        else band = Rect{d.r.x, d.r.y + d.r.h / 2 - kDividerBand / 2, d.r.w, kDividerBand};
        dividers_[i]->arrange(Rect{snap(band.x), snap(band.y), snap(band.w), snap(band.h)});
    }
}

// ---------------------------------------------------------------- sidebar rows

class SidebarRow : public Widget {
public:
    SidebarRow(MainView& mv, Sidebar& sb, std::string id) : Widget(std::move(id)), mv_(mv), sb_(sb), hover_(*this) {
        style().dir = LayoutStyle::Dir::Row;
        style().align_items = LayoutStyle::Align::Center;
        style().height = kRowH;
        style().gap = 6;
    }
    void on_hover(bool in) override {
        hover_.set(in ? 1.0f : 0.0f, fade_tr());
        hover_changed(in);
    }
    virtual void hover_changed(bool) {}
    virtual void click(const PointerEvent&) = 0;
    virtual void context(Vec2 at) = 0;
    void set_selected(bool s) {
        if (s == selected_) return;
        selected_ = s;
        invalidate_paint();
    }
    bool selected() const { return selected_; }
    void paint(PaintContext& p) const override {
        const Rect b = local_bounds().inset(Insets::hv(6, 1));
        const Theme& t = p.theme;
        Color c{};
        if (selected_) c = t.fg.with_alpha(p.window && p.window->window_focused() ? 0.10f : 0.07f);
        const float h = hover_.value();
        if (h > 0 && !selected_) c = t.fg.with_alpha(0.05f * h);
        if (c.a > 0) p.dl.round_rect(b, t.radius_control, c);
    }
    std::string describe() const override { return selected_ ? "selected" : ""; }
    EventResult on_pointer(PointerEvent& e) override {
        switch (e.type) {
            case PointerEvent::Type::Press:
                if (e.button == platform::MouseButton::right) {
                    context(e.pos);
                    return EventResult::Handled;
                }
                if (e.button != platform::MouseButton::left) return EventResult::Ignored;
                press_ = e.pos;
                dragging_ = false;
                click(e);
                return EventResult::Handled;
            case PointerEvent::Type::Move:
                if (!e.captured) return EventResult::Ignored;
                if (!dragging_ && std::hypot(e.pos.x - press_.x, e.pos.y - press_.y) > kDragThreshold) dragging_ = true;
                if (dragging_) sb_.drag_move(*this, e.pos);
                return EventResult::Handled;
            case PointerEvent::Type::Release:
                if (dragging_) {
                    dragging_ = false;
                    sb_.drag_end(*this, e.pos, false);
                }
                return EventResult::Handled;
            default: return EventResult::Ignored;
        }
    }
    EventResult on_key(const KeyEvent& k) override {
        if (k.key == platform::Key::escape && dragging_) {
            dragging_ = false;
            sb_.drag_end(*this, {}, true);
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }
    uint64_t key = 0;

protected:
    MainView& mv_;
    Sidebar& sb_;
    Animated hover_;
    bool selected_ = false;
    bool dragging_ = false;
    Vec2 press_;
};

class TabRow final : public SidebarRow {
public:
    TabRow(MainView& mv, Sidebar& sb, TabKey tab) : SidebarRow(mv, sb, "tab"), tab_(tab), close_fade_(*this) {
        key = tab;
        style().padding = Insets{30, 0, 10, 0};
        icon_ = &emplace<IconView>(Icon::terminal, 14.0f, Tone::muted);
        title_ = &emplace<Label>("", TextStyle::body, Tone::normal, "title");
        title_->style().shrink = 1;
        title_->style().grow = 1;
        title_->style().basis = 0.0f;
        auto cb = Button::icon_button(Icon::close, std::string(tr("Close Tab")), [this] { mv_.view().close_tab(tab_); }, "close", 18);
        close_ = &add(std::move(cb));
        close_->set_opacity(0);
        close_fade_.on_value([this](float v) { close_->set_opacity(v); });
    }
    void update(const WorkspaceSnapshot& s, const ProjectItem& p, const TabItem& t, size_t index) {
        set_automation_id("tab[" + std::to_string(index + 1) + "]");
        title_->set_text(t.title.empty() ? std::string(tr("Shell")) : t.title);
        const std::string agent = agent_of(s, t);
        agent_ = agent;
        icon_->set_icon(t.panes.size() > 1 ? Icon::split_right : agent_icon(agent));
        if (agent.empty()) icon_->set_color_fn({});
        else icon_->set_color_fn([agent](const Theme& th) { return agent_color(th, agent); });
        attention_ = strongest(s, t.panes);
        set_selected(s.shown == p.id && p.active_tab == t.id);
        set_tooltip(t.title);
        invalidate_paint();
    }
    void hover_changed(bool in) override { close_fade_.set(in ? 1.0f : 0.0f, fade_tr()); }
    void click(const PointerEvent& e) override {
        if (e.clicks >= 2) {
            mv_.rename_tab_dialog(tab_);
            return;
        }
        mv_.view().select_tab(tab_);
        mv_.sync();
        mv_.focus_active_pane();
    }
    void context(Vec2 at) override { mv_.tab_menu(tab_, at); }
    void paint_over(PaintContext& p) const override {
        if (attention_ == Attention::idle) return;
        const Rect ib = icon_->frame();
        const Color under = selected_ ? over(p.theme.fg.with_alpha(0.10f), p.theme.sidebar_bg) : p.theme.sidebar_bg;
        status_dot(p, ib.origin(), ib.w, attention_color(p.theme, attention_), under);
    }
    TabKey tab() const { return tab_; }

private:
    TabKey tab_;
    IconView* icon_ = nullptr;
    Label* title_ = nullptr;
    Button* close_ = nullptr;
    Animated close_fade_;
    Attention attention_ = Attention::idle;
    std::string agent_;
};

class ProjectRow final : public SidebarRow {
public:
    ProjectRow(MainView& mv, Sidebar& sb, ProjectKey p) : SidebarRow(mv, sb, "row"), project_(p), plus_fade_(*this) {
        key = p;
        style().padding = Insets{10, 0, 10, 0};
        chevron_ = &emplace<IconView>(Icon::chevron_down, 12.0f, Tone::dim);
        folder_ = &emplace<IconView>(Icon::folder, 16.0f, Tone::muted);
        name_ = &emplace<Label>("", TextStyle::body_bold, Tone::normal, "name");
        name_->style().shrink = 1;
        name_->style().grow = 1;
        name_->style().basis = 0.0f;
        count_ = &emplace<Label>("", TextStyle::small, Tone::dim, "count");
        auto plus = Button::icon_button(Icon::plus, std::string(tr("New Tab")), [this] {
            mv_.view().new_tab(project_);
            mv_.sync();
            mv_.focus_active_pane();
        }, "new_tab", 18);
        plus_ = &add(std::move(plus));
        plus_->set_opacity(0);
        plus_fade_.on_value([this](float v) { plus_->set_opacity(v); });
    }
    void update(const WorkspaceSnapshot& s, const ProjectItem& p) {
        expanded_ = p.expanded;
        name_->set_text(p.name);
        chevron_->set_icon(p.expanded ? Icon::chevron_down : Icon::chevron_right);
        folder_->set_icon(p.scratch ? Icon::terminal : Icon::folder);
        if (p.color != tag_) {
            tag_ = p.color;
            if (tag_ == ProjectColor::none) folder_->set_color_fn({});
            else folder_->set_color_fn([c = tag_](const Theme& th) { return th.tag(c); });
        }
        std::vector<PaneKey> all;
        for (const auto& t : p.tabs) all.insert(all.end(), t.panes.begin(), t.panes.end());
        count_->set_text(std::to_string(p.tabs.size()));
        count_->set_visible(!p.expanded);
        attention_ = strongest(s, all);
        set_selected(s.shown == p.id && (!p.expanded || p.tabs.empty()));
        set_tooltip(home_relative(p.path, s.home));
        invalidate_paint();
    }
    void hover_changed(bool in) override { plus_fade_.set(in ? 1.0f : 0.0f, fade_tr()); }
    void click(const PointerEvent& e) override {
        const Vec2 l = to_local(e.pos);
        if (l.x < chevron_->frame().right() + 3 || e.clicks >= 2) {
            mv_.view().set_expanded(project_, !expanded_);
            mv_.sync();
            return;
        }
        mv_.view().show_project(project_);
        mv_.sync();
        mv_.focus_active_pane();
    }
    void context(Vec2 at) override { mv_.project_menu(project_, at); }
    void paint_over(PaintContext& p) const override {
        if (attention_ == Attention::idle || expanded_) return;
        const Rect ib = folder_->frame();
        status_dot(p, ib.origin(), ib.w, attention_color(p.theme, attention_), p.theme.sidebar_bg);
    }
    ProjectKey project() const { return project_; }

private:
    ProjectKey project_;
    IconView* chevron_ = nullptr;
    IconView* folder_ = nullptr;
    Label* name_ = nullptr;
    Label* count_ = nullptr;
    Button* plus_ = nullptr;
    Animated plus_fade_;
    bool expanded_ = true;
    ProjectColor tag_ = ProjectColor::none;
    Attention attention_ = Attention::idle;
};

// A project with its tab rows below (automation: "project[name]/tab[2]").
class ProjectSection final : public Widget {
public:
    ProjectSection(MainView& mv, Sidebar& sb, ProjectKey p) : Widget("project"), mv_(mv), sb_(sb), project_(p) {
        style().dir = LayoutStyle::Dir::Column;
        row_ = &emplace<ProjectRow>(mv, sb, p);
        tabs_ = &add(column("tabs"));
    }
    void update(const WorkspaceSnapshot& s, const ProjectItem& p) {
        set_automation_id("project[" + p.name + "]");
        row_->update(s, p);
        tabs_->set_visible(p.expanded);
        std::vector<uint64_t> keys;
        for (const auto& t : p.tabs) keys.push_back(t.id);
        reconcile(
            *tabs_, keys, [](const Widget& w) { return static_cast<const TabRow&>(w).tab(); },
            [this](uint64_t k) { return std::make_unique<TabRow>(mv_, sb_, k); },
            [&](Widget& w, uint64_t k) {
                for (size_t i = 0; i < p.tabs.size(); ++i) {
                    if (p.tabs[i].id == k) static_cast<TabRow&>(w).update(s, p, p.tabs[i], i);
                }
            });
    }
    ProjectKey project() const { return project_; }
    Widget& tabs() { return *tabs_; }
    ProjectRow& row_widget() { return *row_; }

private:
    MainView& mv_;
    Sidebar& sb_;
    ProjectKey project_;
    ProjectRow* row_ = nullptr;
    Box* tabs_ = nullptr;
};

// ---------------------------------------------------------------- Sidebar

Sidebar::Sidebar(MainView& mv) : Widget("sidebar"), mv_(mv) {
    style().dir = LayoutStyle::Dir::Column;
    style().clip = true;
    style().shrink = 0;
    // Macterm's sidebar header: the toggle right of the traffic lights (macOS); on Windows and
    // Linux the app menu and the toggle in the same spot. Then the magnifier: Search History.
    auto top = row("sidebar_top", 4);
    top->style().height = 38.0f;
    top->style().shrink = 0;
    top->style().align_items = LayoutStyle::Align::Center;
    top->style().padding = Insets{mv.options().mac_titlebar ? 78.0f : 8.0f, 0, 8, 0};
    if (mv.options().in_window_menu) {
        auto m = Button::icon_button(Icon::menu, std::string(tr("Menu")), {}, "menu", 26);
        m->set_on_click([this] {
            if (mv_.hooks().app_menu) mv_.hooks().app_menu(menu_->bounds_in_window());
        });
        menu_ = &top->add(std::move(m));
    }
    auto tg = Button::icon_button(Icon::sidebar, std::string(tr("Hide Sidebar")), [this] { mv_.toggle_sidebar(); }, "toggle_sidebar", 26);
    toggle_ = &top->add(std::move(tg));
    auto se = Button::icon_button(Icon::search, std::string(tr("Search History")), [this] {
        if (mv_.hooks().search_history) mv_.hooks().search_history();
    }, "search_history", 26);
    search_ = &top->add(std::move(se));
    top_ = &add(std::move(top));
    auto list = column("projects");
    list->style().padding = Insets{0, 2, 0, 8};
    list_ = list.get();
    auto sv = std::make_unique<ScrollView>(std::move(list), "scroll");
    sv->style().grow = 1;
    sv->style().shrink = 1;
    sv->style().basis = 0.0f;
    scroll_ = &add(std::move(sv));
    auto foot = row("footer");
    foot->style().padding = Insets{8, 6, 8, 10};
    foot->style().shrink = 0;
    auto np = std::make_unique<Button>(std::string(tr("New Project")), [this] { mv_.view().new_project(); }, Button::Kind::plain,
                                       "new_project");
    np->set_icon(Icon::plus, 14);
    np->style().grow = 1;
    foot->add(std::move(np));
    add(std::move(foot));
}

bool Sidebar::is_caption(Vec2 local) const {
    if (!top_->frame().contains(local)) return false;
    const Vec2 l{local.x - top_->frame().x, local.y - top_->frame().y};
    for (const auto& c : top_->children()) {
        if (c->visible() && c->frame().contains(l)) return false;
    }
    return true;
}

void Sidebar::update(const WorkspaceSnapshot& s) {
    toggle_->set_checked(s.sidebar_visible);
    std::vector<uint64_t> keys;
    for (const auto& p : s.projects) keys.push_back(p.id);
    reconcile(
        *list_, keys, [](const Widget& w) { return static_cast<const ProjectSection&>(w).project(); },
        [this](uint64_t k) { return std::make_unique<ProjectSection>(mv_, *this, k); },
        [&](Widget& w, uint64_t k) {
            if (const ProjectItem* p = s.project(k)) static_cast<ProjectSection&>(w).update(s, *p);
        });
}

void Sidebar::paint(PaintContext& p) const {
    const Rect b = local_bounds();
    p.dl.rect(b, p.theme.sidebar_bg);
    p.dl.hairline(Rect{b.w - 1, 0, 1, b.h}, p.theme.sidebar_border);
}

Sidebar::Drop Sidebar::drop_for(Widget& row, Vec2 local) const {
    Drop d;
    const Vec2 lo = list_->bounds_in_window().origin(), me = bounds_in_window().origin();
    const float y = local.y + me.y - lo.y;   // in list coordinates
    const WorkspaceSnapshot& s = mv_.view().snapshot();
    if (auto* pr = dynamic_cast<ProjectRow*>(&row)) {
        (void)pr;
        // Between project sections.
        size_t idx = 0;
        float line = 0;
        for (size_t i = 0; i < list_->child_count(); ++i) {
            const Rect f = list_->child(i)->frame();
            if (y > f.y + f.h / 2) {
                idx = i + 1;
                line = f.bottom();
            } else if (i == 0) {
                line = f.y;
            }
        }
        d.valid = true;
        d.project = 0;
        d.index = idx;
        d.y = line + lo.y - me.y;
        d.x = 8;
        return d;
    }
    if (dynamic_cast<TabRow*>(&row)) {
        for (size_t i = 0; i < list_->child_count(); ++i) {
            auto* sec = static_cast<ProjectSection*>(list_->child(i));
            const Rect f = sec->frame();
            if (y < f.y || y >= f.bottom()) continue;
            const ProjectItem* p = s.project(sec->project());
            if (!p) return d;
            Widget& tabs = sec->tabs();
            size_t idx = 0;
            float line = f.y + sec->row_widget().frame().bottom();
            for (size_t k = 0; k < tabs.child_count(); ++k) {
                const Rect tf = tabs.child(k)->frame().translated({0, f.y + tabs.frame().y});
                if (y > tf.y + tf.h / 2) {
                    idx = k + 1;
                    line = tf.bottom();
                }
            }
            d.valid = true;
            d.project = p->id;
            d.index = idx;
            d.y = line + lo.y - me.y;
            d.x = 30;
            return d;
        }
    }
    return d;
}

void Sidebar::drag_move(Widget& row, Vec2 pos) {
    drag_row_ = &row;
    drop_ = drop_for(row, to_local(pos));
    invalidate_paint();
}

void Sidebar::drag_end(Widget& row, Vec2 pos, bool cancel) {
    Drop d = cancel ? Drop{} : drop_for(row, to_local(pos));
    drag_row_ = nullptr;
    drop_ = {};
    invalidate_paint();
    if (!d.valid) return;
    const WorkspaceSnapshot& s = mv_.view().snapshot();
    if (auto* pr = dynamic_cast<ProjectRow*>(&row)) {
        const ProjectKey k = pr->project();
        size_t from = 0;
        for (size_t i = 0; i < s.projects.size(); ++i) {
            if (s.projects[i].id == k) from = i;
        }
        size_t to = d.index > from ? d.index - 1 : d.index;
        if (to != from) mv_.view().move_project(k, to);
    } else if (auto* tr_ = dynamic_cast<TabRow*>(&row)) {
        const ProjectItem* owner = nullptr;
        s.tab(tr_->tab(), &owner);
        size_t to = d.index;
        if (owner && owner->id == d.project) {
            size_t from = 0;
            for (size_t i = 0; i < owner->tabs.size(); ++i) {
                if (owner->tabs[i].id == tr_->tab()) from = i;
            }
            if (to > from) --to;
            if (to == from) return;
        }
        mv_.view().move_tab(tr_->tab(), d.project, to);
    }
    mv_.sync();
}

void Sidebar::paint_over(PaintContext& p) const {
    if (!drag_row_ || !drop_.valid) return;
    const Rect b = local_bounds();
    p.dl.round_rect(Rect{drop_.x, drop_.y - 1, b.w - drop_.x - 10, 2}, 1, p.theme.accent);
    p.dl.round_rect(Rect{drop_.x - 3, drop_.y - 3, 6, 6}, 3, {}, 1.5f, p.theme.accent);
}

// ---------------------------------------------------------------- title bar: title and indicator

std::string agent_state(const PaneItem& p) {
    if (p.attention == Attention::bell || p.agent_activity == 2) return std::string(tr("waiting for input"));
    if (p.agent_activity == 1) return std::string(tr("working"));
    if (p.res.valid && p.res.cpu >= 3.0f) return std::string(tr("running"));
    if (p.attention == Attention::failed) return std::string(tr("failed"));
    if (p.attention == Attention::done) return std::string(tr("done"));
    return std::string(tr("idle"));
}

std::string format_context_percent(float pct) {
    if (pct < 0) return {};
    if (pct > 0 && pct < 1) return "<1%";
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", static_cast<double>(std::min(pct, 999.0f)));
    return b;
}

std::string format_tokens(uint64_t n) {
    char b[32];
    if (n >= 1'000'000) {
        if (n % 1'000'000 == 0) std::snprintf(b, sizeof b, "%lluM", static_cast<unsigned long long>(n / 1'000'000));
        else std::snprintf(b, sizeof b, "%.2gM", static_cast<double>(n) / 1e6);
    } else if (n >= 100'000) {
        std::snprintf(b, sizeof b, "%lluk", static_cast<unsigned long long>((n + 500) / 1000));
    } else if (n >= 1000) {
        std::snprintf(b, sizeof b, "%.1fk", static_cast<double>(n) / 1000.0);
    } else {
        std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(n));
    }
    return b;
}

std::string agent_title(const std::string& id) {
    if (id == "claude") return "Claude Code";
    if (id == "codex") return "Codex";
    if (id == "gemini") return "Gemini CLI";
    if (id == "qoder" || id == "qodercli") return "Qoder";
    if (id == "opencode") return "opencode";
    if (id == "aider") return "Aider";
    if (id.empty()) return {};
    std::string s = id;
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

std::string context_line(const AgentStats& a) {
    if (!a.used_tokens && !a.window_tokens) return {};
    std::string s = a.used_tokens ? format_tokens(a.used_tokens) : std::string("?");
    if (a.window_tokens) s += " / " + format_tokens(a.window_tokens);
    if (a.percent >= 0) s += " (" + format_context_percent(a.percent) + ")";
    return s;
}

std::string format_uptime(uint64_t s) {
    char buf[32];
    if (s < 60) std::snprintf(buf, sizeof buf, "%llus", static_cast<unsigned long long>(s));
    else if (s < 3600) std::snprintf(buf, sizeof buf, "%llum", static_cast<unsigned long long>(s / 60));
    else if (s < 86400) std::snprintf(buf, sizeof buf, "%lluh %llum", static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60));
    else std::snprintf(buf, sizeof buf, "%llud %lluh", static_cast<unsigned long long>(s / 86400), static_cast<unsigned long long>(s / 3600 % 24));
    return buf;
}

namespace {

constexpr size_t kIndicatorHistory = 30;
constexpr float kIndicatorSpark = 30, kIndicatorIcon = 12, kIndicatorIconGap = 4, kIndicatorGap = 12, kIndicatorPad = 8;
const char* const kDash = "\xE2\x80\x94";

// The widest text each slot can hold (digits of UI fonts are tabular).
constexpr std::string_view kCpuWidest[] = {"999%", "9.9%", "9.9k%"};
constexpr std::string_view kMemWidest[] = {"9999 MB", "999.9 MB", "99.9 GB", "999 KB"};
constexpr std::string_view kCtxWidest[] = {"ctx 100%"};

}  // namespace

ResourceIndicator::ResourceIndicator(MainView& mv) : Widget("indicator"), mv_(mv) {
    style().shrink = 0;
}

void ResourceIndicator::update(const WorkspaceSnapshot& s) {
    const TabItem* t = s.shown_tab();
    tab_ = t ? t->id : 0;
    Resources sum{};
    float ctx = -1;
    std::string agent, state;
    uint32_t procs = 0;
    const PaneItem* fp = t ? s.pane(t->focused) : nullptr;
    if (t) {
        for (PaneKey k : t->panes) {
            const PaneItem* p = s.pane(k);
            if (!p) continue;
            procs += p->processes;
            if (p->res.valid) {
                sum.valid = true;
                sum.memory += p->res.memory;
                sum.cpu += p->res.cpu;
            }
            // The context of the focused pane's agent, else of any agent in the tab.
            if (!p->agent.empty() && (agent.empty() || p == fp)) {
                agent = p->agent;
                ctx = p->context;
                state = agent_state(*p);
            }
        }
    }
    const size_t shown_before = shown_segments();
    cpu_ = sum.valid ? compact_cpu(sum) : std::string();
    mem_ = sum.valid ? format_bytes(sum.memory) : std::string();
    agent_ = !agent.empty();
    // "ctx 19%": the word keeps it apart from the CPU figure.
    ctx_ = agent_ && ctx >= 0 ? "ctx " + format_context_percent(ctx * 100) : std::string();
    state_ = state;
    if (hist_tab_ != tab_) {
        hist_tab_ = tab_;
        cpu_hist_.clear();
        last_ = {};
    }
    if (sum.valid && (!last_.valid || sum.cpu != last_.cpu || sum.memory != last_.memory)) {
        cpu_hist_.push_back(sum.cpu);
        if (cpu_hist_.size() > kIndicatorHistory) cpu_hist_.erase(cpu_hist_.begin());
        last_ = sum;
    }
    // The tooltip names every figure of the header in words; the popup has the rest.
    std::string tip;
    auto add = [&](const std::string& line) {
        if (line.empty()) return;
        if (!tip.empty()) tip += "\n";
        tip += line;
    };
    if (fp && !fp->agent.empty()) {
        std::string head = agent_title(fp->agent);
        if (!fp->model.empty()) head += " \xC2\xB7 " + fp->model;
        head += " \xC2\xB7 " + agent_state(*fp);
        add(head);
        const std::string cl = context_line(fp->agent_stats);
        add(std::string(tr("Context: ")) + (cl.empty() ? std::string(tr("no figures yet (after the agent's first reply)")) : cl));
    } else if (fp) {
        add(!fp->process.empty() ? fp->process : std::string(tr("Shell")));
    }
    if (sum.valid) {
        std::string line = std::string(tr("CPU ")) + compact_cpu(sum) + " \xC2\xB7 " + std::string(tr("Memory ")) + format_bytes(sum.memory);
        if (procs) line += " \xC2\xB7 " + std::to_string(procs) + " " + std::string(procs == 1 ? tr("process") : tr("processes"));
        add(line);
    }
    if (fp && fp->uptime_s) add(std::string(tr("Running for ")) + format_uptime(fp->uptime_s));
    add(std::string(tr("Click for processes and agent details")));
    set_tooltip(tip);
    if (shown_segments() != shown_before) invalidate_layout();   // the header gives the room back
    invalidate_paint();
}

size_t ResourceIndicator::shown_segments() const {
    // CPU and memory always (their dashes until the first figures); the agent's context and
    // state only up to the last one with a value, so a shell or an agent without figures does
    // not end in a row of widely spaced dashes.
    if (!state_.empty()) return 4;
    if (!ctx_.empty()) return 3;
    return 2;
}

ResourceIndicator::Layout ResourceIndicator::layout_for(ITextService& text, const Theme& t, float scale, float height) const {
    const FontRole f = role_for(t, TextStyle::small);
    const FontMetrics m = text.metrics(f, scale);
    Layout l;
    const float content = m.ascent + m.descent;
    l.height = std::max(24.0f, std::ceil(std::max(content, m.line)) + 8);
    const float h = height > 0 ? height : l.height;
    // One baseline for every segment, the text block centered in the widget.
    l.baseline = px_round((h - content) / 2 + m.ascent, scale);
    l.xheight = std::max(4.0f, px_round(content * 0.42f, scale));
    float x = kIndicatorPad;
    auto widest = [&](auto const& list) {
        float w = 0;
        for (std::string_view v : list) w = std::max(w, text.shape(v, f, scale)->width());
        return std::ceil(w);
    };
    auto add = [&](Icon ic, const std::string& v, float slot, bool spark, bool numeric) {
        Segment s;
        s.icon = ic;
        s.dim = v.empty();
        s.run = text.shape(v.empty() ? std::string_view(kDash) : std::string_view(v), f, scale);
        s.x = x;
        s.slot = slot;
        s.spark = spark;
        s.numeric = numeric;
        x += (ic != Icon::none ? kIndicatorIcon + kIndicatorIconGap : 0) + slot + (spark ? 6 + kIndicatorSpark : 0) + kIndicatorGap;
        l.segs.push_back(std::move(s));
    };
    add(Icon::cpu, cpu_, widest(kCpuWidest), true, true);
    add(Icon::memory, mem_, widest(kMemWidest), false, true);
    const size_t n = shown_segments();
    if (n >= 3) add(Icon::agent, ctx_, widest(kCtxWidest), false, true);
    if (n < 4) {
        l.width = std::ceil(x - kIndicatorGap + kIndicatorPad);
        return l;
    }
    const std::string states[] = {agent_state(PaneItem{}), std::string(tr("running")), std::string(tr("failed")),
                                  std::string(tr("done")), std::string(tr("waiting for input")), std::string(tr("working"))};
    float sw = 0;
    for (const auto& st : states) sw = std::max(sw, text.shape(st, f, scale)->width());
    add(Icon::none, state_, std::ceil(sw), false, false);
    l.width = std::ceil(x - kIndicatorGap + kIndicatorPad);
    return l;
}

std::vector<float> ResourceIndicator::segment_x() const {
    std::vector<float> out;
    Window* w = window();
    if (!w) return out;
    for (const Segment& s : layout_for(w->text(), w->theme(), w->scale(), frame().h).segs) out.push_back(s.x);
    return out;
}

Size ResourceIndicator::intrinsic_size(Size) const {
    Window* w = window();
    if (!w) return {340, 24};
    const Layout l = layout_for(w->text(), w->theme(), w->scale(), 0);
    return {l.width, l.height};
}

void ResourceIndicator::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    if (hovered() || pressed_) p.dl.round_rect(b, t.radius_control, t.fg.with_alpha(pressed_ ? 0.10f : 0.06f));
    const Layout l = layout_for(p.text, t, p.scale, b.h);
    // Icons and the sparkline sit on the middle of the x-height, centered with the digits.
    const float mid = l.baseline - l.xheight / 2;
    for (const Segment& s : l.segs) {
        float tx = s.x;
        if (s.icon != Icon::none) {
            p.dl.icon(s.icon, kIndicatorIcon, {px_round(s.x, p.scale), px_round(mid - kIndicatorIcon / 2, p.scale)}, t.fg_muted);
            tx += kIndicatorIcon + kIndicatorIconGap;
        }
        // Numbers right-aligned in their slot, labels left-aligned; the dash of an absent value centered.
        float vx = tx;
        if (s.dim) vx = tx + (s.slot - s.run->width()) / 2;
        else if (s.numeric) vx = tx + s.slot - s.run->width();
        p.dl.text(s.run, {px_round(vx, p.scale), l.baseline}, s.dim ? t.fg_dim : t.fg);
        if (s.spark) {
            const float sh = l.xheight;
            const Rect sb{px_round(tx + s.slot + 6, p.scale), l.baseline - sh, kIndicatorSpark, sh};
            const float px1 = 1.0f / p.scale;
            p.dl.rect(Rect{sb.x, sb.bottom() - px1, sb.w, px1}, t.success.with_alpha(0.25f));
            float mx = 25;   // a calm scale: small loads stay small
            for (float v : cpu_hist_) mx = std::max(mx, v);
            const float col = sb.w / static_cast<float>(kIndicatorHistory);
            const size_t n = cpu_hist_.size();
            for (size_t i = 0; i < n; ++i) {
                const float f = std::clamp(cpu_hist_[i] / mx, 0.0f, 1.0f);
                if (f <= 0) continue;
                const float h = std::max(px1, px_round(f * sb.h, p.scale));
                const float x = sb.right() - static_cast<float>(n - i) * col;
                p.dl.rect(Rect{px_round(x, p.scale), sb.bottom() - h, std::max(px1, col - px1), h}, t.success.with_alpha(i + 1 == n ? 0.95f : 0.55f));
            }
        }
    }
}

EventResult ResourceIndicator::on_pointer(PointerEvent& e) {
    if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left) {
        pressed_ = true;
        invalidate_paint();
        // CPU and memory open the Processes page, the context and the agent state the Agent page.
        ResourceScope scope{ResourceScope::Kind::tab, tab_};
        const std::vector<float> xs = segment_x();
        scope.page = xs.size() >= 3 && e.local.x >= xs[2] - kIndicatorGap / 2 ? 2 : 1;
        if (mv_.hooks().show_resources && tab_) mv_.hooks().show_resources(scope, bounds_in_window(), false);
        return EventResult::Handled;
    }
    if (e.type == PointerEvent::Type::Release) {
        pressed_ = false;
        invalidate_paint();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void ResourceIndicator::on_hover(bool in) {
    Widget::on_hover(in);
    if (!in) pressed_ = false;
    invalidate_paint();
}

std::string ResourceIndicator::describe() const {
    auto v = [](const std::string& x) { return x.empty() ? std::string(kDash) : x; };
    return "CPU " + v(cpu_) + " | " + v(mem_) + " | context " + v(ctx_) + " | " + v(state_);
}

// ---------------------------------------------------------------- title bar

TitleBar::TitleBar(MainView& mv) : Widget("titlebar"), mv_(mv) {
    style().dir = LayoutStyle::Dir::Row;
    style().align_items = LayoutStyle::Align::Center;
    style().height = 38.0f;
    style().shrink = 0;
    style().gap = 4;
    style().padding = Insets{8, 0, 12, 0};
    auto lead = std::make_unique<Widget>("lead");
    lead->style().width = 0.0f;
    lead->set_hit_testable(false);
    lead_ = &add_child(std::move(lead));
    // With the sidebar hidden its header buttons sit at the left edge of the title bar.
    if (mv.options().in_window_menu) {
        auto m = Button::icon_button(Icon::menu, std::string(tr("Menu")), {}, "menu", 26);
        m->set_on_click([this] {
            if (mv_.hooks().app_menu) mv_.hooks().app_menu(menu_->bounds_in_window());
        });
        menu_ = &add(std::move(m));
    }
    auto sb = Button::icon_button(Icon::sidebar, std::string(tr("Show Sidebar")), [this] { mv_.toggle_sidebar(); }, "toggle_sidebar", 26);
    sidebar_btn_ = &add(std::move(sb));
    auto se = Button::icon_button(Icon::search, std::string(tr("Search History")), [this] {
        if (mv_.hooks().search_history) mv_.hooks().search_history();
    }, "search_history", 26);
    search_btn_ = &add(std::move(se));
    // Macterm's two-line title: the project in bold, the folder below, dim.
    auto title = column("title", 0);
    title->style().shrink = 1;
    title->style().min_w = 0;
    title->style().margin.l = 4;
    title_ = &title->emplace<Label>("", TextStyle::body_bold, Tone::normal, "name");
    title_->set_elide(Label::Elide::end);
    title_->style().shrink = 1;
    title_->set_hit_testable(false);
    path_ = &title->emplace<Label>("", TextStyle::small, Tone::muted, "path");
    path_->set_elide(Label::Elide::middle);
    path_->style().shrink = 1;
    // Hit-testable for its tooltip (the full path); is_caption keeps the rest of the title a drag area.
    title_box_ = &add(std::move(title));
    indicator_ = &emplace<ResourceIndicator>(mv);
    indicator_->style().margin.l = 12;
    add(spacer());
}

void TitleBar::update(const WorkspaceSnapshot& s) {
    const ProjectItem* proj = nullptr;
    const TabItem* t = s.shown_tab();
    if (t) (void)s.tab(t->id, &proj);
    const PaneItem* fp = t ? s.pane(t->focused) : nullptr;
    // Line 1: the project (the tab's title is in the sidebar); line 2: the focused pane's folder.
    // The fixed workspaces carry their English names (Scratch, Quick Terminal): translated here.
    std::string name = proj ? (proj->name.empty() ? std::string(tr("Scratch"))
                               : proj->scratch ? std::string(tr(proj->name))
                                               : proj->name)
                            : std::string("keepterm");
    title_->set_text(name);
    title_->set_tooltip(name);
    const std::string cwd = fp ? fp->cwd : (proj ? proj->path : std::string());
    path_->set_text(folder_label(cwd));   // the full path is the tooltip
    path_->set_tooltip(cwd);
    path_->set_visible(!cwd.empty());
    indicator_->update(s);
    const bool collapsed = !s.sidebar_visible;
    sidebar_btn_->set_visible(collapsed);
    search_btn_->set_visible(collapsed);
    if (menu_) menu_->set_visible(collapsed);
    const float lead = collapsed && mv_.options().mac_titlebar ? 70.0f : 0.0f;
    if (lead_->cstyle().width != lead) lead_->style().width = lead;
    // Macterm's inset: the title starts on the sidebar's content grid.
    style().padding.l = collapsed ? 8.0f : 16.0f;
}

void TitleBar::paint(PaintContext& p) const {
    const Rect b = local_bounds();
    p.dl.rect(b, p.theme.bg);
    p.dl.hairline(Rect{0, b.h - 1, b.w, 1}, p.theme.border);
}

bool TitleBar::is_caption(Vec2 local) const {
    if (!local_bounds().contains(local)) return false;
    for (const auto& c : children()) {
        if (!c->visible() || !c->hit_testable() || c.get() == lead_ || c.get() == title_box_) continue;
        if (c->frame().contains(local)) return false;
    }
    // The folder line shows its full path on hover, so it is not a drag area.
    if (path_->visible() && !path_->text().empty()) {
        const Rect tb = title_box_->frame(), pf = path_->frame();
        if (Rect{tb.x + pf.x, tb.y + pf.y, pf.w, pf.h}.contains(local)) return false;
    }
    return true;
}

EventResult TitleBar::on_pointer(PointerEvent& e) {
    if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left && is_caption(e.local)) {
        if (mv_.hooks().caption_press) mv_.hooks().caption_press(e.pos, e.clicks);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

// ---------------------------------------------------------------- status bar

StatusBar::StatusBar(MainView& mv) : Widget("status"), mv_(mv) {
    style().dir = LayoutStyle::Dir::Row;
    style().align_items = LayoutStyle::Align::Center;
    style().height = 24.0f;
    style().shrink = 0;
    style().gap = 6;
    style().padding = Insets{10, 0, 10, 0};
    path_ = &emplace<Label>("", TextStyle::small, Tone::muted, "sessions");
    path_->set_elide(Label::Elide::end);
    path_->style().shrink = 1;
    path_->style().grow = 1;
    path_->style().basis = 0.0f;
    attention_ = &emplace<Badge>("", Icon::bell, Tone::warning, "attention");
    attention_->set_visible(false);
    auto m = row("memory", 4);
    m->emplace<IconView>(Icon::memory, 12.0f, Tone::dim);
    mem_ = &m->emplace<Label>("", TextStyle::small, Tone::muted, "value");
    fixed_column(*mem_, {"9999 MB", "999.9 MB", "99.9 GB"});
    mem_spark_ = &m->emplace<Sparkline>(40, "spark");
    mem_spark_->style().width = 40.0f;
    mem_spark_->style().height = 12.0f;
    mem_spark_->set_tone(Tone::accent);
    m->set_tooltip(std::string(tr("Memory of all keepterm sessions and of keepterm itself. Click for the process tree.")));
    mem_row_ = &add(std::move(m));
    auto c = row("cpu", 4);
    c->style().margin.l = 6;
    c->emplace<IconView>(Icon::cpu, 12.0f, Tone::dim);
    cpu_ = &c->emplace<Label>("", TextStyle::small, Tone::muted, "value");
    fixed_column(*cpu_, {"9999%"});
    cpu_spark_ = &c->emplace<Sparkline>(40, "spark");
    cpu_spark_->style().width = 40.0f;
    cpu_spark_->style().height = 12.0f;
    cpu_spark_->set_tone(Tone::success);
    cpu_spark_->set_max(100.0f);
    c->set_tooltip(std::string(tr("CPU of all keepterm sessions and of keepterm itself. Click for the process tree.")));
    cpu_row_ = &add(std::move(c));
    auto st = row("storage", 5);
    st->style().margin.l = 6;
    storage_icon_ = &st->emplace<IconView>(Icon::storage, 12.0f, Tone::dim);
    gauge_ = &st->emplace<Gauge>("gauge");
    gauge_->style().width = 48.0f;
    gauge_->style().height = 6.0f;
    storage_ = &st->emplace<Label>("", TextStyle::small, Tone::muted, "value");
    st->set_tooltip(std::string(tr("History storage")));
    storage_row_ = &add(std::move(st));
    renderer_ = &emplace<Label>("", TextStyle::small, Tone::dim, "renderer");
    renderer_->style().margin.l = 6;
    set(StatusInfo{});   // dashes until the first figures arrive
}

void StatusBar::update(const WorkspaceSnapshot& s) {
    // The title bar shows the focused pane; the status bar is about everything keepterm runs.
    size_t tabs = 0;
    for (const auto& p : s.projects) tabs += p.tabs.size();
    path_->set_text(std::string(tr("All sessions")) + "  \xC2\xB7  " + std::to_string(s.panes.size()) + " " +
                    std::string(s.panes.size() == 1 ? tr("pane") : tr("panes")) + ", " + std::to_string(tabs) + " " +
                    std::string(tabs == 1 ? tr("tab") : tr("tabs")));
}

void StatusBar::arrange(const Rect& frame) {
    // Widths at the test font; the session summary takes what is left and elides.
    const float w = frame.w;
    renderer_->set_visible(w >= 720 && !renderer_->text().empty());
    storage_row_->set_visible(w >= 600 && storage_->text() != "");
    mem_spark_->set_visible(w >= 480);
    cpu_spark_->set_visible(w >= 480);
    cpu_row_->set_visible(w >= 330);
    Widget::arrange(frame);
}

EventResult StatusBar::on_pointer(PointerEvent& e) {
    if (e.type != PointerEvent::Type::Press || e.button != platform::MouseButton::left) return EventResult::Ignored;
    const Vec2 l = to_local(e.pos);
    const bool on_mem = mem_row_->frame().contains(l), on_cpu = cpu_row_->frame().contains(l);
    if ((on_mem || on_cpu || path_->frame().contains(l)) && mv_.hooks().show_resources) {
        const Widget* w = on_cpu ? cpu_row_ : on_mem ? mem_row_ : static_cast<Widget*>(path_);
        mv_.hooks().show_resources(ResourceScope{ResourceScope::Kind::all, 0, 1}, w->bounds_in_window(), true);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void StatusBar::set(const StatusInfo& s) {
    mem_->set_text(s.total.valid ? format_bytes(s.total.memory) : std::string("\xE2\x80\x94"));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f%%", static_cast<double>(s.total.cpu));
    cpu_->set_text(s.total.valid ? std::string(buf) : std::string("\xE2\x80\x94"));
    mem_spark_->set_values(s.mem_history);
    cpu_spark_->set_values(s.cpu_history);
    if (s.storage.valid) {
        gauge_->set_visible(true);
        gauge_->set_fraction(s.storage.limit ? static_cast<float>(static_cast<double>(s.storage.used) / static_cast<double>(s.storage.limit))
                                             : 0.0f);
        storage_->set_text(s.storage.label.empty() ? format_bytes(s.storage.used) : s.storage.label);
    } else {
        gauge_->set_visible(false);
        storage_->set_text(s.storage.label.empty() ? std::string("\xE2\x80\x94") : s.storage.label);
    }
    renderer_->set_text(s.renderer);
    attention_->set_visible(s.attention > 0);
    attention_->set_text(std::to_string(s.attention));
}

void StatusBar::paint(PaintContext& p) const {
    const Rect b = local_bounds();
    p.dl.rect(b, p.theme.bg);
    p.dl.hairline(Rect{0, 0, b.w, 1}, p.theme.border);
}

// ---------------------------------------------------------------- MainView

namespace {

class SidebarEdge final : public Widget {
public:
    explicit SidebarEdge(MainView& mv) : Widget("sidebar_edge"), mv_(mv) {
        style().position = LayoutStyle::Position::Absolute;
    }
    platform::CursorShape cursor_at(Vec2) const override { return platform::CursorShape::resize_ew; }
    EventResult on_pointer(PointerEvent& e) override {
        if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left) {
            mv_.begin_sidebar_resize();
            return EventResult::Handled;
        }
        if (e.type == PointerEvent::Type::Move && e.captured) {
            mv_.sidebar_resize(e.pos.x - mv_.bounds_in_window().x);
            return EventResult::Handled;
        }
        if (e.type == PointerEvent::Type::Release) {
            mv_.end_sidebar_resize();
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }

private:
    MainView& mv_;
};

}  // namespace

MainView::MainView(IWorkspaceView& view, MainViewHooks hooks, MainViewOptions opts)
    : Widget("main"), view_(view), hooks_(std::move(hooks)), opts_(opts), sidebar_w_(*this, 220, true) {
    style().dir = LayoutStyle::Dir::Row;
    style().grow = 1;
    sidebar_ = &emplace<Sidebar>(*this);
    auto content = column("content");
    content->style().grow = 1;
    content->style().shrink = 1;
    content->style().basis = 0.0f;
    titlebar_ = &content->emplace<TitleBar>(*this);
    auto sc = std::make_unique<SplitContainer>(*this);
    sc->style().basis = 0.0f;
    sc->style().shrink = 1;
    split_ = &content->add(std::move(sc));
    status_ = &content->emplace<StatusBar>(*this);
    status_->set_visible(opts.show_status);
    add(std::move(content));
    sidebar_edge_ = &emplace<SidebarEdge>(*this);
    const WorkspaceSnapshot& s = view.snapshot();
    sidebar_on_ = s.sidebar_visible;
    sidebar_target_ = s.sidebar_width;
    sidebar_w_.jump(sidebar_on_ ? sidebar_target_ : 0);
    sidebar_->style().width = sidebar_w_.value();
    sync();
}

MainView::~MainView() = default;

void MainView::arrange(const Rect& f) {
    // The sidebar width follows its animation.
    const float w = std::round(sidebar_w_.value());
    if (sidebar_->cstyle().width != w) sidebar_->style().width = w;
    sidebar_->set_visible(w > 0.5f);
    Widget::arrange(f);
    sidebar_edge_->set_visible(sidebar_on_ && w > 0.5f);
    sidebar_edge_->arrange(Rect{w - 4, 0, 8, f.h});
}

void MainView::sync() {
    const WorkspaceSnapshot& s = view_.snapshot();
    if (s.rev == rev_) return;
    rev_ = s.rev;
    sidebar_->update(s);
    titlebar_->update(s);
    status_->update(s);
    const TabItem* t = s.shown_tab();
    split_->update(s, t);
    if (!resizing_ && s.sidebar_width != sidebar_target_) {
        sidebar_target_ = s.sidebar_width;
        if (sidebar_on_) sidebar_w_.jump(sidebar_target_);
    }
    // Keyboard focus follows the model's focused pane of the shown tab.
    const TabKey tk = t ? t->id : 0;
    const PaneKey pk = t ? t->focused : 0;
    if (tk != shown_tab_ || pk != shown_focus_) {
        shown_tab_ = tk;
        shown_focus_ = pk;
        Window* w = window();
        Widget* f = w ? w->focused() : nullptr;
        const bool focus_in_terminals = !f || dynamic_cast<TerminalWidget*>(f) != nullptr;
        if (focus_in_terminals) focus_active_pane();
    }
    invalidate_layout();
}

void MainView::set_status(const StatusInfo& s) { status_->set(s); }

void MainView::toggle_sidebar() {
    sidebar_on_ = !sidebar_on_;
    sidebar_w_.set(sidebar_on_ ? sidebar_target_ : 0, Transition::spring(std::chrono::milliseconds(260)));
    view_.set_sidebar(sidebar_target_, sidebar_on_);
    rev_ = ~uint64_t{0};
    sync();
}

void MainView::begin_sidebar_resize() { resizing_ = true; }

void MainView::sidebar_resize(float width) {
    const Theme& t = window() ? window()->theme() : Theme{};
    sidebar_target_ = std::clamp(std::round(width), t.sidebar_min, std::min(t.sidebar_max, frame().w * 0.6f));
    sidebar_w_.jump(sidebar_target_);
    invalidate_layout();
}

void MainView::end_sidebar_resize() {
    resizing_ = false;
    view_.set_sidebar(sidebar_target_, sidebar_on_);
    sync();
}

TerminalWidget* MainView::terminal_widget(PaneKey k) const {
    PaneFrame* f = split_->frame_for(k);
    return f ? &f->terminal() : nullptr;
}

MainView::PaneDrop MainView::pane_drop_at(PaneKey dragged, Vec2 p) const {
    PaneDrop d;
    const PaneKey k = pane_at(p);
    if (!k || k == dragged || !split_) return d;
    const PaneFrame* f = split_->frame_for(k);
    if (!f) return d;
    const Rect r = f->bounds_in_window();
    if (r.w < 1 || r.h < 1) return d;
    // The nearest side, measured relative to the pane's size.
    const float dx = (p.x - (r.x + r.w / 2)) / r.w, dy = (p.y - (r.y + r.h / 2)) / r.h;
    if (std::abs(dx) >= std::abs(dy)) d.side = dx < 0 ? SplitDir::left : SplitDir::right;
    else d.side = dy < 0 ? SplitDir::up : SplitDir::down;
    switch (d.side) {
        case SplitDir::left: d.preview = Rect{r.x, r.y, r.w / 2, r.h}; break;
        case SplitDir::right: d.preview = Rect{r.x + r.w / 2, r.y, r.w / 2, r.h}; break;
        case SplitDir::up: d.preview = Rect{r.x, r.y, r.w, r.h / 2}; break;
        case SplitDir::down: d.preview = Rect{r.x, r.y + r.h / 2, r.w, r.h / 2}; break;
    }
    d.target = k;
    return d;
}

void MainView::set_pane_drop(const PaneDrop& d) {
    if (d.target == drop_.target && d.side == drop_.side && d.preview.x == drop_.preview.x && d.preview.y == drop_.preview.y &&
        d.preview.w == drop_.preview.w && d.preview.h == drop_.preview.h) {
        return;
    }
    drop_ = d;
    invalidate_paint();
}

void MainView::paint_over(PaintContext& p) const {
    if (!drop_.target) return;
    const Rect r = drop_.preview.translated(Vec2{-bounds_in_window().x, -bounds_in_window().y}).inset(Insets::all(3));
    p.dl.round_rect(r, 6, p.theme.accent.with_alpha(0.18f), 2, p.theme.accent.with_alpha(0.8f));
}

PaneKey MainView::pane_at(Vec2 p) const {
    const TabItem* t = view_.snapshot().shown_tab();
    if (!t) return 0;
    for (PaneKey k : t->panes) {
        if (t->zoomed && *t->zoomed != k) continue;   // the other panes are hidden
        const TerminalWidget* tw = terminal_widget(k);
        if (tw && tw->visible() && tw->bounds_in_window().contains(p)) return k;
    }
    return 0;
}

bool MainView::over_sidebar(Vec2 p) const {
    return sidebar_ && sidebar_on_ && sidebar_->visible() && sidebar_->bounds_in_window().contains(p);
}

PaneKey MainView::focused_pane() const {
    const TabItem* t = view_.snapshot().shown_tab();
    return t ? t->focused : 0;
}

void MainView::focus_active_pane() {
    Window* w = window();
    if (!w) return;
    sync();
    if (TerminalWidget* tw = terminal_widget(focused_pane())) w->set_focus(tw, false);
}

bool MainView::is_caption(Vec2 p) const {
    const Rect tb = titlebar_->bounds_in_window();
    if (tb.contains(p)) return titlebar_->is_caption({p.x - tb.x, p.y - tb.y});
    // The sidebar's header strip (under the traffic lights on macOS), not its buttons.
    if (sidebar_->visible()) {
        const Rect sb = sidebar_->bounds_in_window();
        return sidebar_->is_caption({p.x - sb.x, p.y - sb.y});
    }
    return false;
}

void MainView::project_menu(ProjectKey k, Vec2 at) {
    Window* w = window();
    const ProjectItem* p = view_.snapshot().project(k);
    if (!w || !p) return;
    std::vector<MenuItem> items;
    MenuItem nt;
    nt.label = std::string(tr("New Tab"));
    nt.action = [this, k] {
        view_.new_tab(k);
        sync();
        focus_active_pane();
    };
    items.push_back(nt);
    MenuItem cp;
    cp.label = std::string(tr("Copy Path"));
    cp.enabled = !p->path.empty();
    cp.action = [this, path = p->path] { view_.copy_text(path); };
    items.push_back(cp);
    MenuItem of;
#if defined(__APPLE__)
    of.label = std::string(tr("Show in Finder"));
#elif defined(_WIN32)
    of.label = std::string(tr("Show in Explorer"));
#else
    of.label = std::string(tr("Open in File Manager"));
#endif
    of.enabled = !p->path.empty();
    of.action = [this, path = p->path] { (void)view_.open_folder(path); };
    items.push_back(of);
    items.push_back(MenuItem::sep());
    MenuItem rn;
    rn.label = std::string(tr("Rename Project\xE2\x80\xA6"));
    rn.enabled = !p->scratch;
    rn.action = [this, k] { rename_project_dialog(k); };
    items.push_back(rn);
    MenuItem color;
    color.label = std::string(tr("Color"));
    color.enabled = !p->scratch;
    static constexpr std::pair<ProjectColor, const char*> kColors[] = {
        {ProjectColor::none, "None"},     {ProjectColor::red, "Red"},     {ProjectColor::orange, "Orange"},
        {ProjectColor::yellow, "Yellow"}, {ProjectColor::green, "Green"}, {ProjectColor::teal, "Teal"},
        {ProjectColor::blue, "Blue"},     {ProjectColor::purple, "Purple"}, {ProjectColor::pink, "Pink"},
        {ProjectColor::gray, "Gray"}};
    for (const auto& entry : kColors) {
        const ProjectColor pc = entry.first;
        MenuItem m;
        m.label = std::string(tr(entry.second));
        m.checked = p->color == pc;
        if (pc != ProjectColor::none) m.swatch = w->theme().tag(pc);
        m.action = [this, k, pc] {
            view_.set_project_color(k, pc);
            sync();
        };
        color.submenu.push_back(m);
    }
    items.push_back(color);
    items.push_back(MenuItem::sep());
    const auto& ps = view_.snapshot().projects;
    size_t idx = 0;
    for (size_t i = 0; i < ps.size(); ++i) {
        if (ps[i].id == k) idx = i;
    }
    MenuItem up;
    up.label = std::string(tr("Move Up"));
    up.enabled = idx > 0 && !p->scratch;
    up.action = [this, k, idx] {
        view_.move_project(k, idx - 1);
        sync();
    };
    items.push_back(up);
    MenuItem down;
    down.label = std::string(tr("Move Down"));
    down.enabled = idx + 1 < ps.size() && !ps[idx + 1].scratch && !p->scratch;
    down.action = [this, k, idx] {
        view_.move_project(k, idx + 1);
        sync();
    };
    items.push_back(down);
    items.push_back(MenuItem::sep());
    MenuItem rm;
    rm.label = std::string(tr("Remove Project\xE2\x80\xA6"));
    rm.enabled = !p->scratch;
    rm.action = [this, k] { confirm_remove_project(k); };
    items.push_back(rm);
    open_menu(*w, std::move(items), at);
}

void MainView::terminal_focused(PaneKey pane) {
    const TabItem* t = view_.snapshot().shown_tab();
    if (t && t->focused != pane) view_.focus_pane(pane);
    if (hooks_.pane_focused) hooks_.pane_focused(pane);
}

void MainView::pane_menu(PaneKey k, Vec2 at) {
    Window* w = window();
    if (!w || !hooks_.pane_menu_items) return;
    auto items = hooks_.pane_menu_items(k, true);
    if (!items.empty()) open_menu(*w, std::move(items), at);
}

void MainView::tab_menu(TabKey k, Vec2 at) {
    Window* w = window();
    const ProjectItem* owner = nullptr;
    const TabItem* t = view_.snapshot().tab(k, &owner);
    if (!w || !t || !owner) return;
    size_t idx = 0;
    for (size_t i = 0; i < owner->tabs.size(); ++i) {
        if (owner->tabs[i].id == k) idx = i;
    }
    std::vector<MenuItem> items;
    MenuItem rn;
    rn.label = std::string(tr("Rename Tab\xE2\x80\xA6"));
    rn.action = [this, k] { rename_tab_dialog(k); };
    items.push_back(rn);
    items.push_back(MenuItem::sep());
    MenuItem up;
    up.label = std::string(tr("Move Up"));
    up.enabled = idx > 0;
    up.action = [this, k, pk = owner->id, idx] {
        view_.move_tab(k, pk, idx - 1);
        sync();
    };
    items.push_back(up);
    MenuItem down;
    down.label = std::string(tr("Move Down"));
    down.enabled = idx + 1 < owner->tabs.size();
    down.action = [this, k, pk = owner->id, idx] {
        view_.move_tab(k, pk, idx + 1);
        sync();
    };
    items.push_back(down);
    MenuItem mv;
    mv.label = std::string(tr("Move to Project"));
    for (const auto& p : view_.snapshot().projects) {
        if (p.id == owner->id) continue;
        MenuItem m;
        m.label = p.name;
        if (p.color != ProjectColor::none) m.swatch = w->theme().tag(p.color);
        m.action = [this, k, pk = p.id, n = p.tabs.size()] {
            view_.move_tab(k, pk, n);
            sync();
        };
        mv.submenu.push_back(m);
    }
    mv.enabled = !mv.submenu.empty();
    items.push_back(mv);
    if (hooks_.pane_menu_items) {
        auto agent = hooks_.pane_menu_items(t->focused, false);
        if (!agent.empty()) {
            items.push_back(MenuItem::sep());
            for (auto& m : agent) items.push_back(std::move(m));
        }
    }
    items.push_back(MenuItem::sep());
    MenuItem cl;
    cl.label = std::string(tr("Close Tab"));
    cl.action = [this, k] {
        view_.close_tab(k);
        sync();
        focus_active_pane();
    };
    items.push_back(cl);
    open_menu(*w, std::move(items), at);
}

namespace {

void rename_dialog(Window& w, std::string title, std::string current, std::function<void(std::string)> done) {
    auto value = std::make_shared<std::string>(current);
    auto field = std::make_unique<TextField>(std::string(), "name");
    field->set_text(current);
    field->select_all();
    field->on_change = [value](const std::string& s) { *value = s; };
    TextField* raw = field.get();
    DialogSpec d;
    d.title = std::move(title);
    d.body = std::move(field);
    d.width = 340.0f;
    d.buttons.push_back({std::string(tr("Cancel")), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({std::string(tr("Rename")), [value, done] { done(*value); }, Button::Kind::primary, true, false});
    show_dialog(w, std::move(d));
    w.set_focus(raw, true);
}

}  // namespace

void MainView::rename_project_dialog(ProjectKey k) {
    Window* w = window();
    const ProjectItem* p = view_.snapshot().project(k);
    if (!w || !p) return;
    rename_dialog(*w, std::string(tr("Rename Project")), p->name, [this, k](std::string name) {
        if (name.empty()) return;
        view_.rename_project(k, std::move(name));
        sync();
    });
}

void MainView::rename_tab_dialog(TabKey k) {
    Window* w = window();
    const TabItem* t = view_.snapshot().tab(k);
    if (!w || !t) return;
    rename_dialog(*w, std::string(tr("Rename Tab")), t->title, [this, k](std::string title) {
        view_.rename_tab(k, std::move(title));
        sync();
    });
}

void MainView::confirm_remove_project(ProjectKey k) {
    Window* w = window();
    const ProjectItem* p = view_.snapshot().project(k);
    if (!w || !p) return;
    DialogSpec d;
    d.title = std::string(tr("Remove \xE2\x80\x9C")) + p->name + "\xE2\x80\x9D?";
    d.message = std::string(tr("Its tabs close and their sessions end. The folder on disk stays."));
    d.buttons.push_back({std::string(tr("Cancel")), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({std::string(tr("Remove")),
                         [this, k] {
                             view_.remove_project(k);
                             sync();
                             focus_active_pane();
                         },
                         Button::Kind::danger, true, false});
    show_dialog(*w, std::move(d));
}

// ---------------------------------------------------------------- WorkspaceSnapshot

const ProjectItem* WorkspaceSnapshot::project(ProjectKey k) const {
    for (const auto& p : projects) {
        if (p.id == k) return &p;
    }
    return nullptr;
}

const TabItem* WorkspaceSnapshot::tab(TabKey k, const ProjectItem** owner) const {
    for (const auto& p : projects) {
        for (const auto& t : p.tabs) {
            if (t.id == k) {
                if (owner) *owner = &p;
                return &t;
            }
        }
    }
    return nullptr;
}

const TabItem* WorkspaceSnapshot::shown_tab() const {
    const ProjectItem* p = shown_project();
    if (!p) return nullptr;
    for (const auto& t : p->tabs) {
        if (t.id == p->active_tab) return &t;
    }
    return p->tabs.empty() ? nullptr : &p->tabs.front();
}

const PaneItem* WorkspaceSnapshot::pane(PaneKey k) const {
    auto it = panes.find(k);
    return it == panes.end() ? nullptr : &it->second;
}

}  // namespace kt::ui
