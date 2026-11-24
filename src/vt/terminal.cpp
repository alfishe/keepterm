#include "vt/terminal.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>
#include <utility>

#include "vt/utf8.h"
#include "vt/width.h"

namespace kt::vt {
namespace {

constexpr std::chrono::milliseconds kSyncTimeout{200};

// DEC Special Graphics, 0x5F..0x7E.
constexpr std::array<char32_t, 32> kDecGraphics = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
    0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
    0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};

uint16_t clamp16(int v, int lo, int hi) { return static_cast<uint16_t>(std::clamp(v, lo, hi)); }

}  // namespace

// ------------------------------------------------------------------------------- lifecycle

Result<std::unique_ptr<Terminal>> Terminal::create(GridSize size, const TerminalOptions& opts, TerminalSink* sink) {
    if (size.cols == 0 || size.rows == 0) return make_error(Errc::invalid_argument, "grid size is zero");
    return std::unique_ptr<Terminal>(new Terminal(size, opts, sink));
}

Terminal::Terminal(GridSize size, const TerminalOptions& opts, TerminalSink* sink)
    : opts_(opts),
      sink_(sink),
      size_(size),
      parser_(ParserOptions{opts.max_osc_bytes}),
      primary_(size.cols, size.rows),
      active_(&primary_),
      scrollback_(opts.scrollback_lines, opts.scrollback_max_bytes) {
    full_reset();
}

Terminal::~Terminal() = default;

void Terminal::full_reset() {
    if (alt_) alt_.reset();
    active_ = &primary_;
    primary_.clear(Cell{});
    // The tables are kept: the scrollback survives RIS (xterm) and still references them.
    // Unused entries go at the next collection pass.
    x_ = y_ = 0;
    pending_wrap_ = false;
    shape_ = default_shape_;
    shape_default_ = true;
    pen_ = Pen{};
    link_ = 0;
    g_ = {};
    gl_ = 0;
    single_shift_ = 0;
    saved_ = {};
    last_cp_ = 0;
    title_stack_.clear();
    saved_dec_modes_.clear();
    rev_wrap_ext_ = false;
    dcs_kind_ = 0;
    dcs_buf_.clear();
    if (color_overrides_ != ThemeColors{}) {
        color_overrides_ = ThemeColors{};
        if (sink_) sink_->colors_changed();
    }
    reset_margins();
    reset_tabs();
    dec_.reset();
    dec_.set(static_cast<std::size_t>(dec_slot(7)));   // DECAWM
    dec_.set(static_cast<std::size_t>(dec_slot(25)));  // DECTCEM
    if (opts_.grapheme_clusters) dec_.set(static_cast<std::size_t>(dec_slot(2027)));
    if (default_blink_) dec_.set(static_cast<std::size_t>(dec_slot(12)));
    insert_mode_ = false;
    lnm_ = false;
    sync_ = false;
    modes_ = InputModes{};
    force_publish_ = true;
    notify_modes();
}

void Terminal::soft_reset() {
    // DECSTR (VT510): cursor visible, IRM, DECOM, DECCKM, DECKPAM off, margins full, SGR and
    // charsets default, saved cursor home. DECAWM is left as it is.
    dec_.set(static_cast<std::size_t>(dec_slot(25)));
    dec_.reset(static_cast<std::size_t>(dec_slot(6)));
    dec_.reset(static_cast<std::size_t>(dec_slot(1)));
    dec_.reset(static_cast<std::size_t>(dec_slot(66)));
    // xterm also resets reverse wraparound, and DEC STD 070 resets DECLRMM.
    dec_.reset(static_cast<std::size_t>(dec_slot(45)));
    dec_.reset(static_cast<std::size_t>(dec_slot(69)));
    rev_wrap_ext_ = false;
    insert_mode_ = false;
    modes_.cursor_keys_app = false;
    modes_.keypad_app = false;
    reset_margins();
    pen_ = Pen{};
    update_pen();
    g_ = {};
    gl_ = 0;
    single_shift_ = 0;
    saved_[alt_active() ? 1 : 0] = SavedCursor{};
    pending_wrap_ = false;
    notify_modes();
}

void Terminal::reset(ResetKind kind) {
    if (kind == ResetKind::Full) {
        full_reset();
    } else {
        soft_reset();
    }
}

void Terminal::clear_scrollback() {
    scrollback_.clear();
    force_publish_ = true;
}

void Terminal::set_scrollback_limit(uint32_t lines) {
    opts_.scrollback_lines = lines;
    scrollback_.set_limit(lines);
    force_publish_ = true;
}

void Terminal::set_scrollback_max_bytes(uint32_t bytes) {
    opts_.scrollback_max_bytes = bytes;
    scrollback_.set_max_bytes(bytes);
    force_publish_ = true;
}

void Terminal::reset_program_modes() {
    if (alt_active()) {
        // As DECRST 1049: back to the primary screen and the cursor saved there.
        active_ = &primary_;
        const SavedCursor& sc = saved_[0];
        if (sc.valid) {
            x_ = std::min<uint16_t>(sc.x, static_cast<uint16_t>(cols() - 1u));
            y_ = std::min<uint16_t>(sc.y, static_cast<uint16_t>(rows() - 1u));
            pen_.style = sc.style;
            pen_.prot = sc.prot;
            update_pen();
        }
        pending_wrap_ = false;
        primary_.mark_all_dirty();
    }
    alt_.reset();
    saved_[1] = SavedCursor{};
    for (uint16_t m : {uint16_t{1}, uint16_t{9}, uint16_t{47}, uint16_t{66}, uint16_t{1000}, uint16_t{1002},
                       uint16_t{1003}, uint16_t{1004}, uint16_t{1005}, uint16_t{1006}, uint16_t{1007},
                       uint16_t{1015}, uint16_t{1016}, uint16_t{1047}, uint16_t{1049}, uint16_t{2004},
                       uint16_t{2026}, uint16_t{9001}}) {
        dec_.reset(static_cast<std::size_t>(dec_slot(m)));
    }
    dec_.set(static_cast<std::size_t>(dec_slot(25)));
    sync_ = false;
    const InputModes old = modes_;
    modes_ = InputModes{};
    modes_.backarrow_sends_bs = old.backarrow_sends_bs;
    modes_.modify_other_keys = 0;
    force_publish_ = true;
    notify_modes();
}

