#include "pty/agent/scraper.h"

#include <algorithm>

namespace kt::pty::agent {
namespace {

// At most this many rows are caught up after the window moved down; more means a resync.
constexpr int32_t kMaxCatchUp = 4000;
// ReadConsoleOutputW fails on old consoles above a 64 KiB heap limit.
constexpr int kMaxCellsPerRead = 8192;

constexpr uint16_t kAttrMask = 0x00FF | kAttrReverse | kAttrUnderscore;

constexpr uint8_t console_to_ansi(unsigned c) {
    return static_cast<uint8_t>(((c & 1u) << 2) | (c & 2u) | ((c & 4u) >> 2) | (c & 8u));
}

bool is_high(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool is_low(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// East Asian Wide/Fullwidth blocks of the BMP. The console stores such a character in two
// cells; on consoles that do not set LEADING/TRAILING the code unit appears twice.
bool wide_guess(char16_t c) {
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6);
}

bool is_blank(const ConCell& c) {
    return (c.ch == u' ' || c.ch == 0) && (c.attr & 0xF0) == 0 && !(c.attr & (kAttrReverse | kAttrUnderscore));
}

// Marks cells that continue the character to their left (second half of a wide character or
// the low half of a surrogate pair). Left to right, so "AAAA" of a doubled wide code unit
// pairs up as (A A)(A A).
void continuation_map(const ConCell* row, int w, std::vector<uint8_t>& cont) {
    cont.assign(static_cast<std::size_t>(w), 0);
    for (int x = 0; x < w; ++x) {
        const ConCell& c = row[x];
        if (c.attr & kAttrTrailing) {
            cont[static_cast<std::size_t>(x)] = 1;
            continue;
        }
        if (x + 1 >= w) break;
        const ConCell& n = row[x + 1];
        if (c.attr & kAttrLeading) continue;  // the next cell carries TRAILING
        if ((is_high(c.ch) && is_low(n.ch)) || (n.ch == c.ch && wide_guess(c.ch) && !(n.attr & kAttrLeading))) {
            cont[static_cast<std::size_t>(x + 1)] = 1;
            ++x;
        }
    }
}

void put_utf8(std::string& o, char32_t cp) {
    if (cp < 0x80) {
        o.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool read_rows(IConsole& con, int32_t top, int rows, int16_t left, int w, ConCell* out) {
    const int chunk = std::max(1, kMaxCellsPerRead / std::max(1, w));
    for (int r = 0; r < rows; r += chunk) {
        const int n = std::min(chunk, rows - r);
        if (!con.read(static_cast<int16_t>(top + r), static_cast<int16_t>(n), left, static_cast<int16_t>(w),
                      std::span(out + static_cast<std::ptrdiff_t>(r) * w, static_cast<std::size_t>(n * w)))) {
            return false;
        }
    }
    return true;
}

bool same_geometry(const ConInfo& a, const ConInfo& b) {
    return a.buf_w == b.buf_w && a.buf_h == b.buf_h && a.win_left == b.win_left && a.win_top == b.win_top &&
           a.win_w == b.win_w && a.win_h == b.win_h && a.cur_x == b.cur_x && a.cur_y == b.cur_y;
}

}  // namespace

std::string Scraper::start() {
    // win32-input-mode: the host's encoder may send keys as INPUT_RECORD fields.
    return "\x1b[?9001h";
}

void Scraper::cup(std::string& o, int y, int x) {
    if (host_y_ == y && host_x_ == x) return;
    o += "\x1b[";
    o += std::to_string(y + 1);
    o += ';';
    o += std::to_string(x + 1);
    o += 'H';
    host_y_ = y;
    host_x_ = x;
}

void Scraper::set_sgr(std::string& o, uint16_t attr) {
    attr &= kAttrMask;
    if (attr == sgr_) return;
    sgr_ = attr;
    o += "\x1b[0";
    const unsigned fg = attr & 0xFu, bg = (attr >> 4) & 0xFu;
    if (fg != 7) {
        const unsigned a = console_to_ansi(fg);
        o += ';';
        o += std::to_string(a < 8 ? 30 + a : 90 + a - 8);
    }
    if (bg != 0) {
        const unsigned a = console_to_ansi(bg);
        o += ';';
        o += std::to_string(a < 8 ? 40 + a : 100 + a - 8);
    }
    if (attr & kAttrReverse) o += ";7";
    if (attr & kAttrUnderscore) o += ";4";
    o += 'm';
}

void Scraper::reset_model(std::string& o, int w, int h, int32_t top) {
    set_sgr(o, kAttrDefault);
    o += "\x1b[H\x1b[2J";
    host_y_ = 0;
    host_x_ = 0;
    w_ = w;
    h_ = h;
    top_ = top;
    model_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), ConCell{});
    valid_ = true;
}

void Scraper::emit_text(std::string& o, const ConCell* row, int from, int to) {
    std::vector<uint8_t> cont;
    continuation_map(row, w_, cont);
    for (int x = from; x < to; ++x) {
        if (cont[static_cast<std::size_t>(x)]) continue;
        const ConCell& c = row[x];
        set_sgr(o, c.attr);
        char32_t cp = c.ch;
        int cells = 1;
        if (is_high(c.ch) && x + 1 < w_ && cont[static_cast<std::size_t>(x + 1)] && is_low(row[x + 1].ch)) {
            cp = 0x10000 + ((char32_t{c.ch} - 0xD800) << 10) + (char32_t{row[x + 1].ch} - 0xDC00);
            cells = 2;
        } else if (is_high(c.ch) || is_low(c.ch)) {
            cp = 0xFFFD;
        } else if (cp < 0x20 || cp == 0x7F) {
            cp = U' ';
        } else if (x + 1 < w_ && cont[static_cast<std::size_t>(x + 1)]) {
            cells = 2;
        }
        put_utf8(o, cp);
        host_x_ += cells;
    }
    if (host_x_ >= w_) host_x_ = -1;  // pending wrap: position unknown until the next CUP
}

void Scraper::emit_row(std::string& o, int y, const ConCell* now) {
    ConCell* old = &model_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_)];
    int a = -1, b = -1;
    for (int x = 0; x < w_; ++x) {
        if (!(old[x] == now[x])) {
            if (a < 0) a = x;
            b = x + 1;
        }
    }
    if (a < 0) return;
    std::vector<uint8_t> cn, co;
    continuation_map(now, w_, cn);
    continuation_map(old, w_, co);
    while (a > 0 && (cn[static_cast<std::size_t>(a)] || co[static_cast<std::size_t>(a)])) --a;
    while (b < w_ && (cn[static_cast<std::size_t>(b)] || co[static_cast<std::size_t>(b)])) ++b;
    int end = w_;
    while (end > 0 && is_blank(now[end - 1])) --end;
    while (end < w_ && cn[static_cast<std::size_t>(end)]) ++end;  // never split a wide character
    cup(o, y, a);
    if (b > end) {
        if (a < end) emit_text(o, now, a, end);
        if (host_x_ < 0) cup(o, y, end);  // after pending wrap (not reachable: end < w)
        set_sgr(o, kAttrDefault);
        o += "\x1b[K";
    } else {
        emit_text(o, now, a, b);
    }
    std::copy(now, now + w_, old);
}

