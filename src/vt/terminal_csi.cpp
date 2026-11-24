// ESC and CSI dispatch, SGR and modes.
#include <algorithm>
#include <string>

#include "vt/terminal.h"

#ifndef KT_VERSION
#define KT_VERSION "0.0.0"
#endif

namespace kt::vt {
namespace {

uint16_t clamp_count(uint16_t v) { return v == 0 ? uint16_t{1} : v; }

std::string num(unsigned v) { return std::to_string(v); }

}  // namespace

// ----------------------------------------------------------------------------------- ESC

void Terminal::esc_dispatch(const Sequence& s) {
    const std::string_view im = s.intermediate_view();
    if (im.empty()) {
        switch (s.final) {
            case '7': save_cursor(); break;
            case '8': restore_cursor(); break;
            case 'D': index(); break;
            case 'E':  // NEL: index first, so a cursor right of the margins does not scroll (xterm)
                index();
                carriage_return();
                break;
            case 'H':
                tabs_[x_] = 1;
                break;
            case 'M': reverse_index(); break;
            case 'c': full_reset(); break;
            case 'Z': reply("\x1b[?62;22c"); break;  // DECID: answered as DA1
            case '=':
                modes_.keypad_app = true;
                notify_modes();
                break;
            case '>':
                modes_.keypad_app = false;
                notify_modes();
                break;
            case '6': back_index(); break;     // DECBI
            case '9': forward_index(); break;  // DECFI
            case 'N': single_shift_ = 2; break;
            case 'O': single_shift_ = 3; break;
            case 'n': gl_ = 2; break;  // LS2
            case 'o': gl_ = 3; break;  // LS3
            default: break;            // ESC \ (ST) and unknown: ignored
        }
        return;
    }
    if (im.size() == 1) {
        const char i0 = im[0];
        if (i0 == '(' || i0 == ')' || i0 == '*' || i0 == '+') {
            const std::size_t g = i0 == '(' ? 0u : i0 == ')' ? 1u : i0 == '*' ? 2u : 3u;
            switch (s.final) {
                case 'B': g_[g] = Charset::Ascii; break;
                case '0': g_[g] = Charset::DecGraphics; break;
                case 'A': g_[g] = Charset::Uk; break;
                default: g_[g] = Charset::Ascii; break;  // unsupported sets map to ASCII
            }
            return;
        }
        if (i0 == '#' && s.final == '8') {
            decaln();
            return;
        }
    }
}

// ----------------------------------------------------------------------------------- CSI

void Terminal::csi_dispatch(const Sequence& s) {
    const Params& p = s.params;
    const std::string_view im = s.intermediate_view();
    const auto arg = [&p](std::size_t n, uint16_t def) { return p.get(n, def); };
    const auto count = [&p](std::size_t n) { return clamp_count(p.get(n, 1)); };
    // Raw value of top-level parameter n (0 when absent), for parameters whose default is 0.
    const auto raw = [&p](std::size_t n) { return p.get(n, 0); };

    if (s.prefix == 0 && im.empty()) {
        switch (s.final) {
            case '@': insert_chars(count(0)); return;
            case 'A': cursor_up(count(0)); return;
            case 'B': cursor_down(count(0)); return;
            case 'C': cursor_right(count(0)); return;
            case 'D': cursor_left(count(0)); return;
            case 'E':
                cursor_down(count(0));
                carriage_return();
                return;
            case 'F':
                cursor_up(count(0));
                carriage_return();
                return;
            case 'G':
            case '`': {  // CHA, HPA
                const int col = count(0) - 1;
                if (dec(6)) {
                    x_ = static_cast<uint16_t>(std::clamp(col + left_, int{left_}, int{right_}));
                } else {
                    x_ = static_cast<uint16_t>(std::clamp(col, 0, cols() - 1));
                }
                pending_wrap_ = false;
                return;
            }
            case 'a': cursor_right(count(0)); return;  // HPR
            case 'H':
            case 'f': cup(count(0) - 1, count(1) - 1); return;
            case 'I': tab_forward(count(0)); return;
            case 'Z': tab_backward(count(0)); return;
            case 'J': erase_display(raw(0), false); return;
            case 'K': erase_line(raw(0), false); return;
            case 'L': insert_lines(count(0)); return;
            case 'M': delete_lines(count(0)); return;
            case 'P': delete_chars(count(0)); return;
            case 'X': erase_chars(count(0)); return;
            case 'S': scroll_up(count(0)); return;
            case 'T':
                if (p.top_count() <= 1) scroll_down(count(0));  // 5 params: mouse highlight, ignored
                return;
            case 'b': repeat_last(count(0)); return;
            case 'd': {  // VPA
                const int row = count(0) - 1;
                if (dec(6)) {
                    y_ = static_cast<uint16_t>(std::clamp(row + top_, int{top_}, int{bottom_}));
                } else {
                    y_ = static_cast<uint16_t>(std::clamp(row, 0, rows() - 1));
                }
                pending_wrap_ = false;
                return;
            }
            case 'e': cursor_down(count(0)); return;  // VPR
            case 'c':
                if (raw(0) == 0) reply("\x1b[?62;22c");  // DA1
                return;
            case 'g':
                if (raw(0) == 0) {
                    tabs_[x_] = 0;
                } else if (raw(0) == 3) {
                    std::fill(tabs_.begin(), tabs_.end(), uint8_t{0});
                }
                return;
            case 'h':
            case 'l':
                for (std::size_t i = 0; i < p.top_count(); ++i) set_ansi_mode(raw(i), s.final == 'h');
                return;
            case 'm': sgr(p); return;
            case 'n':
                if (raw(0) == 5) {
                    reply("\x1b[0n");
                } else if (raw(0) == 6) {
                    const unsigned row = static_cast<unsigned>(y_ - (dec(6) ? top_ : 0)) + 1u;
                    const unsigned col = static_cast<unsigned>(x_ - (dec(6) ? left_ : 0)) + 1u;
                    reply("\x1b[" + num(row) + ";" + num(col) + "R");
                }
                return;
            case 'r': {  // DECSTBM
                const uint16_t top = arg(0, 1);
                const uint16_t bottom = std::min<uint16_t>(arg(1, rows()), rows());
                if (top < bottom) {
                    top_ = static_cast<uint16_t>(top - 1u);
                    bottom_ = static_cast<uint16_t>(bottom - 1u);
                    cup(0, 0);
                }
                return;
            }
            case 's':
                if (lr_margins()) {  // DECSLRM
                    const uint16_t left = arg(0, 1);
                    const uint16_t right = std::min<uint16_t>(arg(1, cols()), cols());
                    if (left < right) {
                        left_ = static_cast<uint16_t>(left - 1u);
                        right_ = static_cast<uint16_t>(right - 1u);
                        cup(0, 0);
                    }
                } else {
                    save_cursor();  // SCOSC
                }
                return;
            case 'u':
                if (p.count == 0) restore_cursor();  // SCORC
                return;
            case 't': window_op(p); return;  // XTWINOPS
            default: return;
        }
    }

    if (s.prefix == '?' && im.empty()) {
        switch (s.final) {
            case 'h':
            case 'l':
                for (std::size_t i = 0; i < p.top_count(); ++i) set_dec_mode(raw(i), s.final == 'h');
                return;
            case 'J': erase_display(raw(0), true); return;  // DECSED
            case 'K': erase_line(raw(0), true); return;     // DECSEL
            case 'n':
                switch (raw(0)) {
                    case 6: {  // DECXCPR
                        const unsigned row = static_cast<unsigned>(y_ - (dec(6) ? top_ : 0)) + 1u;
                        const unsigned col = static_cast<unsigned>(x_ - (dec(6) ? left_ : 0)) + 1u;
                        reply("\x1b[?" + num(row) + ";" + num(col) + ";1R");
                        return;
                    }
                    // The VT420 status reports, answered as a terminal without the device.
                    case 15: reply("\x1b[?13n"); return;          // no printer
                    case 25: reply("\x1b[?20n"); return;          // UDKs unlocked
                    case 26: reply("\x1b[?27;1n"); return;        // keyboard: North American (VT220 form, as DA1)
                    case 53:
                    case 55: reply("\x1b[?53n"); return;          // no locator
                    case 56: reply("\x1b[?57;0n"); return;        // locator type: none
                    case 62: reply("\x1b[0*{"); return;           // DECMSR: no macro space
                    case 63: reply("\x1bP" + num(raw(1)) + "!~0000\x1b\\"); return;  // DECCKSR
                    case 75: reply("\x1b[?70n"); return;          // data integrity: no errors
                    case 85: reply("\x1b[?83n"); return;          // no multiple sessions
                    default: return;
                }
            case 's': save_dec_modes(p); return;     // XTSAVE
            case 'r': restore_dec_modes(p); return;  // XTRESTORE
            default: return;
        }
    }

    if (s.prefix == '>' && im.empty()) {
        switch (s.final) {
            case 'c':
                if (raw(0) == 0) reply("\x1b[>1;10;0c");  // DA2
                return;
            case 'q':
                if (raw(0) == 0) reply("\x1bP>|keepterm " KT_VERSION "\x1b\\");  // XTVERSION
                return;
            case 'm':  // XTMODKEYS
                if (p.count == 0) {
                    modes_.modify_other_keys = 0;
                } else if (raw(0) == 4) {
                    modes_.modify_other_keys = static_cast<uint8_t>(std::min<uint16_t>(raw(1), 2));
                } else {
                    return;
                }
                notify_modes();
                return;
            default: return;
        }
    }

    if (s.prefix == '=' && im.empty() && s.final == 'c') {
        if (raw(0) == 0) reply("\x1bP!|00000000\x1b\\");  // DA3
        return;
    }

    if (im == "$" && s.final == 'p' && (s.prefix == 0 || s.prefix == '?')) {
        report_mode(s);
        return;
    }
    if (s.prefix == 0 && im == " " && s.final == 'q') {  // DECSCUSR
        const uint16_t v = raw(0);
        if (v > 6) return;
        if (v == 0) {  // the user's default style
            shape_ = default_shape_;
            shape_default_ = true;
            dec_.set(static_cast<std::size_t>(dec_slot(12)), default_blink_);
            return;
        }
        shape_ = v <= 2 ? CursorShape::Block : v <= 4 ? CursorShape::Underline : CursorShape::Bar;
        shape_default_ = false;
        dec_.set(static_cast<std::size_t>(dec_slot(12)), (v % 2) == 1);
        return;
    }
    if (s.prefix == 0 && im == "!" && s.final == 'p') {
        soft_reset();
        return;
    }
    if (s.prefix == 0 && im == "\"" && s.final == 'q') {  // DECSCA
        pen_.prot = raw(0) == 1;
        return;
    }
    if (s.prefix == 0 && im == "*" && s.final == 'y') {  // DECRQCRA
        if (opts_.allow_rect_checksum) rect_checksum(p);
        return;
    }
    if (s.prefix == 0 && im == "'") {
        if (s.final == '}') insert_columns(count(0));  // DECIC
        if (s.final == '~') delete_columns(count(0));  // DECDC
        return;
    }
    if (s.prefix == 0 && im == " ") {
        if (s.final == '@') scroll_columns(count(0), true);   // SL
        if (s.final == 'A') scroll_columns(count(0), false);  // SR
        return;
    }
}

// XTWINOPS (CSI Ps ; Ps ; Ps t). Window manipulation (1-9) is not for programs to do; the
// title stack and the size reports are. Pixel reports (14, 16) are answered only when the
// owner gave the cell size in GridSize; otherwise there is no answer, as with a window system
// that refuses them.
void Terminal::window_op(const Params& p) {
    const uint16_t op = p.get(0, 0);
    const uint16_t which = p.get(1, 0);  // 22/23: 0 both, 1 icon name, 2 title
    const unsigned cw = size_.cell_w_px, ch = size_.cell_h_px;
    switch (op) {
        case 14:  // text area in pixels (14;2 asks for the window: the same here)
            if (cw != 0 && ch != 0) reply("\x1b[4;" + num(ch * rows()) + ";" + num(cw * cols()) + "t");
            return;
        case 16:  // cell size in pixels
            if (cw != 0 && ch != 0) reply("\x1b[6;" + num(ch) + ";" + num(cw) + "t");
            return;
        case 18:  // text area size in characters
        case 19:  // screen size in characters: the same here
            reply("\x1b[" + num(op == 18 ? 8u : 9u) + ";" + num(rows()) + ";" + num(cols()) + "t");
            return;
        case 20:  // icon name report, and 21: title report (a known injection vector: opt-in)
        case 21: {
            if (!opts_.allow_title_query) return;
            const std::u8string& v = op == 20 ? icon_name_ : title_;
            reply(std::string(op == 20 ? "\x1b]L" : "\x1b]l") + std::string(v.begin(), v.end()) + "\x1b\\");
            return;
        }
        case 22:
            if (which > 2) return;
            if (title_stack_.size() == kTitleStackMax) title_stack_.erase(title_stack_.begin());
            title_stack_.emplace_back(title_, icon_name_);
            return;
        case 23: {
            if (which > 2 || title_stack_.empty()) return;
            auto [title, icon] = std::move(title_stack_.back());
            title_stack_.pop_back();
            if (which != 2) icon_name_ = std::move(icon);
            if (which != 1 && title != title_) {
                title_ = std::move(title);
                if (sink_) sink_->title_changed(title_);
            }
            return;
        }
        default: return;
    }
}

// DECRQCRA (CSI Pid ; Pp ; Pt ; Pl ; Pb ; Pr * y): DCS Pid ! ~ XXXX ST, the 16-bit sum of the
// code points in the rectangle (origin mode aware). As xterm since patch 334, which follows the
// DEC documentation: no negation, no attributes, empty and erased cells count as a space, and a
// wide character counts once.
void Terminal::rect_checksum(const Params& p) {
    const unsigned id = p.get(0, 0);
    const bool om = dec(6);
    const int oy = om ? top_ : 0, ox = om ? left_ : 0;
    const int maxy = om ? bottom_ : rows() - 1, maxx = om ? right_ : cols() - 1;
    const int t = std::clamp(oy + int{p.get(2, 1)} - 1, oy, maxy);
    const int l = std::clamp(ox + int{p.get(3, 1)} - 1, ox, maxx);
    const int b = std::clamp(oy + int{p.get(4, static_cast<uint16_t>(maxy - oy + 1))} - 1, oy, maxy);
    const int r = std::clamp(ox + int{p.get(5, static_cast<uint16_t>(maxx - ox + 1))} - 1, ox, maxx);
    uint32_t sum = 0;
    for (int y = t; y <= b; ++y) {
        const auto row = active_->row(static_cast<uint16_t>(y));
        for (int x = l; x <= r; ++x) {
            const Cell& c = row[static_cast<std::size_t>(x)];
            if (c.width() == CellWidth::WideTail) continue;
            if (c.width() == CellWidth::Empty || c.cp() == 0) {
                sum += 0x20u;
            } else if (c.cluster()) {
                const std::u32string_view g = graphemes_[c.cp()];
                sum += g.empty() ? 0u : static_cast<uint32_t>(g[0]);  // the base character
            } else {
                sum += c.cp();
            }
        }
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string hex(4, '0');
    for (int i = 3; i >= 0; --i, sum >>= 4) hex[static_cast<std::size_t>(i)] = kHex[sum & 0xFu];
    reply("\x1bP" + num(id) + "!~" + hex + "\x1b\\");
}

// ----------------------------------------------------------------------------------- DCS

void Terminal::dcs_hook(const Sequence& s) {
    dcs_buf_.clear();
    dcs_kind_ = 0;
    const std::string_view im = s.intermediate_view();
    if (s.prefix == 0 && s.params.count == 0 && s.final == 'q') {
        if (im == "$") dcs_kind_ = 1;  // DECRQSS
        if (im == "+") dcs_kind_ = 2;  // XTGETTCAP
    }
}

void Terminal::dcs_put(uint8_t b) {
    constexpr std::size_t kMaxRequest = 256;  // a request is a few bytes; anything longer is noise
    if (dcs_kind_ == 0) return;
    if (dcs_buf_.size() == kMaxRequest) {
        dcs_kind_ = 0;
        return;
    }
    dcs_buf_.push_back(static_cast<char>(b));
}

void Terminal::dcs_unhook() {
    const uint8_t kind = dcs_kind_;
    dcs_kind_ = 0;
    if (kind == 1) request_status(dcs_buf_);
    if (kind == 2) request_termcap(dcs_buf_);
    dcs_buf_.clear();
}

// The current SGR as DECRQSS reports it: "0" followed by every attribute that is set.
std::string Terminal::sgr_string() const {
    const Style& st = pen_.style;
    std::string out = "0";
    const auto add = [&out](std::string_view v) {
        out.push_back(';');
        out.append(v);
    };
    if (st.has(kBold)) add("1");
    if (st.has(kFaint)) add("2");
    if (st.has(kItalic)) add("3");
    switch (st.underline) {
        case Underline::None: break;
        case Underline::Single: add("4"); break;
        case Underline::Double: add("4:2"); break;
        case Underline::Curly: add("4:3"); break;
        case Underline::Dotted: add("4:4"); break;
        case Underline::Dashed: add("4:5"); break;
    }
    if (st.has(kBlink)) add("5");
    if (st.has(kInverse)) add("7");
    if (st.has(kInvisible)) add("8");
    if (st.has(kStrike)) add("9");
    if (st.has(kOverline)) add("53");
    const auto color = [&](Color c, unsigned base, bool has_short) {
        if (c.tag() == Color::Palette) {
            const unsigned i = c.index();
            if (has_short && i < 8) {
                add(num(base - 8u + i));
            } else if (has_short && i < 16) {
                add(num(base + 44u + i));  // 90-97, 100-107
            } else {
                add(num(base) + ":5:" + num(i));
            }
        } else if (c.tag() == Color::Rgb) {
            add(num(base) + ":2::" + num(c.r()) + ":" + num(c.g()) + ":" + num(c.b()));
        }
    };
    color(st.fg, 38, true);
    color(st.bg, 48, true);
    color(st.underline_color, 58, false);
    return out;
}

// DECRQSS (DCS $ q Pt ST). Answers DCS 1 $ r <setting> ST, or DCS 0 $ r ST for a setting it
// does not know (the VT510 reply; xterm uses the same codes).
void Terminal::request_status(std::string_view what) {
    std::string v;
    if (what == "m") {
        v = sgr_string() + "m";
    } else if (what == "r") {
        v = num(top_ + 1u) + ";" + num(bottom_ + 1u) + "r";
    } else if (what == "s") {
        v = num(left_ + 1u) + ";" + num(right_ + 1u) + "s";
    } else if (what == " q") {
        const unsigned base = shape_ == CursorShape::Block ? 1u : shape_ == CursorShape::Underline ? 3u : 5u;
        v = num(base + (dec(12) ? 0u : 1u)) + " q";
    } else if (what == "\"q") {
        v = std::string(pen_.prot ? "1" : "0") + "\"q";
    } else if (what == "\"p") {
        v = "62;1\"p";  // DECSCL: VT200 level (matches DA1), 7-bit controls
    } else if (what == "t") {
        v = num(rows()) + "t";  // DECSLPP
    } else if (what == "$|") {
        v = num(cols()) + "$|";  // DECSCPP
    } else if (what == "*|") {
        v = num(rows()) + "*|";  // DECSNLS
    } else if (what == "$}") {
        v = "0$}";  // DECSASD: there is no status line, the main display is active
    } else if (what == "$~") {
        v = "0$~";  // DECSSDT: no status line
    }
    if (v.empty()) {
        reply("\x1bP0$r\x1b\\");
    } else {
        reply("\x1bP1$r" + v + "\x1b\\");
    }
}

// XTGETTCAP (DCS + q hex(name) [; hex(name)...] ST). Only the capabilities that describe the
// terminal itself: its terminfo name (the TERM the hosts set), colors and direct color. Others
// get DCS 0 + r.
void Terminal::request_termcap(std::string_view hex) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    const auto decode = [](std::string_view h, std::string& out) {
        if (h.size() % 2 != 0) return false;
        for (std::size_t i = 0; i < h.size(); i += 2) {
            int v = 0;
            for (int k = 0; k < 2; ++k) {
                const char c = h[i + static_cast<std::size_t>(k)];
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) return false;
                v = v * 16 + d;
            }
            out.push_back(static_cast<char>(v));
        }
        return true;
    };
    const auto encode = [](std::string_view v) {
        std::string out;
        for (char c : v) {
            out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xFu]);
            out.push_back(kHex[static_cast<unsigned char>(c) & 0xFu]);
        }
        return out;
    };
    while (!hex.empty()) {
        const std::size_t semi = hex.find(';');
        const std::string_view h = hex.substr(0, semi);
        hex = semi == std::string_view::npos ? std::string_view{} : hex.substr(semi + 1);
        std::string name;
        if (!decode(h, name)) {
            reply("\x1bP0+r\x1b\\");
            return;
        }
        std::string value;
        if (name == "Co" || name == "colors") value = "256";
        if (name == "RGB") value = "8/8/8";
        if (name == "TN" || name == "name") value = "xterm-256color";
        if (value.empty()) {
            reply("\x1bP0+r" + std::string(h) + "\x1b\\");
        } else {
            reply("\x1bP1+r" + std::string(h) + "=" + encode(value) + "\x1b\\");
        }
    }
}