FramePublisher& Terminal::frames() {
    if (!publisher_) {
        publisher_ = std::make_unique<FramePublisher>();
        force_publish_ = true;
    }
    return *publisher_;
}

CursorState Terminal::cursor() const {
    CursorState c;
    c.col = x_;
    c.row = y_;
    c.shape = shape_;
    c.visible = dec(25);
    c.blink = dec(12);
    c.pending_wrap = pending_wrap_;
    return c;
}

int Terminal::dec_mode(uint16_t mode) const {
    if (mode == 1045) return rev_wrap_ext_ ? 1 : 2;
    // Known DEC modes that are never set: smooth scroll, print form feed and extent, national
    // characters, Hebrew/ hiragana cursor control.
    if (mode == 4 || mode == 18 || mode == 19 || mode == 42 || mode == 60) return 4;
    if (dec_slot(mode) < 0) return 0;
    if (mode == 1049 || mode == 1047 || mode == 47) return alt_active() ? 1 : 2;
    return dec(mode) ? 1 : 2;
}

int Terminal::ansi_mode(uint16_t mode) const {
    if (mode == 4) return insert_mode_ ? 1 : 2;
    if (mode == 20) return lnm_ ? 1 : 2;
    // ECMA-48 modes that this terminal knows but never sets: permanently reset. KAM, SRM and
    // the rest of the editing and transfer modes.
    switch (mode) {
        case 1: case 2: case 3: case 5: case 7: case 10: case 11: case 12: case 13: case 14:
        case 15: case 16: case 17: case 18: case 19:
            return 4;
        default: return 0;
    }
}