bool Scraper::probe(IConsole& con) {
    if (!valid_ || need_full_) return true;
    ConInfo a;
    if (!con.info(a)) return false;
    if (!same_geometry(a, last_) || a.cur_visible != last_.cur_visible) return true;
    const int y0 = std::max<int>(a.win_top, a.cur_y - 1);
    const int y1 = std::min<int>(a.win_top + a.win_h, a.cur_y + 2);
    if (y1 <= y0 || a.win_w != w_) return true;
    scratch_.resize(static_cast<std::size_t>((y1 - y0) * w_));
    if (!read_rows(con, y0, y1 - y0, a.win_left, w_, scratch_.data())) return true;
    for (int y = y0; y < y1; ++y) {
        const int my = y - a.win_top;
        if (!std::equal(scratch_.begin() + (y - y0) * w_, scratch_.begin() + (y - y0 + 1) * w_,
                        model_.begin() + my * w_)) {
            return true;
        }
    }
    return false;
}

std::string Scraper::scrape(IConsole& con) {
    ++scrapes_;
    ConInfo a, b;
    int32_t from = 0;
    int w = 0, h = 0;
    bool consistent = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!con.info(a) || a.win_w <= 0 || a.win_h <= 0) return {};
        w = a.win_w;
        h = a.win_h;
        const bool alt = a.buf_h <= a.win_h;
        from = a.win_top;
        if (valid_ && !alt && !alt_ && w == w_ && h == h_ && a.win_top > top_ && a.win_top - top_ <= kMaxCatchUp) {
            from = top_;
        }
        const int rows = a.win_top + h - from;
        scratch_.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(w));
        if (!read_rows(con, from, rows, a.win_left, w, scratch_.data())) {
            // The rows that left the window may be gone already; read the window alone.
            if (from == a.win_top) return {};
            from = a.win_top;
            scratch_.resize(static_cast<std::size_t>(h) * static_cast<std::size_t>(w));
            if (!read_rows(con, from, h, a.win_left, w, scratch_.data())) return {};
        }
        if (!con.info(b)) return {};
        if (same_geometry(a, b)) {
            consistent = true;
            break;
        }
    }
    // After three moving reads the last one is used and the next probe scrapes again.
    need_full_ = !consistent;

    std::string o;
    const bool alt = a.buf_h <= a.win_h;
    if (alt != alt_) {
        set_sgr(o, kAttrDefault);
        o += alt ? "\x1b[?1049h" : "\x1b[?1049l";
        alt_ = alt;
        valid_ = false;
    }
    if (!valid_ || w != w_ || h != h_ || (!alt && a.win_top < top_) ||
        (!alt && a.win_top > top_ && from == a.win_top)) {
        reset_model(o, w, h, a.win_top);
    } else if (!alt && a.win_top > top_) {
        // The window moved down: emit each leaving row, then scroll it into the host scrollback.
        const int32_t k = a.win_top - top_;
        for (int32_t i = 0; i < k; ++i) {
            const int32_t abs_row = top_ + i;
            emit_row(o, 0, &scratch_[static_cast<std::size_t>(abs_row - from) * static_cast<std::size_t>(w)]);
            set_sgr(o, kAttrDefault);
            cup(o, h - 1, host_y_ == h - 1 && host_x_ >= 0 ? host_x_ : 0);
            o += '\n';
            std::move(model_.begin() + w, model_.end(), model_.begin());
            std::fill(model_.end() - w, model_.end(), ConCell{});
        }
        top_ = a.win_top;
    } else {
        top_ = a.win_top;
    }

    for (int y = 0; y < h; ++y) {
        emit_row(o, y, &scratch_[static_cast<std::size_t>(a.win_top + y - from) * static_cast<std::size_t>(w)]);
    }

    const int cy = std::clamp<int>(a.cur_y - a.win_top, 0, h - 1);
    const int cx = std::clamp<int>(a.cur_x - a.win_left, 0, w - 1);
    const bool moved = !same_geometry(a, last_);
    if (!o.empty() || moved || host_y_ != cy || host_x_ != cx) {
        host_y_ = -1;  // always reposition: the host may have wrapped
        cup(o, cy, cx);
    }
    if (a.cur_visible != host_visible_) {
        o += a.cur_visible ? "\x1b[?25h" : "\x1b[?25l";
        host_visible_ = a.cur_visible;
    }
    std::u16string t = con.title();
    if (t != title_) {
        title_ = t;
        o += "\x1b]2;";
        for (std::size_t i = 0; i < t.size(); ++i) {
            char16_t c = t[i];
            if (c < 0x20 || c == 0x7F) continue;
            if (is_high(c) && i + 1 < t.size() && is_low(t[i + 1])) {
                put_utf8(o, 0x10000 + ((char32_t{c} - 0xD800) << 10) + (char32_t{t[i + 1]} - 0xDC00));
                ++i;
            } else {
                put_utf8(o, (is_high(c) || is_low(c)) ? char32_t{0xFFFD} : char32_t{c});
            }
        }
        o += "\x1b\\";
    }
    last_ = a;
    if (o.empty()) return o;
    ++changes_;
    return "\x1b[?2026h" + o + "\x1b[?2026l";
}

}  // namespace kt::pty::agent
