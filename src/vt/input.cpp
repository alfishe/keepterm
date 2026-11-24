#include "vt/input.h"

#include <array>
#include <optional>

#include "vt/utf8.h"

namespace kt::vt {

namespace {

constexpr std::string_view kCsi = "\x1b[";
constexpr std::string_view kSs3 = "\x1bO";

constexpr uint8_t kModMask = kModShift | kModAlt | kModCtrl | kModSuper;

int key_index(Key k) { return static_cast<int>(k); }
bool in_range(Key k, Key first, Key last) { return key_index(k) >= key_index(first) && key_index(k) <= key_index(last); }

void put_num(std::string& out, unsigned v) { out += std::to_string(v); }

// US layout characters for the printable keys: unshifted, shifted. Used when the platform
// gives no text and no logical character (tests, the CLI's `pane key`, a stripped event).
struct UsChars {
    char base, shifted;
};
std::optional<UsChars> us_chars(Key k) {
    if (in_range(k, Key::a, Key::z)) {
        const char c = static_cast<char>('a' + (key_index(k) - key_index(Key::a)));
        return UsChars{c, static_cast<char>(c - 'a' + 'A')};
    }
    if (in_range(k, Key::d1, Key::d0)) {
        static constexpr char base[] = "1234567890";
        static constexpr char shifted[] = "!@#$%^&*()";
        const auto i = static_cast<std::size_t>(key_index(k) - key_index(Key::d1));
        return UsChars{base[i], shifted[i]};
    }
    switch (k) {
        case Key::space: return UsChars{' ', ' '};
        case Key::minus: return UsChars{'-', '_'};
        case Key::equal: return UsChars{'=', '+'};
        case Key::left_bracket: return UsChars{'[', '{'};
        case Key::right_bracket: return UsChars{']', '}'};
        case Key::backslash: return UsChars{'\\', '|'};
        case Key::non_us_hash: return UsChars{'#', '~'};
        case Key::semicolon: return UsChars{';', ':'};
        case Key::apostrophe: return UsChars{'\'', '"'};
        case Key::grave: return UsChars{'`', '~'};
        case Key::comma: return UsChars{',', '<'};
        case Key::period: return UsChars{'.', '>'};
        case Key::slash: return UsChars{'/', '?'};
        case Key::non_us_backslash: return UsChars{'\\', '|'};
        default: return std::nullopt;
    }
}

// The single code point of `text`, if it is exactly one printable code point.
std::optional<char32_t> single_printable(std::u8string_view text) {
    if (text.empty()) return std::nullopt;
    const auto b0 = static_cast<uint8_t>(text[0]);
    char32_t cp = 0;
    std::size_t len = 0;
    if (b0 < 0x80) {
        cp = b0;
        len = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1Fu;
        len = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0Fu;
        len = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07u;
        len = 4;
    } else {
        return std::nullopt;
    }
    if (text.size() != len) return std::nullopt;
    for (std::size_t i = 1; i < len; ++i) {
        const auto b = static_cast<uint8_t>(text[i]);
        if ((b & 0xC0) != 0x80) return std::nullopt;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return std::nullopt;
    return cp;
}

char32_t ascii_upper(char32_t c) { return (c >= U'a' && c <= U'z') ? c - U'a' + U'A' : c; }
bool ascii_letter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }

// The character the key types with the current Shift state (Ctrl, Alt and Super ignored).
std::optional<char32_t> typed_char(const KeyEvent& ev) {
    if (auto cp = single_printable(ev.text)) return cp;
    const bool shift = (ev.mods & kModShift) != 0;
    if (ev.logical >= 0x20 && ev.logical != 0x7F) {
        if (!shift) return ev.logical;
        if (ascii_letter(ev.logical)) return ascii_upper(ev.logical);
        // The shifted form of a non-letter depends on the layout: only the US table knows it.
        if (auto us = us_chars(ev.key); us && static_cast<char32_t>(us->base) == ev.logical)
            return static_cast<char32_t>(us->shifted);
        return ev.logical;
    }
    if (auto us = us_chars(ev.key)) return static_cast<char32_t>(shift ? us->shifted : us->base);
    return std::nullopt;
}

// The unshifted character of the key; Latin letters come from the key position when the layout
// is not Latin, so Ctrl+C works on a Cyrillic layout (xterm does the same through the keysym).
std::optional<char32_t> base_char(const KeyEvent& ev) {
    if (in_range(ev.key, Key::a, Key::z)) {
        if (ascii_letter(ev.logical)) return ev.logical | 0x20u;
        return static_cast<char32_t>(us_chars(ev.key)->base);
    }
    if (ev.logical >= 0x20 && ev.logical != 0x7F) return ev.logical;
    if (auto us = us_chars(ev.key)) return static_cast<char32_t>(us->base);
    return std::nullopt;
}

// C0 for Ctrl + character (xterm / VT220 rules), or nothing.
std::optional<char> ctrl_code(char32_t c) {
    if (ascii_letter(c)) return static_cast<char>(c & 0x1Fu);
    switch (c) {
        case U'@': case U' ': case U'2': return '\0';
        case U'[': case U'3': return '\x1b';
        case U'\\': case U'4': return '\x1c';
        case U']': case U'5': return '\x1d';
        case U'^': case U'6': return '\x1e';
        case U'_': case U'7': case U'/': return '\x1f';
        case U'?': case U'8': return '\x7f';
        default: return std::nullopt;
    }
}

// modifyOtherKeys form: CSI 27 ; m ; code ~
void put_mok(std::string& out, unsigned m, char32_t code) {
    out += kCsi;
    out += "27;";
    put_num(out, m);
    out += ';';
    put_num(out, static_cast<unsigned>(code));
    out += '~';
}

// CSI final, SS3 final, or CSI 1 ; m final.
void put_cursor_like(std::string& out, char final, unsigned m, bool app) {
    if (m > 1) {
        out += kCsi;
        out += "1;";
        put_num(out, m);
    } else {
        out += app ? kSs3 : kCsi;
    }
    out += final;
}

// CSI n ~ or CSI n ; m ~
void put_tilde(std::string& out, unsigned n, unsigned m) {
    out += kCsi;
    put_num(out, n);
    if (m > 1) {
        out += ';';
        put_num(out, m);
    }
    out += '~';
}

bool encode_function_key(Key key, uint8_t mods, std::string& out) {
    int fn = 0;  // 1..12
    if (in_range(key, Key::f1, Key::f12)) {
        fn = key_index(key) - key_index(Key::f1) + 1;
    } else if (in_range(key, Key::f13, Key::f24)) {
        // xterm: F13..F24 are F1..F12 with Shift (F13 = CSI 1;2P, F17 = CSI 15;2~).
        fn = key_index(key) - key_index(Key::f13) + 1;
        mods |= kModShift;
    } else {
        return false;
    }
    const unsigned m = 1u + (mods & kModMask);
    if (fn <= 4) {
        const char final = static_cast<char>('P' + (fn - 1));
        put_cursor_like(out, final, m, /*app=*/true);  // SS3 P..S unmodified in both keypad modes
        return true;
    }
    static constexpr std::array<unsigned, 8> kCodes = {15, 17, 18, 19, 20, 21, 23, 24};
    put_tilde(out, kCodes[static_cast<std::size_t>(fn - 5)], m);
    return true;
}

// Keypad keys other than Enter: SS3 codes in application keypad mode (unless Num Lock is on),
// else their character.
std::optional<std::pair<char, char>> keypad_codes(Key k) {  // (SS3 final, character)
    if (in_range(k, Key::kp_1, Key::kp_9)) {
        const int i = key_index(k) - key_index(Key::kp_1);
        return std::pair{static_cast<char>('q' + i), static_cast<char>('1' + i)};
    }
    switch (k) {
        case Key::kp_0: return std::pair{'p', '0'};
        case Key::kp_decimal: return std::pair{'n', '.'};
        case Key::kp_divide: return std::pair{'o', '/'};
        case Key::kp_multiply: return std::pair{'j', '*'};
        case Key::kp_minus: return std::pair{'m', '-'};
        case Key::kp_plus: return std::pair{'k', '+'};
        case Key::kp_comma: return std::pair{'l', ','};
        case Key::kp_equal: return std::pair{'X', '='};
        default: return std::nullopt;
    }
}

void put_text(std::string& out, std::u8string_view text) {
    out.append(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace

bool InputEncoder::encode_key(const KeyEvent& ev, const InputModes& modes, const EncoderOptions& opts,
                              std::string& out) {
    if (ev.action == KeyAction::Release) return false;
    const uint8_t mods = ev.mods & kModMask;
    const bool shift = (mods & kModShift) != 0;
    const bool alt = (mods & kModAlt) != 0;
    const bool ctrl = (mods & kModCtrl) != 0;
    const unsigned m = 1u + mods;
    // A Windows console (ConPTY's conhost, or the agent) reads the keys, not the program: it
    // decodes the legacy forms into key records but drops the modifyOtherKeys ones, so Ctrl+C
    // and Ctrl+D would be lost when a program behind ConPTY asks for level 2.
    // The inbox conhost of Windows 10 does not set 9001, hence the option.
    const uint8_t mok = modes.win32_input || opts.console_reader ? 0 : modes.modify_other_keys;
    const bool esc_prefix = alt && opts.alt_sends_esc;

    switch (ev.key) {
        case Key::up: put_cursor_like(out, 'A', m, modes.cursor_keys_app); return true;
        case Key::down: put_cursor_like(out, 'B', m, modes.cursor_keys_app); return true;
        case Key::right: put_cursor_like(out, 'C', m, modes.cursor_keys_app); return true;
        case Key::left: put_cursor_like(out, 'D', m, modes.cursor_keys_app); return true;
        case Key::home: put_cursor_like(out, 'H', m, modes.cursor_keys_app); return true;
        case Key::end: put_cursor_like(out, 'F', m, modes.cursor_keys_app); return true;
        case Key::insert: put_tilde(out, 2, m); return true;
        case Key::del: put_tilde(out, 3, m); return true;
        case Key::page_up: put_tilde(out, 5, m); return true;
        case Key::page_down: put_tilde(out, 6, m); return true;

        case Key::backspace: {
            const bool bs = modes.backarrow_sends_bs || !opts.backspace_is_del;
            char c = bs ? '\x08' : '\x7f';
            if (ctrl) c = bs ? '\x7f' : '\x08';
            if (esc_prefix) out += '\x1b';
            out += c;
            return true;
        }
        case Key::enter:
            if (mok >= 1 && (mods & (kModShift | kModCtrl))) {
                put_mok(out, m, 13);
                return true;
            }
            if (esc_prefix) out += '\x1b';
            out += '\r';
            return true;
        case Key::kp_enter:
            if (modes.keypad_app && !(ev.mods & kModNumLock)) {
                out += kSs3;
                out += 'M';
                return true;
            }
            if (esc_prefix) out += '\x1b';
            out += '\r';
            return true;
        case Key::tab:
            if (mok >= 1 && ctrl) {
                put_mok(out, m, 9);
                return true;
            }
            if (esc_prefix) out += '\x1b';
            if (shift) {
                out += kCsi;
                out += 'Z';
            } else {
                out += '\t';
            }
            return true;
        case Key::escape:
            if (mok >= 1 && (mods & (kModShift | kModCtrl))) {
                put_mok(out, m, 27);
                return true;
            }
            if (esc_prefix) out += '\x1b';
            out += '\x1b';
            return true;
        default: break;
    }

    if (encode_function_key(ev.key, ev.mods, out)) return true;

    if (auto kp = keypad_codes(ev.key)) {
        if (modes.keypad_app && !(ev.mods & kModNumLock)) {
            out += kSs3;
            out += kp->first;
            return true;
        }
        if (esc_prefix) out += '\x1b';
        if (!ev.text.empty()) put_text(out, ev.text);
        else out += kp->second;
        return true;
    }

    // Printable keys: letters, digits, punctuation, space, or any key that typed text.
    const std::optional<char32_t> typed = typed_char(ev);
    if (!typed) {
        if (ev.text.empty()) return false;
        if (esc_prefix) out += '\x1b';
        put_text(out, ev.text);
        return true;
    }

    // Ctrl+C, Ctrl+D, Ctrl+Z and Ctrl+\ always send their C0 code: the tty turns them into
    // SIGINT, EOF, SIGTSTP and SIGQUIT only from that byte. A program that asked for
    // modifyOtherKeys and ended without resetting it (a crash, a kill) would otherwise leave a
    // shell whose programs cannot be interrupted.
    if (ctrl && !(mods & (kModAlt | kModShift | kModSuper))) {
        const std::optional<char32_t> base = base_char(ev);
        const char32_t b = base ? *base : *typed;
        if (b == U'c' || b == U'C' || b == U'd' || b == U'D' || b == U'z' || b == U'Z' || b == U'\\') {
            if (auto c0 = ctrl_code(b)) {
                out += *c0;
                return true;
            }
        }
    }

    // Level 2: every modified key except Shift alone (Shift+Space included).
    if (mok >= 2 && ((mods & (kModCtrl | kModAlt | kModSuper)) || (shift && *typed == U' '))) {
        put_mok(out, m, *typed);
        return true;
    }

    if (ctrl) {
        const std::optional<char32_t> base = base_char(ev);
        std::optional<char> c0;
        if (base && ascii_letter(*base)) c0 = ctrl_code(*base);
        else if (auto c = ctrl_code(*typed)) c0 = c;
        else if (base) c0 = ctrl_code(*base);
        const bool letter = base && ascii_letter(*base);
        if (c0 && !(mok >= 1 && shift && letter)) {
            if (esc_prefix) out += '\x1b';
            out += *c0;
            return true;
        }
        // No standard encoding (Ctrl+1, Ctrl+;, Ctrl+Shift+letter): level 1 reports it.
        if (mok >= 1) {
            put_mok(out, m, *typed);
            return true;
        }
        if (esc_prefix) out += '\x1b';
        append_utf8(out, *typed);
        return true;
    }

    if (esc_prefix) {
        out += '\x1b';
        append_utf8(out, *typed);
        return true;
    }
    // Plain text, or the character Option produced when Alt does not send ESC.
    if (!ev.text.empty()) put_text(out, ev.text);
    else append_utf8(out, *typed);
    return true;
}

namespace {

// Base button code (before the motion and modifier bits), or -1 for no button.
int button_code(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return 0;
        case MouseButton::Middle: return 1;
        case MouseButton::Right: return 2;
        case MouseButton::WheelUp: return 64;
        case MouseButton::WheelDown: return 65;
        case MouseButton::WheelLeft: return 66;
        case MouseButton::WheelRight: return 67;
        case MouseButton::Back: return 128;
        case MouseButton::Forward: return 129;
        case MouseButton::B10: return 130;
        case MouseButton::B11: return 131;
        case MouseButton::None: return -1;
    }
    return -1;
}

bool is_wheel(MouseButton b) {
    return b == MouseButton::WheelUp || b == MouseButton::WheelDown || b == MouseButton::WheelLeft ||
           b == MouseButton::WheelRight;
}

}  // namespace

bool InputEncoder::encode_mouse(const MouseEvent& ev, const InputModes& modes, std::string& out) {
    const MouseTracking t = modes.mouse_tracking;
    if (t == MouseTracking::None) {
        // 1007: in the alternate screen the wheel becomes cursor keys (three per notch).
        if (ev.action == MouseAction::Wheel && modes.alt_screen && modes.alt_scroll &&
            (ev.button == MouseButton::WheelUp || ev.button == MouseButton::WheelDown)) {
            const char final = ev.button == MouseButton::WheelUp ? 'A' : 'B';
            for (int i = 0; i < 3; ++i) put_cursor_like(out, final, 1, modes.cursor_keys_app);
            return true;
        }
        return false;
    }

    int code = 0;
    bool release = false;
    switch (ev.action) {
        case MouseAction::Press:
            if (is_wheel(ev.button) && t == MouseTracking::X10) return false;
            code = button_code(ev.button);
            if (code < 0) return false;
            break;
        case MouseAction::Wheel:
            if (!is_wheel(ev.button) || t == MouseTracking::X10) return false;
            code = button_code(ev.button);
            break;
        case MouseAction::Release:
            if (t == MouseTracking::X10 || is_wheel(ev.button)) return false;
            release = true;
            code = button_code(ev.button);
            if (code < 0) code = 0;
            break;
        case MouseAction::Move:
            if (t == MouseTracking::Any) {
            } else if (t == MouseTracking::Button) {
                if (ev.button == MouseButton::None) return false;
            } else {
                return false;
            }
            code = (ev.button == MouseButton::None ? 3 : button_code(ev.button)) + 32;
            break;
    }
    if (t != MouseTracking::X10) {
        if (ev.mods & kModShift) code += 4;
        if (ev.mods & kModAlt) code += 8;
        if (ev.mods & kModCtrl) code += 16;
    }

    const MouseFormat f = modes.mouse_format;
    if (f == MouseFormat::Sgr || f == MouseFormat::SgrPixels) {
        const bool pixels = f == MouseFormat::SgrPixels;
        out += kCsi;
        out += '<';
        put_num(out, static_cast<unsigned>(code));
        out += ';';
        put_num(out, (pixels ? ev.px : ev.col) + 1u);
        out += ';';
        put_num(out, (pixels ? ev.py : ev.row) + 1u);
        out += release ? 'm' : 'M';
        return true;
    }
    // The legacy encodings cannot tell which button was released.
    if (release) code = 3 + (code & (4 | 8 | 16));
    if (f == MouseFormat::Urxvt) {
        out += kCsi;
        put_num(out, static_cast<unsigned>(code) + 32u);
        out += ';';
        put_num(out, ev.col + 1u);
        out += ';';
        put_num(out, ev.row + 1u);
        out += 'M';
        return true;
    }
    const unsigned x = ev.col + 1u + 32u, y = ev.row + 1u + 32u, b = static_cast<unsigned>(code) + 32u;
    if (f == MouseFormat::Utf8) {
        if (x > 2047 || y > 2047 || b > 2047) return false;  // two UTF-8 bytes at most
        out += "\x1b[M";
        append_utf8(out, static_cast<char32_t>(b));
        append_utf8(out, static_cast<char32_t>(x));
        append_utf8(out, static_cast<char32_t>(y));
        return true;
    }
    if (x > 255 || y > 255 || b > 255) return false;  // coordinate <= 223: no report beyond
    out += "\x1b[M";
    out += static_cast<char>(b);
    out += static_cast<char>(x);
    out += static_cast<char>(y);
    return true;
}

void InputEncoder::encode_paste(std::u8string_view text, const InputModes& modes, std::string& out) {
    if (modes.bracketed_paste) out += "\x1b[200~";
    const auto* p = reinterpret_cast<const uint8_t*>(text.data());
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n;) {
        const uint8_t b0 = p[i];
        if (b0 < 0x80) {
            ++i;
            if (b0 == '\r') {
                if (i < n && p[i] == '\n') ++i;  // CR LF -> CR
                out += '\r';
            } else if (b0 == '\n') {
                out += '\r';
            } else if (b0 == '\t' || (b0 >= 0x20 && b0 != 0x7F)) {
                out += static_cast<char>(b0);
            }  // other C0 and DEL: dropped (ESC included, so the end marker cannot be forged)
            continue;
        }
        // Multi-byte sequence: decode strictly; invalid bytes become U+FFFD.
        std::size_t len = 0;
        char32_t cp = 0, min = 0;
        if ((b0 & 0xE0) == 0xC0) {
            len = 2;
            cp = b0 & 0x1Fu;
            min = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
            cp = b0 & 0x0Fu;
            min = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
            cp = b0 & 0x07u;
            min = 0x10000;
        }
        bool ok = len != 0 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; ++k) {
            const uint8_t b = p[i + k];
            if ((b & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (b & 0x3Fu);
        }
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (!ok) {
            append_utf8(out, U'�');
            ++i;
            continue;
        }
        i += len;
        if (cp >= 0x80 && cp <= 0x9F) continue;  // C1 controls (8-bit CSI could end the paste too)
        append_utf8(out, cp);
    }
    if (modes.bracketed_paste) out += "\x1b[201~";
}

void InputEncoder::encode_focus(bool focused, const InputModes& modes, std::string& out) {
    if (!modes.focus_events) return;
    out += focused ? "\x1b[I" : "\x1b[O";
}

}  // namespace kt::vt