void Terminal::report_mode(const Sequence& s) {
    const uint16_t mode = s.params.get(0, 0);
    if (s.prefix == '?') {
        reply("\x1b[?" + num(mode) + ";" + num(static_cast<unsigned>(dec_mode(mode))) + "$y");
    } else {
        reply("\x1b[" + num(mode) + ";" + num(static_cast<unsigned>(ansi_mode(mode))) + "$y");
    }
}

// ----------------------------------------------------------------------------------- SGR

void Terminal::sgr(const Params& p) {
    const std::size_t n = std::min<std::size_t>(p.count, Params::kMax);
    Style& st = pen_.style;
    if (n == 0) {
        st = Style{};
        update_pen();
        return;
    }
    // Start indices of the top-level parameters.
    std::array<uint8_t, Params::kMax + 1> starts{};
    std::size_t groups = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (p.is_start(i)) starts[groups++] = static_cast<uint8_t>(i);
    }
    starts[groups] = static_cast<uint8_t>(n);

    // Parses an extended color (38/48/58) at group g. Returns the number of extra groups used.
    const auto ext_color = [&](std::size_t g, Color& out) -> std::size_t {
        const std::size_t i = starts[g];
        const std::size_t sub = starts[g + 1] - i - 1u;  // subparameters after 38
        if (sub > 0) {  // colon form: 38:5:n, 38:2::r:g:b, 38:2:r:g:b
            const uint16_t kind = p.values[i + 1];
            if (kind == 5 && sub >= 2) {
                if (p.values[i + 2] <= 255) out = Color::palette(static_cast<uint8_t>(p.values[i + 2]));
            } else if (kind == 2 && sub >= 4) {
                const std::size_t c = sub >= 5 ? i + 3 : i + 2;  // skip the color space id
                if (p.values[c] <= 255 && p.values[c + 1] <= 255 && p.values[c + 2] <= 255) {
                    out = Color::rgb(static_cast<uint8_t>(p.values[c]), static_cast<uint8_t>(p.values[c + 1]),
                                     static_cast<uint8_t>(p.values[c + 2]));
                }
            }
            return 0;
        }
        // Semicolon form: 38;5;n and 38;2;r;g;b use the following top-level parameters.
        if (g + 1 >= groups) return 0;
        const uint16_t kind = p.values[starts[g + 1]];
        if (kind == 5) {
            if (g + 2 >= groups) return groups - g - 1u;
            const uint16_t idx = p.values[starts[g + 2]];
            if (idx <= 255) out = Color::palette(static_cast<uint8_t>(idx));
            return 2;
        }
        if (kind == 2) {
            if (g + 4 >= groups) return groups - g - 1u;
            const uint16_t r = p.values[starts[g + 2]], gr = p.values[starts[g + 3]], b = p.values[starts[g + 4]];
            if (r <= 255 && gr <= 255 && b <= 255) {
                out = Color::rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b));
            }
            return 4;
        }
        return 1;
    };

    for (std::size_t g = 0; g < groups; ++g) {
        const std::size_t i = starts[g];
        const uint16_t v = p.values[i];
        const bool has_sub = starts[g + 1] - i > 1u;
        switch (v) {
            case 0: st = Style{}; break;
            case 1: st.set(kBold, true); break;
            case 2: st.set(kFaint, true); break;
            case 3: st.set(kItalic, true); break;
            case 4:
                if (has_sub) {
                    const uint16_t u = p.values[i + 1];
                    if (u <= 5) st.underline = static_cast<Underline>(u);
                } else {
                    st.underline = Underline::Single;
                }
                break;
            case 5:
            case 6: st.set(kBlink, true); break;
            case 7: st.set(kInverse, true); break;
            case 8: st.set(kInvisible, true); break;
            case 9: st.set(kStrike, true); break;
            case 21: st.underline = Underline::Double; break;
            case 22:
                st.set(kBold, false);
                st.set(kFaint, false);
                break;
            case 23: st.set(kItalic, false); break;
            case 24: st.underline = Underline::None; break;
            case 25: st.set(kBlink, false); break;
            case 27: st.set(kInverse, false); break;
            case 28: st.set(kInvisible, false); break;
            case 29: st.set(kStrike, false); break;
            case 38: g += ext_color(g, st.fg); break;
            case 39: st.fg = Color::def(); break;
            case 48: g += ext_color(g, st.bg); break;
            case 49: st.bg = Color::def(); break;
            case 53: st.set(kOverline, true); break;
            case 55: st.set(kOverline, false); break;
            case 58: g += ext_color(g, st.underline_color); break;
            case 59: st.underline_color = Color::def(); break;
            default:
                if (v >= 30 && v <= 37) {
                    st.fg = Color::palette(static_cast<uint8_t>(v - 30));
                } else if (v >= 40 && v <= 47) {
                    st.bg = Color::palette(static_cast<uint8_t>(v - 40));
                } else if (v >= 90 && v <= 97) {
                    st.fg = Color::palette(static_cast<uint8_t>(v - 90 + 8));
                } else if (v >= 100 && v <= 107) {
                    st.bg = Color::palette(static_cast<uint8_t>(v - 100 + 8));
                }
                break;  // unknown: ignored
        }
    }
    update_pen();
}

