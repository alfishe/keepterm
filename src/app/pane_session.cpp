#include "app/pane_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "app/gui_input.h"
#include "base/log.h"
#include "ui/widgets.h"

namespace kt::app {

namespace {

using platform::Clock;

constexpr std::chrono::milliseconds kPublishRetry{4};   // skipped publish
constexpr std::chrono::milliseconds kCloseGrace{3000};

std::string_view as_chars(std::u8string_view s) { return {reinterpret_cast<const char*>(s.data()), s.size()}; }

}  // namespace

PaneSession::PaneSession(PaneEnv& env, ui::PaneKey key, text::FontSet& font, view::AtlasTextures& tex, view::ViewOptions vo,
                         vt::TerminalOptions to, PaneEvents events)
    : env_(env), key_(key), events_(std::move(events)), term_opts_(to) {
    tv_ = std::make_unique<view::TerminalView>(font, tex, vo);
    tv_->set_focused(false);
    const text::CellMetrics& m = font.metrics();
    // A first grid of 80x24 until the pane is laid out.
    tv_->set_bounds(render::IRect{0, 0, 80 * m.cell_w + 2 * vo.pad_x, 24 * m.cell_h + 2 * vo.pad_y});
    grid_ = tv_->grid_for_bounds();
    if (auto t = vt::Terminal::create(grid_, term_opts_, this)) {
        term_ = std::move(*t);
        modes_ = term_->input_modes();
        term_->set_default_cursor(cursor_shape_, cursor_blink_);
        term_->frames().set_waker([this] { wake(); });
    } else {
        log::error("gui", "terminal: " + t.error().what);
    }
}

PaneSession::~PaneSession() {
    *alive_ = false;
    close();
    if (retry_timer_ && env_.loop) env_.loop->cancel_timer(retry_timer_);
    stop_blink();
    tv_.reset();
}

void PaneSession::post(std::function<void()> fn) {
    if (!env_.loop) return;
    env_.loop->post([alive = alive_, fn = std::move(fn)] {
        if (*alive) fn();
    });
}

void PaneSession::wake() {
    dirty_ = true;
    if (wake_posted_.exchange(true)) return;
    post([this] {
        wake_posted_ = false;
        if (env_.request_frame) env_.request_frame();
    });
}

bool PaneSession::linked() const {
    std::lock_guard lk(link_mu_);
    return link_ != nullptr;
}

Result<void> PaneSession::attach(const std::string& session_id, const std::string& endpoint) {
#ifdef KT_HAVE_SESSIONS
    if (!term_) return make_error(Errc::invalid_argument, "no terminal");
    close();
    auto link = attach_session(session_id, endpoint, pty_size(grid_));
    if (!link) return Unexpected(link.error());
    mirror_ = true;   // before start(): the snapshot may arrive at once
    first_snapshot_seen_ = false;
    if (auto r = (*link)->start(*this); !r) {
        mirror_ = false;
        return r;
    }
    {
        std::lock_guard lk(link_mu_);
        link_ = std::move(*link);
    }
    session_ = session_id;
    exited_ = false;
    exit_code_.reset();
    return {};
#else
    (void)session_id;
    (void)endpoint;
    return make_error(Errc::not_supported, "sessions are not built in");
#endif
}

Result<void> PaneSession::start_local(pty::SpawnSpec spec) {
    if (!term_) return make_error(Errc::invalid_argument, "no terminal");
    close();
    spec.size = pty_size(grid_);
    auto link = spawn_local_pty(spec);
    if (!link) return Unexpected(link.error());
    mirror_ = false;
    {
        std::lock_guard lk(link_mu_);
        link_ = std::move(*link);
    }
    std::lock_guard lk(link_mu_);
    return link_->start(*this);
}

void PaneSession::close() {
    std::unique_ptr<ISessionLink> link;
    {
        std::lock_guard lk(link_mu_);
        link = std::move(link_);
    }
    // A session link only detaches; a local PTY hangs up its program.
    if (link) link->close(mirror_ ? std::chrono::milliseconds(0) : kCloseGrace);
}

void PaneSession::set_font_set(text::FontSet& f, float pad_x_dip, float pad_y_dip) {
    tv_->set_font_set(f);
    tv_->set_padding(static_cast<int32_t>(std::round(pad_x_dip * env_.scale)), static_cast<int32_t>(std::round(pad_y_dip * env_.scale)));
    full_next_ = true;
    relayout(false);
    dirty_ = true;
}

void PaneSession::set_theme(const view::Theme& t) {
    tv_->set_theme(t);
    full_next_ = true;
    dirty_ = true;
}

std::string PaneSession::title() const {
    std::lock_guard lk(title_mu_);
    return title_;
}

void PaneSession::set_title(std::string t) {
    {
        std::lock_guard lk(title_mu_);
        if (t == title_) return;
        title_ = std::move(t);
    }
    // Titles may change with every line of output; the window syncs once per burst.
    if (title_posted_.exchange(true)) return;
    post([this] {
        title_posted_ = false;
        if (events_.title_changed) events_.title_changed();
    });
}

vt::ThemeColors PaneSession::effective_colors() const {
    std::lock_guard lk(term_mu_);
    return term_ ? term_->effective_colors() : vt::ThemeColors{};
}

pty::GridSize PaneSession::pty_size(const vt::GridSize& g) {
    return pty::GridSize{g.cols, g.rows, static_cast<uint16_t>(std::min(65535, g.cols * g.cell_w_px)),
                         static_cast<uint16_t>(std::min(65535, g.rows * g.cell_h_px))};
}

bool PaneSession::send_key(const view::KeyInput& k) {
    std::string out;
    if (tv_->key(k, modes(), out) != view::InputResult::sent || out.empty()) return false;
    tv_->scroll_to(0);
    send(out);
    if (env_.request_frame) env_.request_frame();
    return true;
}

PaneSession::GridInfo PaneSession::grid_info() const {
    std::lock_guard lk(term_mu_);
    GridInfo g;
    if (!term_) return g;
    const view::TerminalLines src(*term_);
    g.cols = term_->size().cols;
    g.rows = term_->size().rows;
    const vt::CursorState cur = term_->cursor();
    g.cursor_col = cur.col;
    g.cursor_row = cur.row;
    const uint64_t screen_top = src.end_line() > g.rows ? src.end_line() - g.rows : src.first_line();
    g.scrollback_lines = screen_top > src.first_line() ? screen_top - src.first_line() : 0;
    g.alt_screen = term_->input_modes().alt_screen;
    g.scroll_offset = tv_->scroll_offset();
    return g;
}

std::string PaneSession::dump_text(bool scrollback, std::size_t max_lines) const {
    std::lock_guard lk(term_mu_);
    if (!term_) return {};
    const view::TerminalLines src(*term_);
    const uint64_t end = src.end_line();
    uint64_t first = scrollback ? src.first_line() : (end > term_->size().rows ? end - term_->size().rows : src.first_line());
    if (first < src.first_line()) first = src.first_line();
    if (end <= first || src.cols() == 0) return {};
    view::Selection sel;
    sel.anchor = view::SelPoint{first, 0};
    sel.head = view::SelPoint{end - 1, static_cast<uint16_t>(src.cols() - 1)};
    sel.mode = view::SelMode::Line;
    sel.active = true;
    std::string text = view::selection_text(src, sel);
    while (!text.empty() && text.back() == '\n') text.pop_back();   // the empty rows under the prompt
    // The newest max_lines lines of what is left.
    if (max_lines > 0) {
        std::size_t pos = text.size(), n = 0;
        while (pos > 0) {
            const std::size_t nl = text.rfind('\n', pos - 1);
            if (nl == std::string::npos) break;
            if (++n == max_lines) {
                text.erase(0, nl + 1);
                break;
            }
            pos = nl;
        }
    }
    return text;
}

bool PaneSession::wants_mouse() const {
    std::lock_guard lk(modes_mu_);
    return modes_.mouse_tracking != vt::MouseTracking::None;
}

vt::InputModes PaneSession::modes() {
    std::lock_guard lk(modes_mu_);
    return modes_;
}

void PaneSession::send(std::string_view bytes) {
    if (bytes.empty()) return;
    std::lock_guard lk(link_mu_);
    if (link_) link_->write(bytes);
}

// A session mirror only asks the host; the host's RESIZED resizes the mirror in stream order.
void PaneSession::relayout(bool force_link) {
    if (!term_ || bounds_.w <= 0 || bounds_.h <= 0) return;
    const vt::GridSize g = tv_->grid_for_bounds();
    if (g == grid_ && !force_link) return;
    grid_ = g;
    if (!mirror_) {
        std::lock_guard lk(term_mu_);
        term_->resize(g);
        term_->publish();
    }
    std::lock_guard lk(link_mu_);
    if (link_) link_->resize(pty_size(g));
}

// ---------------------------------------------------------------- ITerminalSurface

void PaneSession::set_bounds(render::IRect px) {
    if (px.x == bounds_.x && px.y == bounds_.y && px.w == bounds_.w && px.h == bounds_.h) return;
    bounds_ = px;
    tv_->set_bounds(px);
    full_next_ = true;
    relayout(false);
}

void PaneSession::paint(render::DrawList& out, bool full, std::vector<render::IRect>& damage) {
    dirty_ = false;
    if (bounds_.w <= 0 || bounds_.h <= 0) return;
    vt::Terminal* term = nullptr;
    {
        // term_ is swapped by snapshots on the I/O thread: read it only under the lock. Terminals
        // replaced while the last paint drew from them are freed here.
        std::lock_guard lk(term_mu_);
        graveyard_.clear();
        term = term_.get();
        painting_ = term;
    }
    if (!term) return;
    struct PaintDone {
        PaneSession& s;
        ~PaintDone() {
            std::lock_guard lk(s.term_mu_);
            s.painting_ = nullptr;
            s.graveyard_.clear();
        }
    } paint_done{*this};
    // Smooth scrolling moves the viewport a little each frame and asks for the next one.
    if (tv_->tick(std::chrono::steady_clock::now())) wake();
    vt::FramePublisher& frames = term->frames();
    const vt::Frame* f = frames.acquire();
    view::BuildResult br;
    bool built = false;
    {
        // Scrolled back, the view reads rows from the terminal's scrollback.
        std::unique_lock lk(term_mu_, std::defer_lock);
        const view::TerminalScrollback sb(*term);
        const view::ScrollbackSource* src = nullptr;
        if (tv_->scrolled_back()) {
            lk.lock();
            src = &sb;
        }
        if (f) {
            tv_->build(*f, out, br, src);
            built = true;
        } else {
            built = tv_->redraw(out, br, src);
        }
    }
    if (f) frames.release(f);
    if (!built) {
        // No frame yet: the terminal's background.
        out.quad(render::solid_quad(static_cast<float>(bounds_.x), static_cast<float>(bounds_.y), static_cast<float>(bounds_.w),
                                    static_cast<float>(bounds_.h), view::render_color(tv_->options().theme.background)));
        damage.push_back(bounds_);
        return;
    }
    if (br.incomplete) wake();   // the glyph rasterization budget ran out
    if (br.full || full_next_ || full) {
        damage.push_back(bounds_);
    } else {
        damage.insert(damage.end(), br.damage.begin(), br.damage.end());
    }
    full_next_ = false;
}

void PaneSession::set_focused(bool on) {
    if (on == focused_) return;
    focused_ = on;
    std::string out;
    tv_->focus(on, modes(), out);
    send(out);
    if (on) restart_blink();
    else stop_blink();
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
}

void PaneSession::key(const ui::KeyEvent& k) {
    if (k.composing || k.action == platform::KeyAction::release) return;
    restart_blink();
    if (copy_.active()) {
        view::CopyMode::Action a = view::CopyMode::Action::None;
        std::string copied;
        {
            std::lock_guard lk(term_mu_);
            if (!term_) return;
            const view::TerminalLines src(*term_);
            copy_.set_viewport(top_line(), grid_.rows);
            a = copy_.key(src, to_key_input(k));
            if (a == view::CopyMode::Action::Copy) {
                copied = view::selection_text(src, copy_.selection());
                copy_.end();
            } else if (a == view::CopyMode::Action::Next || a == view::CopyMode::Action::Previous) {
                if (!matches_.empty()) {
                    const auto n = static_cast<long>(matches_.size());
                    long i = cur_match_ ? static_cast<long>(*cur_match_) + (a == view::CopyMode::Action::Next ? 1 : -1) : n - 1;
                    cur_match_ = static_cast<size_t>(((i % n) + n) % n);
                    copy_.move_to(src, matches_[*cur_match_].start);
                }
            }
            if (copy_.active()) {
                const uint64_t screen = term_->first_abs_line();
                const uint64_t top = copy_.top_line();
                tv_->scroll_to(static_cast<uint32_t>(screen > top ? screen - top : 0));
            }
            update_highlights();
        }
        if (env_.consume_key) env_.consume_key();
        copy_text(copied);
        if (!copy_.active() && events_.copy_mode_changed) events_.copy_mode_changed();
        changed();
        return;
    }
    std::string out;
    if (tv_->key(to_key_input(k), modes(), out) != view::InputResult::sent) return;
    if (read_only_) {
        if (env_.consume_key) env_.consume_key();   // and no text event follows
        return;
    }
    if (key_is_plain_text(k, out)) return;   // on_text delivers it (an IME may take the key)
    if (env_.consume_key) env_.consume_key();
    tv_->scroll_to(0);
    if (!sel_.empty() && !selecting_) clear_selection();
    send(out);
    if (env_.request_frame) env_.request_frame();
}

void PaneSession::text(std::string_view t) {
    if (t.empty() || read_only_) return;
    restart_blink();
    tv_->scroll_to(0);
    send(t);
    if (env_.request_frame) env_.request_frame();
}

// ---------------------------------------------------------------- selection, find, copy mode

void PaneSession::changed() {
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
}

uint64_t PaneSession::top_line() const {
    const uint64_t screen = term_->first_abs_line();
    return screen > tv_->scroll_offset() ? screen - tv_->scroll_offset() : 0;
}

view::SelPoint PaneSession::point_at(ui::Vec2 pos) const {
    const view::CellHit h = tv_->hit(pos.x * env_.scale, pos.y * env_.scale);
    return view::SelPoint{top_line() + static_cast<uint64_t>(std::max(0, h.row)), static_cast<uint16_t>(std::max(0, h.col))};
}

void PaneSession::update_highlights() {
    std::vector<view::Highlight> h;
    if (!term_) return;
    const view::TerminalLines src(*term_);
    const view::Theme& th = tv_->options().theme;
    if (!matches_.empty()) {
        const uint32_t bg = th.background;
        const unsigned luma = ((bg >> 16) & 0xFF) * 3 + ((bg >> 8) & 0xFF) * 6 + (bg & 0xFF);
        const bool light = luma > 1280;   // of 2550
        view::match_highlights(src, matches_, cur_match_, light ? 0xFCE38A : 0x5C4A1E, light ? 0xF5A623 : 0x9A6A12, h);
    }
    const view::Selection& s = copy_.active() ? copy_.selection() : sel_;
    if (!s.empty()) view::selection_highlights(src, s, th.selection_background, th.selection_foreground, h);
    if (copy_.active()) {
        const view::SelPoint c = copy_.cursor();
        h.push_back(view::Highlight{c.line, c.col, static_cast<uint16_t>(c.col + 1), th.cursor, th.background});
    }
    tv_->set_highlights(std::move(h));
}

void PaneSession::reveal(uint64_t line) {
    const uint64_t screen = term_->first_abs_line();
    const uint64_t top = top_line();
    const auto rows = static_cast<uint64_t>(std::max<uint16_t>(1, grid_.rows));
    if (line >= top && line < top + rows) return;
    if (line >= screen) {
        tv_->scroll_to(0);
        return;
    }
    // Center the line when it is above the viewport.
    const uint64_t want_top = line > rows / 2 ? line - rows / 2 : 0;
    tv_->scroll_to(static_cast<uint32_t>(screen > want_top ? screen - want_top : 0));
}

void PaneSession::copy_text(const std::string& s) {
    if (s.empty() || !env_.clipboard) return;
    (void)env_.clipboard->write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
}

std::string PaneSession::selection_text() const {
    std::lock_guard lk(term_mu_);
    if (!term_ || sel_.empty()) return {};
    return view::selection_text(view::TerminalLines(*term_), sel_);
}

void PaneSession::select_all() {
    {
        std::lock_guard lk(term_mu_);
        if (!term_) return;
        const view::TerminalLines src(*term_);
        sel_.active = true;
        sel_.mode = view::SelMode::Char;
        sel_.anchor = {src.first_line(), 0};
        sel_.head = {src.end_line() - 1, static_cast<uint16_t>(src.cols() - 1)};
        update_highlights();
    }
    changed();
}

void PaneSession::clear_selection() {
    if (sel_.empty() && !selecting_) return;
    sel_ = {};
    selecting_ = false;
    {
        std::lock_guard lk(term_mu_);
        update_highlights();
    }
    changed();
}

size_t PaneSession::find(const std::string& query) {
    {
        std::lock_guard lk(term_mu_);
        if (!term_) return 0;
        matches_ = query.empty() ? std::vector<view::Match>{} : view::find_all(view::TerminalLines(*term_), query);
        cur_match_.reset();
        if (!matches_.empty()) {
            // The newest match at or above the viewport bottom, like a backward search.
            const uint64_t bottom = top_line() + grid_.rows;
            size_t i = matches_.size() - 1;
            while (i > 0 && matches_[i].start.line >= bottom) --i;
            cur_match_ = i;
            reveal(matches_[i].start.line);
        }
        update_highlights();
    }
    changed();
    return matches_.size();
}

void PaneSession::find_step(int delta) {
    if (matches_.empty()) return;
    {
        std::lock_guard lk(term_mu_);
        const auto n = static_cast<long>(matches_.size());
        long i = cur_match_ ? static_cast<long>(*cur_match_) + delta : (delta > 0 ? 0 : n - 1);
        i = ((i % n) + n) % n;
        cur_match_ = static_cast<size_t>(i);
        if (term_) {
            reveal(matches_[*cur_match_].start.line);
            update_highlights();
        }
    }
    changed();
}

void PaneSession::end_find() {
    if (matches_.empty()) return;
    matches_.clear();
    cur_match_.reset();
    {
        std::lock_guard lk(term_mu_);
        update_highlights();
    }
    changed();
}

void PaneSession::toggle_copy_mode() {
    {
        std::lock_guard lk(term_mu_);
        if (!term_) return;
        if (copy_.active()) {
            copy_.end();
        } else {
            const view::TerminalLines src(*term_);
            const vt::CursorState c = term_->cursor();
            const view::SelPoint at = tv_->scroll_offset() > 0 ? view::SelPoint{top_line(), 0}
                                                                : view::SelPoint{term_->first_abs_line() + c.row, c.col};
            copy_.begin(src, at, grid_.rows, top_line());
        }
        update_highlights();
    }
    if (events_.copy_mode_changed) events_.copy_mode_changed();
    changed();
}

void PaneSession::pointer(const ui::PointerEvent& e, ui::Vec2) {
    platform::MouseEvent m;
    using T = ui::PointerEvent::Type;
    using MT = platform::MouseEvent::Type;
    switch (e.type) {
        case T::Move: m.type = MT::move; break;
        case T::Press: m.type = MT::press; break;
        case T::Release: m.type = MT::release; break;
        case T::Enter: m.type = MT::enter; break;
        case T::Leave: m.type = MT::leave; break;
    }
    m.button = e.button;
    m.click_count = e.clicks;
    m.mods = e.mods;
    m.pos = platform::DipPoint{e.pos.x, e.pos.y};   // window DIPs: the view's bounds are window px
    std::string out;
    if (!read_only_ && tv_->pointer(to_pointer_input(m, env_.scale), modes(), out) == view::InputResult::sent) {
        hover_url_.clear();
        hover_at_.reset();
        send(out);
        return;
    }
    // Not reported to the program: selection and links.
    const bool primary = (e.mods & ui::primary_mod()) != 0;
    std::string url, shown;
    std::optional<view::PathRef> path;
    view::Match path_where;
    bool hover_only = false;
    std::string selected;   // copy on select: the text of a finished mouse selection
    {
        std::lock_guard lk(term_mu_);
        if (!term_) return;
        const view::TerminalLines src(*term_);
        const view::SelPoint p = point_at(e.pos);
        if (e.type == T::Press && e.button == platform::MouseButton::left) {
            if (primary) {
                view::Match where;
                url = view::url_at(src, p, &where);
                if (!url.empty()) {
                    view::Selection s;
                    s.anchor = where.start;
                    s.head = where.end;
                    s.active = true;
                    shown = view::selection_text(src, s);
                } else if (events_.resolve_path) {
                    path = view::path_at(src, p, &path_where);
                }
            } else {
                if (copy_.active()) copy_.move_to(src, p);
                const bool extend = (e.mods & platform::mod_shift) && e.clicks == 1 && !sel_.empty();
                sel_.active = true;
                sel_.mode = e.clicks >= 3 ? view::SelMode::Line
                            : e.clicks == 2 ? view::SelMode::Word
                            : (e.mods & platform::mod_alt) ? view::SelMode::Block
                                                           : view::SelMode::Char;
                if (!extend) sel_.anchor = p;
                sel_.head = p;
                selecting_ = true;
            }
        } else if (e.type == T::Move && selecting_) {
            sel_.head = p;
            hover_url_.clear();
        } else if (e.type == T::Release && selecting_) {
            selecting_ = false;
            sel_.head = p;
            if (sel_.empty()) sel_ = {};
            else if (env_.copy_on_select && env_.copy_on_select()) selected = view::selection_text(src, sel_);
        } else {
            // Hover: the link under the pointer for the preview (TerminalWidget asks for it).
            if (e.type == T::Move) {
                if (!(hover_at_ && *hover_at_ == p)) {
                    hover_url_ = view::url_at(src, p);
                    hover_path_.reset();
                    if (hover_url_.empty() && primary && events_.resolve_path) hover_path_ = view::path_at(src, p);
                }
                hover_at_ = p;
            } else if (e.type == T::Leave) {
                hover_url_.clear();
                hover_at_.reset();
            }
            hover_only = true;
        }
        if (!hover_only) update_highlights();
    }
    copy_text(selected);
    // Paths are resolved outside the terminal lock (a file system look-up).
    if (hover_path_ && events_.resolve_path) {
        const std::string abs = events_.resolve_path(hover_path_->path);
        hover_url_ = abs.empty() ? std::string() : abs + (hover_path_->line ? ":" + std::to_string(hover_path_->line) : std::string());
        hover_path_.reset();
    }
    if (hover_only) return;
    if (url.empty() && path && events_.open_path) {
        const std::string abs = events_.resolve_path(path->path);
        if (!abs.empty()) {
            hover_url_.clear();
            events_.open_path(abs, path->line, path->col);
        }
    }
    if (!url.empty() && events_.open_url) events_.open_url(url, shown);
    changed();
}

bool PaneSession::wheel(const ui::WheelEvent& e, ui::Vec2) {
    platform::ScrollEvent s;
    s.dx = e.dx;
    s.dy = e.dy;
    s.precise = e.precise;
    s.mods = e.mods;
    s.pos = platform::DipPoint{e.pos.x, e.pos.y};
    std::string out;
    switch (tv_->wheel(to_wheel_input(s, env_.scale), modes(), out)) {
        case view::InputResult::sent:
            if (!read_only_) send(out);
            return true;
        case view::InputResult::scrolled:
            dirty_ = true;
            if (env_.request_frame) env_.request_frame();
            return true;
        default: return false;
    }
}

ui::Rect PaneSession::caret_rect_dip() const {
    vt::CursorState c{};
    {
        std::lock_guard lk(term_mu_);
        if (term_) c = term_->cursor();
    }
    const render::IRect r = tv_->cell_rect(c.col, c.row);
    const float s = env_.scale > 0 ? env_.scale : 1;
    return ui::Rect{static_cast<float>(r.x - bounds_.x) / s, static_cast<float>(r.y - bounds_.y) / s, static_cast<float>(r.w) / s,
                    static_cast<float>(r.h) / s};
}

void PaneSession::preedit(std::string_view, int) {}   // preedit drawing: TerminalView has none yet

void PaneSession::paste(std::u8string_view text) {
    if (text.empty() || read_only_) return;
    std::string out;
    tv_->paste(text, modes(), out);
    send(out);
    if (env_.request_frame) env_.request_frame();
}

void PaneSession::scroll_page(int pages) {
    tv_->scroll_by_animated(pages * std::max<int32_t>(1, grid_.rows - 1));
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
}

void PaneSession::scroll_to_top() {
    tv_->scroll_to(0xFFFFFFFFu);
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
}

ui::ScrollMetrics PaneSession::scroll_metrics() const {
    const GridInfo g = grid_info();
    ui::ScrollMetrics m;
    m.history = g.alt_screen ? 0 : g.scrollback_lines;
    m.rows = g.rows;
    m.offset = static_cast<uint32_t>(std::min<uint64_t>(g.scroll_offset, m.history));
    m.fg = tv_->options().theme.foreground;
    return m;
}

void PaneSession::scroll_to_offset(uint32_t rows_up) {
    tv_->scroll_to(rows_up);
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
}

bool PaneSession::jump_to_prompt(int delta) {
    std::lock_guard lk(term_mu_);
    if (!term_ || term_->alt_active()) return false;
    const vt::Scrollback& sb = term_->scrollback();
    const vt::Screen& scr = term_->screen();
    const uint64_t first = sb.first_abs_line(), screen = term_->first_abs_line(), end = screen + scr.rows();
    auto prompt_at = [&](uint64_t abs) {
        const uint16_t f = abs < screen ? sb.line(static_cast<std::size_t>(abs - first)).flags
                                        : scr.flags(static_cast<uint16_t>(abs - screen));
        return (f & vt::kRowPromptStart) != 0;
    };
    const uint64_t top = std::max(top_line(), first);
    std::optional<uint64_t> hit;
    if (delta < 0) {
        for (uint64_t a = top; a > first && !hit;)
            if (prompt_at(--a)) hit = a;
    } else {
        for (uint64_t a = top + 1; a < end && !hit; ++a)
            if (prompt_at(a)) hit = a;
    }
    if (!hit) {
        bool any = false;
        for (uint64_t a = first; a < end && !any; ++a) any = prompt_at(a);
        if (!any) return false;
        if (delta > 0) scroll_to_bottom();   // past the last prompt: the live screen
        return true;
    }
    tv_->scroll_to(*hit >= screen ? 0u : static_cast<uint32_t>(std::min<uint64_t>(screen - *hit, 0xFFFFFFFEu)));
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
    return true;
}

void PaneSession::scroll_to_bottom() {
    tv_->scroll_to(0);
    dirty_ = true;
    if (env_.request_frame) env_.request_frame();
}

void PaneSession::clear_scrollback() {
    {
        std::lock_guard lk(term_mu_);
        if (term_) {
            term_->clear_scrollback();
            term_->publish();
        }
    }
    if (!read_only_) send("\x0c");   // the shell redraws its prompt at the top
}

// ---------------------------------------------------------------- TerminalSink (I/O thread)

void PaneSession::reply(std::span<const std::byte> bytes) {
    // A session host answers queries itself; the mirror must not answer twice.
    if (mirror_) return;
    send(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void PaneSession::title_changed(std::u8string_view title) { set_title(std::string(as_chars(title))); }

void PaneSession::bell() {
    post([this] {
        if (events_.bell) events_.bell();
    });
}

void PaneSession::notify(std::u8string_view title, std::u8string_view body) {
    post([this, t = std::string(as_chars(title)), b = std::string(as_chars(body))] {
        if (events_.notify) events_.notify(t, b);
    });
}

void PaneSession::command_finished(std::optional<int> code) {
    post([this, code] {
        if (events_.command_finished) events_.command_finished(code);
    });
}

void PaneSession::clipboard_write(vt::ClipboardSel, std::span<const std::byte> data) {   // OSC 52
    post([this, t = std::u8string(reinterpret_cast<const char8_t*>(data.data()), data.size())] {
        if (env_.clipboard) (void)env_.clipboard->write_text(platform::Selection::clipboard, t);
    });
}

void PaneSession::modes_changed(const vt::InputModes& m) {
    std::lock_guard lk(modes_mu_);
    modes_ = m;
}

// ---------------------------------------------------------------- SessionEvents (I/O thread)

void PaneSession::on_output(std::span<const std::byte> bytes) {
    bool retry = false;
    {
        std::lock_guard lk(term_mu_);
        term_->feed(bytes);   // publishes at the end of the batch; the waker posts a frame
        retry = term_->frames().pending() || term_->dec_mode(2026) == 1;
    }
    if (retry && !retry_posted_.exchange(true)) {
        post([this] {
            retry_posted_ = false;
            schedule_publish_retry();
        });
    }
}

// One timer, always at the earliest due time: a skipped publish is retried in 4 ms even while a
// synchronized update waits for its timeout, and every firing plans again, so changes held back
// by a 2026 update that began again before the timer fired are not left behind.
namespace {
constexpr auto kBlinkHalf = std::chrono::milliseconds(530);   // shown, then hidden, as long
}  // namespace

void PaneSession::set_cursor_defaults(vt::CursorShape shape, bool blink) {
    cursor_shape_ = shape;
    cursor_blink_ = blink;
    {
        std::lock_guard lk(term_mu_);
        if (!term_) return;
        term_->set_default_cursor(shape, blink);
        term_->publish();
    }
    restart_blink();
    wake();
}

void PaneSession::stop_blink() {
    if (blink_timer_ && env_.loop) env_.loop->cancel_timer(blink_timer_);
    blink_timer_ = 0;
    if (blink_hidden_ && tv_) {
        blink_hidden_ = false;
        tv_->set_cursor_blink_hidden(false);
        dirty_ = true;
    }
}

void PaneSession::restart_blink() {
    stop_blink();
    arm_blink();
}

// The next blink toggle. The view hides only a cursor whose state blinks
// (terminal.cursor_blink, DECSCUSR, mode 12); a steady one stays drawn.
void PaneSession::arm_blink() {
    if (!focused_ || !env_.loop || blink_timer_) return;
    blink_timer_ = env_.loop->add_timer(platform::Clock::now() + kBlinkHalf, [this, alive = alive_] {
        if (!*alive) return;
        blink_timer_ = 0;
        if (!focused_) return;
        blink_hidden_ = !blink_hidden_;
        tv_->set_cursor_blink_hidden(blink_hidden_);
        dirty_ = true;
        if (env_.request_frame) env_.request_frame();
        arm_blink();
    });
}

void PaneSession::schedule_publish_retry() {
    if (!term_ || !env_.loop) return;
    std::optional<Clock::time_point> due;
    {
        std::lock_guard lk(term_mu_);
        due = term_->publish_due(Clock::now(), kPublishRetry);
    }
    if (!due) return;
    if (retry_timer_) {
        if (retry_due_ <= *due) return;
        env_.loop->cancel_timer(retry_timer_);
    }
    retry_due_ = *due;
    retry_timer_ = env_.loop->add_timer(*due, [this, alive = alive_] {
        if (!*alive) return;
        retry_timer_ = 0;
        {
            std::lock_guard lk(term_mu_);
            term_->publish();
        }
        schedule_publish_retry();
    });
}

void PaneSession::on_exit(int code) {
    post([this, code] {
        exited_ = true;
        exit_code_ = code;
        dirty_ = true;
        if (events_.exited) events_.exited(code);
    });
}

void PaneSession::on_snapshot(std::vector<std::byte> ktsn, bool resync) {
    auto t = vt::Terminal::restore(ktsn, term_opts_, this);
    if (!t) {
        log::error("gui", "session snapshot: " + t.error().what);
        return;
    }
    (*t)->frames().set_waker([this] { wake(); });
    std::string title;
    {
        std::lock_guard lk(term_mu_);
        // A paint in progress may still draw from the old terminal; otherwise it goes now.
        if (term_.get() == painting_) graveyard_.push_back(std::move(term_));
        term_ = std::move(*t);
        term_->set_default_cursor(cursor_shape_, cursor_blink_);
        term_->publish();
        title = std::string(as_chars(term_->title()));
    }
    {
        std::lock_guard lk(modes_mu_);
        modes_ = term_->input_modes();
    }
    set_title(std::move(title));   // an event only if the snapshot brings another title
    post([this, resync] {
        full_next_ = true;
        if (!resync) relayout(true);   // our pane size, if we lead
        if (!first_snapshot_seen_) {
            first_snapshot_seen_ = true;
            if (events_.first_snapshot) events_.first_snapshot();
        }
        if (env_.request_frame) env_.request_frame();
    });
}

std::size_t PaneSession::retired_terminals() const {
    std::lock_guard lk(term_mu_);
    return graveyard_.size();
}

void PaneSession::on_resized(pty::GridSize size) {
    std::lock_guard lk(term_mu_);
    vt::GridSize g = term_->size();
    g.cols = size.cols;
    g.rows = size.rows;
    term_->resize(g);
    term_->publish();
}

void PaneSession::on_cwd(const std::string& path, const std::string& source) {
    post([this, path, source] {
        if (events_.cwd) events_.cwd(path, source);
    });
}

void PaneSession::on_host_lost(const std::string& why) {
    post([this, why] {
        if (events_.host_lost) events_.host_lost(why);
    });
}

}  // namespace kt::app