std::u8string Terminal::cursor_row_text(std::size_t max_bytes) const {
    std::u8string out;
    const auto r = active_->row(y_);
    const uint16_t end = pending_wrap_ ? static_cast<uint16_t>(x_ + 1u) : x_;
    for (uint16_t x = 0; x < end && x < cols(); ++x) {
        const Cell& c = r[x];
        if (c.width() == CellWidth::WideTail || c.width() == CellWidth::Empty) continue;
        std::size_t before = out.size();
        if (c.cluster()) {
            for (char32_t cp : graphemes_[c.cp()]) append_utf8(out, cp);
        } else {
            append_utf8(out, c.cp() == 0 ? U' ' : c.cp());
        }
        if (out.size() > max_bytes) {
            out.resize(before);
            break;
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------ feed

void Terminal::feed(std::span<const std::byte> bytes) {
    stream_offset_ += bytes.size();
    parser_.feed(bytes, *this);
    if (publisher_) publish();
}

bool Terminal::publish() {
    if (!publisher_) return false;
    if (sync_ && std::chrono::steady_clock::now() - sync_since_ < kSyncTimeout) return false;
    const CursorState cur = cursor();
    const bool changed = force_publish_ || publisher_->pending() || active_->any_dirty() || cur != last_cursor_ ||
                         modes_ != last_modes_ || styles_.version() != last_styles_version_;
    if (!changed) return false;
    FrameInput in;
    in.screen = active_;
    in.size = size_;
    in.cursor = cur;
    in.modes = modes_;
    in.styles = styles_.snapshot();
    in.links = links_.snapshot();
    in.graphemes = graphemes_.snapshot();
    in.first_abs_line = scrollback_.next_abs_line();
    in.scrollback_lines = alt_active() ? 0 : scrollback_.size();
    const bool ok = publisher_->publish(in);
    active_->clear_dirty();  // the publisher keeps the damage until it is published
    last_cursor_ = cur;
    last_modes_ = modes_;
    last_styles_version_ = styles_.version();
    force_publish_ = false;
    return ok;
}

std::optional<std::chrono::steady_clock::time_point> Terminal::publish_due(std::chrono::steady_clock::time_point now,
                                                                           std::chrono::steady_clock::duration retry) const {
    if (!publisher_) return std::nullopt;
    if (sync_ && now - sync_since_ < kSyncTimeout) return sync_since_ + kSyncTimeout;
    if (publisher_->pending()) return now + retry;
    return std::nullopt;
}

// -------------------------------------------------------------------------------- printing

char32_t Terminal::map_charset(char32_t cp) {
    const uint8_t gi = single_shift_ != 0 ? single_shift_ : gl_;
    single_shift_ = 0;
    switch (g_[gi]) {
        case Charset::Ascii:
            return cp;
        case Charset::Uk:
            return cp == U'#' ? char32_t{0x00A3} : cp;
        case Charset::DecGraphics:
            if (cp >= 0x5F && cp <= 0x7E) return kDecGraphics[cp - 0x5F];
            return cp;
    }
    return cp;
}

void Terminal::print_ascii(std::string_view run) {
    if (insert_mode_ || single_shift_ != 0 || g_[gl_] != Charset::Ascii) {
        for (char c : run) print(static_cast<unsigned char>(c));
        return;
    }
    const bool autowrap = dec(7);
    std::size_t i = 0;
    while (i < run.size()) {
        if (pending_wrap_) {
            if (autowrap) {
                wrap_line();
            } else {
                pending_wrap_ = false;
            }
        }
        const uint16_t right = line_right();
        const std::size_t avail = static_cast<std::size_t>(right - x_) + 1u;
        std::size_t n = std::min(avail, run.size() - i);
        std::string_view chunk = run.substr(i, n);
        if (!autowrap && run.size() - i > avail) {
            // Without autowrap the rest of the run piles up in the last column: only the last
            // character is visible there.
            chunk = run.substr(i, n - 1u);
        }
        const auto x0 = x_;
        const auto x1 = static_cast<uint16_t>(x0 + chunk.size());
        Screen& s = *active_;
        if (x1 > x0) {
            s.split_pairs(y_, x0, x1);
            auto r = s.row(y_);
            Cell c = Cell::make(0, CellWidth::Narrow, pen_.index, link_);
            c.set_prot(pen_.prot);
            for (std::size_t k = 0; k < chunk.size(); ++k) {
                c.bits = (c.bits & ~Cell::kCpMask) | static_cast<unsigned char>(chunk[k]);
                r[x0 + k] = c;
            }
            if (link_ != 0) s.set_flag(y_, kRowHasLink, true);
            s.mark_dirty(y_);
        }
        if (!autowrap && run.size() - i > avail) {
            // Write the last character of the run into the last column.
            x_ = right;
            const auto last = static_cast<unsigned char>(run.back());
            s.split_pairs(y_, right, static_cast<uint16_t>(right + 1u));
            Cell c = Cell::make(last, CellWidth::Narrow, pen_.index, link_);
            c.set_prot(pen_.prot);
            s.at(right, y_) = c;
            s.mark_dirty(y_);
            last_cp_ = last;
            return;
        }
        last_cp_ = static_cast<unsigned char>(chunk.back());
        i += n;
        if (x0 + n > right) {
            x_ = right;
            pending_wrap_ = autowrap;
        } else {
            x_ = static_cast<uint16_t>(x0 + n);
        }
    }
}

void Terminal::print(char32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return;  // controls are never printed
    if (cp < 0x7F) cp = map_charset(cp);
    const int w = width::cell_width(cp, opts_.ambiguous_is_wide);
    if (w == 0) {
        attach_combining(cp);
        return;
    }
    put_char(cp, w);
}

void Terminal::wrap_line() {
    active_->set_flag(y_, kRowWrapped, true);
    pending_wrap_ = false;
    x_ = left_ <= right_ && x_ >= left_ ? left_ : 0;
    index();
}

void Terminal::put_char(char32_t cp, int w) {
    const bool autowrap = dec(7);
    if (pending_wrap_) {
        if (autowrap) {
            wrap_line();
        } else {
            pending_wrap_ = false;
        }
    }
    Screen& s = *active_;
    uint16_t right = line_right();
    if (w == 2 && x_ >= right) {
        // No room for both halves.
        if (!autowrap) return;  // xterm: not printed
        if (line_left() >= right) return;  // a one-column line can never hold it
        s.split_pairs(y_, x_, static_cast<uint16_t>(x_ + 1u));
        s.at(x_, y_) = Cell::make(0, CellWidth::Empty, pen_.bce_index);
        s.mark_dirty(y_);
        wrap_line();
        right = line_right();
    }
    if (insert_mode_) s.insert_cells(y_, x_, right, static_cast<uint16_t>(w), blank());

    const auto x0 = x_;
    const auto x1 = static_cast<uint16_t>(x0 + w);
    s.split_pairs(y_, x0, x1);
    Cell c = Cell::make(cp, w == 2 ? CellWidth::WideLead : CellWidth::Narrow, pen_.index, link_);
    c.set_prot(pen_.prot);
    s.at(x0, y_) = c;
    if (w == 2) {
        Cell t = Cell::make(0, CellWidth::WideTail, pen_.index, link_);
        t.set_prot(pen_.prot);
        s.at(static_cast<uint16_t>(x0 + 1u), y_) = t;
    }
    if (link_ != 0) s.set_flag(y_, kRowHasLink, true);
    s.mark_dirty(y_);
    last_cp_ = cp;
    if (x0 + w > right) {
        x_ = right;
        pending_wrap_ = autowrap;
    } else {
        x_ = x1;
    }
}

void Terminal::attach_combining(char32_t cp) {
    // Mode 2027 off: a zero-width code point joins the previous cell.
    uint16_t x;
    if (pending_wrap_) {
        x = x_;
    } else if (x_ > 0) {
        x = static_cast<uint16_t>(x_ - 1u);
    } else {
        return;  // nothing to attach to
    }
    Screen& s = *active_;
    if (s.at(x, y_).width() == CellWidth::WideTail && x > 0) --x;
    Cell& c = s.at(x, y_);
    if (c.width() == CellWidth::Empty) return;
    std::u32string cps;
    if (c.cluster()) {
        cps = graphemes_[c.cp()];
    } else {
        cps.push_back(c.cp() == 0 ? U' ' : c.cp());
    }
    if (cps.size() >= GraphemePool::kMaxCodePoints) return;
    cps.push_back(cp);
    if (graphemes_.needs_collect() && collect_due()) collect_tables();
    Cell& target = s.at(x, y_);  // collect_tables() rewrote cells, re-read
    const uint32_t idx = graphemes_.intern(cps);
    if (idx == 0) return;  // pool full: the cell keeps its base code point
    target.set_cluster(idx);
    s.set_flag(y_, kRowHasGrapheme, true);
    s.mark_dirty(y_);
}

void Terminal::repeat_last(uint16_t n) {
    if (last_cp_ == 0) return;
    const char32_t cp = last_cp_;
    const int w = width::cell_width(cp, opts_.ambiguous_is_wide);
    if (w == 0) return;
    const std::size_t limit = std::size_t{cols()} * rows();
    for (std::size_t i = 0; i < n && i < limit; ++i) put_char(cp, w);
}

// ------------------------------------------------------------------------- C0 controls

void Terminal::execute(uint8_t c) {
    switch (c) {
        case 0x05:  // ENQ
            if (!opts_.answerback.empty()) {
                reply(std::string_view(reinterpret_cast<const char*>(opts_.answerback.data()),
                                       opts_.answerback.size()));
            }
            break;
        case 0x07:
            if (sink_) sink_->bell();
            break;
        case 0x08:
            backspace();
            break;
        case 0x09:
            tab_forward(1);
            break;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            index();
            if (lnm_) carriage_return();
            break;
        case 0x0D:
            carriage_return();
            break;
        case 0x0E:
            gl_ = 1;
            break;
        case 0x0F:
            gl_ = 0;
            break;
        case 0x1A:  // SUB
            put_char(0xFFFD, 1);
            break;
        default:
            break;  // NUL, CAN and the rest: ignored
    }
}

// ------------------------------------------------------------------------- cursor motion

bool Terminal::lr_margins() const { return dec(69); }

void Terminal::move_to(int x, int y) {
    x_ = clamp16(x, 0, cols() - 1);
    y_ = clamp16(y, 0, rows() - 1);
    pending_wrap_ = false;
}

void Terminal::cup(int row, int col) {
    if (dec(6)) {
        row += top_;
        col += left_;
        y_ = clamp16(row, top_, bottom_);
        x_ = clamp16(col, left_, right_);
        pending_wrap_ = false;
        return;
    }
    move_to(col, row);
}

void Terminal::cursor_up(int n) {
    const int lo = y_ >= top_ ? top_ : 0;
    y_ = clamp16(y_ - n, lo, rows() - 1);
    pending_wrap_ = false;
}

void Terminal::cursor_down(int n) {
    const int hi = y_ <= bottom_ ? bottom_ : rows() - 1;
    y_ = clamp16(y_ + n, 0, hi);
    pending_wrap_ = false;
}

void Terminal::cursor_left(int n) {
    if (dec(7) && (dec(45) || rev_wrap_ext_)) {
        reverse_wrap_left(n);
        return;
    }
    const int lo = x_ >= left_ ? left_ : 0;
    x_ = clamp16(x_ - n, lo, cols() - 1);
    pending_wrap_ = false;
}

// BS and CUB with reverse wraparound (needs DECAWM), as xterm since patch 383. A pending wrap
// takes the first step. At the left margin the cursor goes on at the right margin of the row
// above: with mode 45 only when that row soft-wrapped into this one (editing a long line), with
// mode 1045 always, and from the top margin to the bottom margin.
void Terminal::reverse_wrap_left(int n) {
    if (pending_wrap_) {
        pending_wrap_ = false;
        --n;
    }
    const bool ext = rev_wrap_ext_;
    const bool in_rows = y_ >= top_ && y_ <= bottom_;
    const uint16_t top = in_rows ? top_ : uint16_t{0};
    const uint16_t bottom = in_rows ? bottom_ : static_cast<uint16_t>(rows() - 1u);
    // Bounded: n is at most 65535, and every pass moves at least one step or stops.
    while (n > 0) {
        const uint16_t left = line_left();
        if (x_ > left) {
            const int step = std::min(n, x_ - left);
            x_ = static_cast<uint16_t>(x_ - step);
            n -= step;
            continue;
        }
        if (y_ > top) {
            if (!ext && (active_->flags(static_cast<uint16_t>(y_ - 1u)) & kRowWrapped) == 0) break;
            --y_;
        } else if (ext && top != bottom) {
            y_ = bottom;
        } else {
            break;
        }
        x_ = line_right();
        --n;
    }
}

void Terminal::cursor_right(int n) {
    const int hi = x_ <= right_ ? right_ : cols() - 1;
    x_ = clamp16(x_ + n, 0, hi);
    pending_wrap_ = false;
}

void Terminal::carriage_return() {
    x_ = line_left();
    pending_wrap_ = false;
}

void Terminal::backspace() {
    if (dec(7) && (dec(45) || rev_wrap_ext_)) {
        reverse_wrap_left(1);
        return;
    }
    pending_wrap_ = false;
    if (x_ > line_left()) --x_;
}

// DECBI (ESC 6) and DECFI (ESC 9): the cursor moves one column; at the left (right) margin the
// region's columns shift right (left) instead. Outside the region only the move happens.
void Terminal::back_index() {
    pending_wrap_ = false;
    if (x_ == left_ && y_ >= top_ && y_ <= bottom_) {
        scroll_columns(1, false);
    } else if (x_ > 0) {
        --x_;
    }
}

void Terminal::forward_index() {
    pending_wrap_ = false;
    if (x_ == right_ && y_ >= top_ && y_ <= bottom_) {
        scroll_columns(1, true);
    } else if (x_ + 1u < cols()) {
        ++x_;
    }
}

void Terminal::index() {
    if (y_ == bottom_) {
        if (x_ >= left_ && x_ <= right_) scroll_up(1);
    } else if (y_ + 1u < rows()) {
        ++y_;
    }
}

void Terminal::reverse_index() {
    if (y_ == top_) {
        if (x_ >= left_ && x_ <= right_) scroll_down(1);
    } else if (y_ > 0) {
        --y_;
    }
    pending_wrap_ = false;
}

void Terminal::tab_forward(int n) {
    const uint16_t right = line_right();
    pending_wrap_ = false;
    while (n-- > 0 && x_ < right) {
        do {
            ++x_;
        } while (x_ < right && tabs_[x_] == 0);
    }
}

void Terminal::tab_backward(int n) {
    // CBT is not bounded by the left margin (xterm; esctest CBT_IgnoresRegion).
    const uint16_t left = 0;
    pending_wrap_ = false;
    while (n-- > 0 && x_ > left) {
        do {
            --x_;
        } while (x_ > left && tabs_[x_] == 0);
    }
}

void Terminal::save_cursor() {
    SavedCursor& s = saved_[alt_active() ? 1 : 0];
    s.valid = true;
    s.x = x_;
    s.y = y_;
    s.pending_wrap = pending_wrap_;
    s.style = pen_.style;
    s.prot = pen_.prot;
    s.origin = dec(6);
    s.g = g_;
    s.gl = gl_;
}

void Terminal::restore_cursor() {
    const SavedCursor& s = saved_[alt_active() ? 1 : 0];
    if (!s.valid) {
        // xterm: DECRC without DECSC homes the cursor and resets the attributes.
        move_to(0, 0);
        pen_ = Pen{};
        update_pen();
        dec_.reset(static_cast<std::size_t>(dec_slot(6)));
        g_ = {};
        gl_ = 0;
        return;
    }
    x_ = std::min<uint16_t>(s.x, static_cast<uint16_t>(cols() - 1u));
    y_ = std::min<uint16_t>(s.y, static_cast<uint16_t>(rows() - 1u));
    pending_wrap_ = s.pending_wrap && x_ == s.x;
    pen_.style = s.style;
    pen_.prot = s.prot;
    update_pen();
    dec_.set(static_cast<std::size_t>(dec_slot(6)), s.origin);
    g_ = s.g;
    gl_ = s.gl;
}

void Terminal::reset_margins() {
    top_ = 0;
    bottom_ = static_cast<uint16_t>(rows() - 1u);
    left_ = 0;
    right_ = static_cast<uint16_t>(cols() - 1u);
}

void Terminal::reset_tabs() {
    tabs_.assign(cols(), 0);
    for (std::size_t x = 8; x < tabs_.size(); x += 8) tabs_[x] = 1;
}

// ---------------------------------------------------------------------- scrolling, editing

void Terminal::scroll_up(uint16_t n) {
    Screen& s = *active_;
    const bool full_screen = top_ == 0 && bottom_ == rows() - 1u && left_ == 0 && right_ == cols() - 1u;
    if (full_screen && !alt_active()) {
        // Rows that leave a full-screen region of the primary screen go to the scrollback (xterm).
        const uint16_t k = std::min(n, rows());
        for (uint16_t y = 0; y < k; ++y) scrollback_.push(s.row(y), s.flags(y));
    }
    s.scroll_up(top_, bottom_, left_, right_, n, blank());
}

void Terminal::scroll_down(uint16_t n) { active_->scroll_down(top_, bottom_, left_, right_, n, blank()); }

void Terminal::erase_display(int mode, bool selective) {
    Screen& s = *active_;
    pending_wrap_ = false;
    switch (mode) {
        case 0:
            s.erase(y_, x_, cols(), blank(), selective);
            s.set_flag(y_, kRowWrapped, false);
            for (uint16_t y = static_cast<uint16_t>(y_ + 1u); y < rows(); ++y) {
                s.erase(y, 0, cols(), blank(), selective);
                s.set_flags(y, 0);
            }
            break;
        case 1:
            for (uint16_t y = 0; y < y_; ++y) {
                s.erase(y, 0, cols(), blank(), selective);
                s.set_flags(y, 0);
            }
            s.erase(y_, 0, static_cast<uint16_t>(x_ + 1u), blank(), selective);
            break;
        case 2:
            for (uint16_t y = 0; y < rows(); ++y) {
                s.erase(y, 0, cols(), blank(), selective);
                s.set_flags(y, 0);
            }
            break;
        case 3:
            if (!selective) clear_scrollback();
            break;
        default:
            break;
    }
}

void Terminal::erase_line(int mode, bool selective) {
    Screen& s = *active_;
    pending_wrap_ = false;
    switch (mode) {
        case 0:
            s.erase(y_, x_, cols(), blank(), selective);
            s.set_flag(y_, kRowWrapped, false);
            break;
        case 1:
            s.erase(y_, 0, static_cast<uint16_t>(x_ + 1u), blank(), selective);
            break;
        case 2:
            s.erase(y_, 0, cols(), blank(), selective);
            s.set_flag(y_, kRowWrapped, false);
            break;
        default:
            break;
    }
}

void Terminal::insert_lines(uint16_t n) {
    if (!in_region()) return;
    active_->scroll_down(y_, bottom_, left_, right_, n, blank());
    x_ = left_;
    pending_wrap_ = false;
}

void Terminal::delete_lines(uint16_t n) {
    if (!in_region()) return;
    active_->scroll_up(y_, bottom_, left_, right_, n, blank());
    x_ = left_;
    pending_wrap_ = false;
}

void Terminal::insert_chars(uint16_t n) {
    if (x_ < left_ || x_ > right_) return;
    active_->insert_cells(y_, x_, right_, n, blank());
    pending_wrap_ = false;
}

void Terminal::delete_chars(uint16_t n) {
    if (x_ < left_ || x_ > right_) return;
    active_->delete_cells(y_, x_, right_, n, blank());
    pending_wrap_ = false;
}

// DECIC, DECDC: columns at the cursor, inside the scroll region (VT420). No effect outside it.
void Terminal::insert_columns(uint16_t n) {
    if (!in_region()) return;
    for (uint16_t y = top_; y <= bottom_; ++y) active_->insert_cells(y, x_, right_, n, blank());
    pending_wrap_ = false;
}

void Terminal::delete_columns(uint16_t n) {
    if (!in_region()) return;
    for (uint16_t y = top_; y <= bottom_; ++y) active_->delete_cells(y, x_, right_, n, blank());
    pending_wrap_ = false;
}

// SL (CSI Pn SP @) and SR (CSI Pn SP A): the scroll region moves left or right by n columns.
void Terminal::scroll_columns(uint16_t n, bool left) {
    if (!in_region()) return;
    for (uint16_t y = top_; y <= bottom_; ++y) {
        if (left) {
            active_->delete_cells(y, left_, right_, n, blank());
        } else {
            active_->insert_cells(y, left_, right_, n, blank());
        }
    }
    pending_wrap_ = false;
}

void Terminal::erase_chars(uint16_t n) {
    const int x1 = std::min<int>(x_ + n, cols());
    active_->erase(y_, x_, static_cast<uint16_t>(x1), blank());
    pending_wrap_ = false;
}

void Terminal::decaln() {
    reset_margins();
    Screen& s = *active_;
    for (uint16_t y = 0; y < rows(); ++y) {
        s.fill(y, 0, cols(), Cell::make(U'E', CellWidth::Narrow, 0));
        s.set_flags(y, 0);
    }
    move_to(0, 0);
}

// ------------------------------------------------------------------------------ screens

void Terminal::switch_screen(bool alt, bool clear_alt) {
    if (alt == alt_active()) {
        if (alt && clear_alt) alt_->clear(blank());
        return;
    }
    if (alt) {
        if (!alt_ || alt_->cols() != cols() || alt_->rows() != rows()) {
            alt_ = std::make_unique<Screen>(cols(), rows());
        }
        if (clear_alt) alt_->clear(blank());
        active_ = alt_.get();
    } else {
        active_ = &primary_;
    }
    active_->mark_all_dirty();
    modes_.alt_screen = alt;
    notify_modes();
}

void Terminal::resize(GridSize size) {
    if (size.cols == 0 || size.rows == 0) return;
    const GridSize old = size_;
    size_ = size;
    if (old.cols == size.cols && old.rows == size.rows) {
        force_publish_ = true;
        return;
    }
    const bool alt = alt_active();
    if (size.cols != old.cols) {
        reflow_primary(old, size);
    } else {
        resize_height(old, size);
    }
    if (alt) {
        alt_->resize(size.cols, size.rows);  // clipped or padded; the application repaints
        x_ = std::min<uint16_t>(x_, static_cast<uint16_t>(size.cols - 1u));
        y_ = std::min<uint16_t>(y_, static_cast<uint16_t>(size.rows - 1u));
        pending_wrap_ = false;
    } else if (alt_) {
        alt_.reset();  // an inactive alternate screen is reallocated on next use
    }
    for (SavedCursor& s : saved_) {
        s.x = std::min<uint16_t>(s.x, static_cast<uint16_t>(size.cols - 1u));
        s.y = std::min<uint16_t>(s.y, static_cast<uint16_t>(size.rows - 1u));
    }
    reset_margins();
    const std::size_t old_cols = tabs_.size();
    tabs_.resize(size.cols, 0);
    for (std::size_t x = old_cols; x < tabs_.size(); ++x) tabs_[x] = (x % 8 == 0) ? 1 : 0;
    active_->mark_all_dirty();
    force_publish_ = true;
    if (publisher_) publish();
}

// Height-only change: no reflow. Keep the cursor row visible when shrinking (rows above it go
// to the scrollback); pull rows back from the scrollback when growing.
void Terminal::resize_height(GridSize old, GridSize size) {
    const bool alt = alt_active();
    SavedCursor& psaved = saved_[0];
    uint16_t pcy = alt ? (psaved.valid ? psaved.y : 0) : y_;
    if (size.rows < old.rows) {
        const int excess = pcy + 1 - size.rows;
        if (excess > 0) {
            const auto k = static_cast<uint16_t>(excess);
            for (uint16_t y = 0; y < k; ++y) scrollback_.push(primary_.row(y), primary_.flags(y));
            primary_.drop_top(k);
            pcy = static_cast<uint16_t>(pcy - k);
        }
        primary_.resize(size.cols, size.rows);
    } else {
        primary_.resize(size.cols, size.rows);
        uint16_t pulled = 0;
        const auto room = static_cast<uint16_t>(size.rows - old.rows);
        Scrollback::Line line;
        while (pulled < room && scrollback_.pop_newest(line)) {
            primary_.push_top(1);
            auto r = primary_.row(0);
            const std::size_t n = std::min(line.cells.size(), r.size());
            std::copy(line.cells.begin(), line.cells.begin() + static_cast<std::ptrdiff_t>(n), r.begin());
            primary_.set_flags(0, line.flags);
            ++pulled;
        }
        pcy = static_cast<uint16_t>(pcy + pulled);
    }
    pcy = std::min<uint16_t>(pcy, static_cast<uint16_t>(size.rows - 1u));
    if (alt) {
        if (psaved.valid) psaved.y = pcy;
    } else {
        y_ = pcy;
        x_ = std::min<uint16_t>(x_, static_cast<uint16_t>(size.cols - 1u));
    }
}

namespace {

// A position followed through a reflow: in screen coordinates before, in rows of the new
// layout after.
struct Tracked {
    bool on = false;
    uint16_t x = 0, y = 0;
    bool pending = false;
    std::size_t index = 0;  // cell index inside its logical line
    std::size_t row = 0;    // after: row in the new layout
    uint16_t col = 0;
    bool placed = false;
    bool wrap_at_end = false;  // after: placed on the last column with a pending wrap
};

uint16_t cell_row_flags(std::span<const Cell> cells) {
    uint16_t f = 0;
    for (const Cell& c : cells) {
        if (c.link != 0) f |= kRowHasLink;
        if (c.cluster()) f |= kRowHasGrapheme;
    }
    return f;
}

// Lays out one logical line at `width` columns and appends its rows to `out`:
// a wide character that would straddle the edge moves to the next row behind an Empty pad;
// trailing blanks never make rows of their own, they fill the last row as far as it reaches.
void layout_line(std::span<const Cell> line, uint16_t line_flags, uint16_t width, std::span<Tracked> tracked,
                 std::vector<Scrollback::Line>& out) {
    std::size_t content = line.size();
    while (content > 0 && line[content - 1].is_blank()) --content;
    for (const Tracked& t : tracked) {
        if (t.on && !t.placed && t.index >= content && t.index < line.size() + width) content = t.index + 1;
    }
    content = std::min(content, line.size());
    const std::size_t first = out.size();
    out.emplace_back();
    out.back().cells.reserve(width);
    const auto new_row = [&] {
        out.back().flags |= kRowWrapped;
        out.emplace_back();
        out.back().cells.reserve(width);
    };
    const auto mark = [&](std::size_t i) {
        for (Tracked& t : tracked) {
            if (t.on && !t.placed && t.index == i) {
                t.row = out.size() - 1;
                t.col = static_cast<uint16_t>(out.back().cells.size());
                t.placed = true;
            }
        }
    };
    for (std::size_t i = 0; i < content; ++i) {
        Cell c = line[i];
        std::vector<Cell>* row = &out.back().cells;
        if (c.width() == CellWidth::WideLead) {
            const bool has_tail = i + 1 < line.size() && line[i + 1].width() == CellWidth::WideTail;
            if (width < 2 || !has_tail) {
                // A one-column line can never hold it (and a lead without a tail is no pair).
                if (row->size() == width) {
                    new_row();
                    row = &out.back().cells;
                }
                mark(i);
                row->push_back(Cell::blank(c.style));
                if (has_tail) {
                    mark(i + 1);
                    ++i;
                }
                continue;
            }
            if (row->size() + 2u > width) {
                if (row->size() < width) row->push_back(Cell::make(0, CellWidth::Empty, 0));
                new_row();
                row = &out.back().cells;
            }
            mark(i);
            row->push_back(c);
            mark(i + 1);
            row->push_back(line[i + 1]);
            ++i;
            continue;
        }
        if (c.width() == CellWidth::WideTail) c = Cell::blank(c.style);  // orphan: no pair
        if (row->size() == width) {
            new_row();
            row = &out.back().cells;
        }
        mark(i);
        row->push_back(c);
    }
    // Trailing blanks, as far as the last row reaches.
    std::vector<Cell>& last = out.back().cells;
    for (std::size_t i = content; i < line.size() && last.size() < width; ++i) {
        mark(i);
        last.push_back(line[i].width() == CellWidth::Narrow ? line[i] : Cell::blank(line[i].style));
    }
    // A blank last row that exists only because a tracked point (the cursor after the last
    // character) sits at its start: the point goes back to the end of the full row above it,
    // with a pending wrap, as if the text had just been written at this width.
    if (out.size() - first > 1u) {
        const std::size_t last_row = out.size() - 1u;
        const auto& lc = out[last_row].cells;
        bool movable = std::all_of(lc.begin(), lc.end(), [](const Cell& x) { return x.is_blank(); });
        bool any = false;
        for (const Tracked& t : tracked) {
            if (t.on && t.placed && t.row == last_row) {
                any = true;
                movable = movable && t.col == 0;
            }
        }
        if (movable && any) {
            out.pop_back();
            out.back().flags &= static_cast<uint16_t>(~static_cast<uint32_t>(kRowWrapped));
            for (Tracked& t : tracked) {
                if (t.on && t.placed && t.row == last_row) {
                    t.row = last_row - 1u;
                    t.col = static_cast<uint16_t>(width - 1u);
                    t.pending = true;
                    t.wrap_at_end = true;
                }
            }
        }
    }
    for (std::size_t r = first; r < out.size(); ++r) out[r].flags |= cell_row_flags(out[r].cells);
    out[first].flags |= line_flags;
}

}  // namespace

// Width change of the primary screen: joins the scrollback and the screen into
// logical lines through the Wrapped flags, re-wraps them at the new width, and splits the result
// between the scrollback and the screen so that the cursor keeps its row where it can. The
// cursor and the saved cursor (DECSC, and the one 1049 restores) follow their cells.
void Terminal::reflow_primary(GridSize old, GridSize size) {
    const bool alt = alt_active();
    const uint16_t width = size.cols;
    // [0] the primary cursor (the saved one while the alternate screen is active), [1] the
    // primary saved cursor while the primary screen is active, [2] the top left cell.
    std::array<Tracked, 3> tr{};
    tr[2] = Tracked{true, 0, 0, false};
    const SavedCursor& ps = saved_[0];
    if (!alt) {
        tr[0] = Tracked{true, x_, y_, pending_wrap_};
        if (ps.valid) tr[1] = Tracked{true, ps.x, ps.y, ps.pending_wrap};
    } else if (ps.valid) {
        tr[0] = Tracked{true, ps.x, ps.y, ps.pending_wrap};
    } else {
        tr[0] = Tracked{true, 0, 0, false};
    }
    for (Tracked& t : tr) {
        t.x = std::min<uint16_t>(t.x, static_cast<uint16_t>(old.cols - 1u));
        t.y = std::min<uint16_t>(t.y, static_cast<uint16_t>(old.rows - 1u));
    }

    const std::size_t sbn = scrollback_.size();
    const std::size_t total = sbn + old.rows;
    std::vector<Scrollback::Line> out;
    out.reserve(total + total / 4u);
    std::vector<Cell> line;
    uint16_t line_flags = 0;
    std::optional<std::size_t> content_end;  // last row of the new layout with text from the screen
    constexpr auto kPerRowFlags = static_cast<uint16_t>(kRowWrapped | kRowHasLink | kRowHasGrapheme);
    for (std::size_t k = 0; k < total; ++k) {
        std::span<const Cell> cells;
        uint16_t f = 0;
        bool on_screen = false;
        uint16_t sy = 0;
        if (k < sbn) {
            const Scrollback::Line& l = scrollback_.line(k);
            cells = l.cells;
            f = l.flags;
        } else {
            sy = static_cast<uint16_t>(k - sbn);
            cells = primary_.row(sy);
            f = primary_.flags(sy);
            on_screen = true;
        }
        const bool wrapped = (f & kRowWrapped) != 0 && k + 1 < total;
        std::size_t n = cells.size();
        if (wrapped) {
            while (n > 0 && cells[n - 1].width() == CellWidth::Empty) --n;  // padding of a wrapped wide char
        }
        for (Tracked& t : tr) {
            if (t.on && on_screen && t.y == sy) {
                n = std::max<std::size_t>(n, std::size_t{t.x} + 1u);
                t.index = line.size() + t.x;
            }
        }
        line.insert(line.end(), cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(n));
        line_flags = static_cast<uint16_t>(line_flags | (f & ~kPerRowFlags));
        if (wrapped) continue;
        // Tracked points of this line only: their index is valid for it.
        std::array<Tracked, 3> here{};
        std::array<Tracked*, 3> owner{};
        std::size_t nh = 0;
        for (Tracked& t : tr) {
            if (t.on && !t.placed && on_screen && t.y <= sy && t.index < line.size()) {
                here[nh] = t;
                owner[nh++] = &t;
            }
        }
        const std::size_t before = out.size();
        layout_line(line, line_flags, width, std::span(here.data(), nh), out);
        if (on_screen) {
            for (std::size_t r = out.size(); r > before; --r) {
                const auto& cs = out[r - 1].cells;
                if (std::any_of(cs.begin(), cs.end(), [](const Cell& x) { return !x.is_blank(); })) {
                    content_end = r - 1;
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < nh; ++i) *owner[i] = here[i];
        line.clear();
        line_flags = 0;
    }

    // Pending wrap: the cursor sits on the last character it wrote. After the reflow it moves to
    // the column after it, unless that column is past the edge again.
    for (Tracked& t : tr) {
        if (!t.on || !t.pending || t.wrap_at_end) continue;
        if (t.col + 1u < width) {
            ++t.col;
            t.pending = false;
        }
    }

    // Split between the scrollback and the screen. A screen whose text or cursor reached its
    // bottom row stays anchored at the bottom: rows above the cursor go to the scrollback, or come
    // back from it, so the prompt stays at the bottom. A screen with room below keeps its top
    // row in place. The cursor is always on the screen.
    const auto c = static_cast<long long>(tr[0].row);
    const long long end = std::max(c, content_end ? static_cast<long long>(*content_end) : 0LL);
    const long long rows = size.rows;
    bool full = tr[0].y + 1u >= old.rows;
    for (uint16_t x = 0; !full && x < old.cols; ++x) full = !primary_.row(static_cast<uint16_t>(old.rows - 1u))[x].is_blank();
    long long s = full ? end - (rows - 1) : std::max(static_cast<long long>(tr[2].row), end - (rows - 1));
    s = std::clamp(s, std::max(0LL, c - (rows - 1)), c);
    const auto split = static_cast<std::size_t>(s);

    const uint64_t first_abs = scrollback_.first_abs_line();
    scrollback_.clear();
    for (std::size_t i = 0; i < split; ++i) scrollback_.push(out[i].cells, out[i].flags);
    // Saturating, as Scrollback counts.
    scrollback_.set_next_abs_line(first_abs > UINT64_MAX - split ? UINT64_MAX : first_abs + split);

    primary_ = Screen(size.cols, size.rows);
    for (uint16_t y = 0; y < size.rows && split + y < out.size(); ++y) {
        const Scrollback::Line& l = out[split + y];
        auto r = primary_.row(y);
        std::copy(l.cells.begin(), l.cells.begin() + static_cast<std::ptrdiff_t>(std::min(l.cells.size(), r.size())), r.begin());
        primary_.set_flags(y, l.flags);
    }
    // The last row of the screen continues below it no more.
    if (split + size.rows < out.size()) primary_.set_flag(static_cast<uint16_t>(size.rows - 1u), kRowWrapped, false);
    primary_.mark_all_dirty();

    const auto place = [&](const Tracked& t, uint16_t& x, uint16_t& y, bool& pending) {
        const long long r = static_cast<long long>(t.row) - s;
        y = static_cast<uint16_t>(std::clamp(r, 0LL, static_cast<long long>(size.rows) - 1));
        x = std::min<uint16_t>(t.col, static_cast<uint16_t>(width - 1u));
        pending = r >= 0 && t.pending;
    };
    if (!alt) {
        place(tr[0], x_, y_, pending_wrap_);
        if (tr[1].on) place(tr[1], saved_[0].x, saved_[0].y, saved_[0].pending_wrap);
    } else if (ps.valid) {
        place(tr[0], saved_[0].x, saved_[0].y, saved_[0].pending_wrap);
    }
}

// -------------------------------------------------------------------------- tables

void Terminal::update_pen() {
    if (styles_.needs_collect() && collect_due()) collect_tables();
    pen_.index = styles_.intern(pen_.style);
    Style bce;
    bce.bg = pen_.style.bg;
    pen_.bce_index = styles_.intern(bce);
}

void Terminal::collect_tables() {
    std::vector<bool> used_styles(styles_.size(), false);
    std::vector<bool> used_links(links_.size(), false);
    std::vector<bool> used_graphemes(graphemes_.size(), false);
    const auto mark = [&](const Cell& c) {
        used_styles[c.style] = true;
        used_links[c.link] = true;
        if (c.cluster()) used_graphemes[c.cp()] = true;
    };
    const auto mark_screen = [&](const Screen& s) {
        for (uint16_t y = 0; y < s.rows(); ++y) {
            for (const Cell& c : s.row(y)) mark(c);
        }
    };
    mark_screen(primary_);
    if (alt_) mark_screen(*alt_);
    for (std::size_t i = 0; i < scrollback_.size(); ++i) {
        for (const Cell& c : scrollback_.line(i).cells) mark(c);
    }
    used_links[link_] = true;

    const auto style_map = styles_.compact(used_styles);
    const auto link_map = links_.compact(used_links);
    const auto grapheme_map = graphemes_.compact(used_graphemes);
    const auto rewrite = [&](Cell& c) {
        c.style = style_map[c.style];
        c.link = link_map[c.link];
        if (c.cluster()) c.set_cluster(grapheme_map[c.cp()]);
    };
    const auto rewrite_screen = [&](Screen& s) {
        for (uint16_t y = 0; y < s.rows(); ++y) {
            for (Cell& c : s.row(y)) rewrite(c);
        }
        s.mark_all_dirty();
    };
    rewrite_screen(primary_);
    if (alt_) rewrite_screen(*alt_);
    for (std::size_t i = 0; i < scrollback_.size(); ++i) {
        for (Cell& c : scrollback_.line(i).cells) rewrite(c);
    }
    link_ = link_map[link_];
    // The pen keeps its Style by value; re-intern it (no recursion: the table now has room).
    pen_.index = styles_.intern(pen_.style);
    Style bce;
    bce.bg = pen_.style.bg;
    pen_.bce_index = styles_.intern(bce);
    force_publish_ = true;
    // A pass scans every cell and rebuilds the tables' indices. One that left a table near its
    // threshold (the live cells use that many entries) would run again after a few new entries.
    // The next pass waits for a quarter of that work in fed bytes (all of it when a table is
    // still at its threshold), so the passes cost O(1) per byte; meanwhile a full table hands
    // out its fallbacks (the nearest style, no link).
    collect_offset_ = stream_offset_;
    ++collections_;
    const std::size_t cells = scrollback_.cells() + std::size_t{primary_.cols()} * primary_.rows() * (alt_ ? 2u : 1u);
    const std::size_t entries = styles_.size() + links_.size() + graphemes_.size();
    const bool still = styles_.needs_collect() || links_.needs_collect() || graphemes_.needs_collect();
    collect_wait_ = (still ? cells : cells / 4) + entries;
}

bool Terminal::collect_due() const {
    return stream_offset_ < collect_offset_ || stream_offset_ - collect_offset_ >= collect_wait_;
}

// --------------------------------------------------------------------------- replies

void Terminal::reply(std::string_view s) {
    if (sink_) sink_->reply(std::as_bytes(std::span(s.data(), s.size())));
}

void Terminal::notify_modes() {
    if (sink_) sink_->modes_changed(modes_);
}

}  // namespace kt::vt