// --------------------------------------------------------------------------------- modes

void Terminal::set_ansi_mode(uint16_t mode, bool on) {
    if (mode == 4) insert_mode_ = on;
    if (mode == 20) lnm_ = on;
}

void Terminal::save_dec_modes(const Params& p) {
    for (std::size_t i = 0; i < p.top_count(); ++i) {
        const uint16_t mode = p.get(i, 0);
        const int v = dec_mode(mode);
        if (v != 1 && v != 2) continue;  // unknown or permanent: nothing to restore
        const bool on = v == 1;
        auto it = std::find_if(saved_dec_modes_.begin(), saved_dec_modes_.end(),
                               [mode](const auto& e) { return e.first == mode; });
        if (it != saved_dec_modes_.end()) {
            it->second = on;
        } else {
            saved_dec_modes_.emplace_back(mode, on);
        }
    }
}

void Terminal::restore_dec_modes(const Params& p) {
    for (std::size_t i = 0; i < p.top_count(); ++i) {
        const uint16_t mode = p.get(i, 0);
        const auto it = std::find_if(saved_dec_modes_.begin(), saved_dec_modes_.end(),
                                     [mode](const auto& e) { return e.first == mode; });
        if (it != saved_dec_modes_.end()) set_dec_mode(mode, it->second);
    }
}

void Terminal::set_dec_mode(uint16_t mode, bool on) {
    if (mode == 1045) {
        rev_wrap_ext_ = on;
        return;
    }
    const int slot = dec_slot(mode);
    if (slot < 0) return;  // unknown: ignored
    const auto bit = static_cast<std::size_t>(slot);
    switch (mode) {
        case 1:
            modes_.cursor_keys_app = on;
            break;
        case 3:
            // DECCOLM: only with mode 40; clears the screen, never resizes the window.
            if (!dec(40)) return;
            erase_display(2, false);
            reset_margins();
            move_to(0, 0);
            break;
        case 5:
            active_->mark_all_dirty();  // DECSCNM: the view inverts the screen
            break;
        case 6:
            dec_.set(bit, on);
            cup(0, 0);
            return;
        case 7:
            if (!on) pending_wrap_ = false;
            break;
        case 9:
        case 1000:
        case 1002:
        case 1003: {
            // The tracking mode set last wins; resetting the active one turns tracking off.
            const MouseTracking t = mode == 9      ? MouseTracking::X10
                                    : mode == 1000 ? MouseTracking::Normal
                                    : mode == 1002 ? MouseTracking::Button
                                                   : MouseTracking::Any;
            if (on) {
                modes_.mouse_tracking = t;
            } else if (modes_.mouse_tracking == t) {
                modes_.mouse_tracking = MouseTracking::None;
            }
            break;
        }
        case 1005:
        case 1006:
        case 1015:
        case 1016: {
            const MouseFormat f = mode == 1005   ? MouseFormat::Utf8
                                  : mode == 1006 ? MouseFormat::Sgr
                                  : mode == 1015 ? MouseFormat::Urxvt
                                                 : MouseFormat::SgrPixels;
            if (on) {
                modes_.mouse_format = f;
            } else if (modes_.mouse_format == f) {
                modes_.mouse_format = MouseFormat::Default;
            }
            break;
        }
        case 1004: modes_.focus_events = on; break;
        case 1007: modes_.alt_scroll = on; break;
        case 2004: modes_.bracketed_paste = on; break;
        case 9001: modes_.win32_input = on; break;
        case 66: modes_.keypad_app = on; break;
        case 67: modes_.backarrow_sends_bs = on; break;
        case 69:
            if (!on) {
                left_ = 0;
                right_ = static_cast<uint16_t>(cols() - 1u);
            }
            break;
        case 47:
            switch_screen(on, false);
            break;
        case 1047:
            if (on) {
                switch_screen(true, false);
            } else {
                if (alt_active()) alt_->clear(blank());
                switch_screen(false, false);
            }
            break;
        case 1048:
            if (on) {
                save_cursor();
            } else {
                restore_cursor();
            }
            break;
        case 1049:
            if (on) {
                if (!alt_active()) save_cursor();
                switch_screen(true, true);
            } else {
                const bool was_alt = alt_active();
                switch_screen(false, false);
                if (was_alt) restore_cursor();
            }
            break;
        case 2026:
            if (on && !sync_) sync_since_ = std::chrono::steady_clock::now();
            sync_ = on;
            break;
        default:
            break;
    }
    dec_.set(bit, on);
    switch (mode) {
        case 1: case 9: case 66: case 67: case 1000: case 1002: case 1003: case 1004: case 1005:
        case 1006: case 1007: case 1015: case 1016: case 2004: case 9001:
            notify_modes();
            break;
        default:
            break;
    }
}

}  // namespace kt::vt
