#include "pty/agent/input_decoder.h"

#include <array>

namespace kt::pty::agent {
namespace {

void press(std::vector<InputEvent>& out, uint16_t vkey, char16_t ch, uint32_t mods) {
    InputEvent e;
    e.key.vk = vkey;
    e.key.ch = ch;
    e.key.mods = mods;
    e.key.down = true;
    out.push_back(e);
    e.key.down = false;
    out.push_back(e);
}

// xterm modifier parameter (1 + bits: 1 Shift, 2 Alt, 4 Ctrl) -> control key state.
uint32_t mods_from_param(int m) {
    if (m < 2) return 0;
    const int bits = m - 1;
    uint32_t s = 0;
    if (bits & 1) s |= kShift;
    if (bits & 2) s |= kLeftAlt;
    if (bits & 4) s |= kLeftCtrl;
    return s;
}

void text_char(std::vector<InputEvent>& out, char32_t cp, uint32_t mods) {
    if (cp >= 0x10000) {
        // Two VK_PACKET events carrying the surrogate halves.
        const char32_t v = cp - 0x10000;
        press(out, vk::Packet, static_cast<char16_t>(0xD800 + (v >> 10)), mods);
        press(out, vk::Packet, static_cast<char16_t>(0xDC00 + (v & 0x3FF)), mods);
    } else {
        press(out, 0, static_cast<char16_t>(cp), mods);
    }
}

void control_char(std::vector<InputEvent>& out, unsigned char c, uint32_t extra) {
    switch (c) {
        case 0x0D:
        case 0x0A: press(out, vk::Return, u'\r', extra); return;
        case 0x09: press(out, vk::Tab, u'\t', extra); return;
        case 0x7F: press(out, vk::Back, 0x08, extra); return;
        case 0x08: press(out, vk::Back, 0x08, extra | kLeftCtrl); return;
        case 0x00: press(out, vk::Space, 0, extra | kLeftCtrl); return;
        case 0x1C: press(out, 0xDC, c, extra | kLeftCtrl); return;           // Ctrl+backslash
        case 0x1D: press(out, 0xDD, c, extra | kLeftCtrl); return;           // Ctrl+]
        case 0x1E: press(out, '6', c, extra | kLeftCtrl | kShift); return;   // Ctrl+^
        case 0x1F: press(out, 0xBD, c, extra | kLeftCtrl | kShift); return;  // Ctrl+_
        default: break;
    }
    if (c >= 0x01 && c <= 0x1A) press(out, static_cast<uint16_t>('A' + c - 1), c, extra | kLeftCtrl);
}

// Decodes one UTF-8 sequence at s[i]. Returns its length, 0 when incomplete, and U+FFFD for
// invalid bytes (length 1).
std::size_t utf8_at(std::string_view s, std::size_t i, char32_t& cp) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    std::size_t n = 1;
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        cp = b0 & 0x07u;
    } else {
        cp = 0xFFFD;
        return 1;
    }
    if (s.size() - i < n) {
        for (std::size_t k = i + 1; k < s.size(); ++k) {
            if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80) {
                cp = 0xFFFD;
                return 1;
            }
        }
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    return n;
}

bool cursor_key(char f, uint16_t& vkey) {
    switch (f) {
        case 'A': vkey = vk::Up; return true;
        case 'B': vkey = vk::Down; return true;
        case 'C': vkey = vk::Right; return true;
        case 'D': vkey = vk::Left; return true;
        case 'H': vkey = vk::Home; return true;
        case 'F': vkey = vk::End; return true;
        case 'P': vkey = vk::F1; return true;
        case 'Q': vkey = vk::F1 + 1; return true;
        case 'R': vkey = vk::F1 + 2; return true;
        case 'S': vkey = vk::F1 + 3; return true;
        default: return false;
    }
}

uint32_t enhanced_for(uint16_t vkey) {
    return (vkey >= vk::Prior && vkey <= vk::Down) || vkey == vk::Insert || vkey == vk::Delete ? kEnhanced : 0;
}

bool tilde_key(int n, uint16_t& vkey) {
    switch (n) {
        case 1: case 7: vkey = vk::Home; return true;
        case 2: vkey = vk::Insert; return true;
        case 3: vkey = vk::Delete; return true;
        case 4: case 8: vkey = vk::End; return true;
        case 5: vkey = vk::Prior; return true;
        case 6: vkey = vk::Next; return true;
        case 11: case 12: case 13: case 14: case 15: vkey = static_cast<uint16_t>(vk::F1 + n - 11); return true;
        case 17: case 18: case 19: case 20: case 21: vkey = static_cast<uint16_t>(vk::F1 + 5 + n - 17); return true;
        case 23: case 24: vkey = static_cast<uint16_t>(vk::F1 + 10 + n - 23); return true;
        default: return false;
    }
}

void csi(std::vector<InputEvent>& out, std::string_view params, char final) {
    char marker = 0;
    if (!params.empty() && params[0] >= '<' && params[0] <= '?') {
        marker = params[0];
        params.remove_prefix(1);
    }
    std::array<int, 8> p{};
    std::array<bool, 8> given{};
    std::size_t np = 0;
    for (std::size_t i = 0; i <= params.size() && np < p.size(); ++i) {
        if (i == params.size() || params[i] == ';') {
            ++np;
            continue;
        }
        const char c = params[i];
        if (c >= '0' && c <= '9') {
            p[np] = p[np] * 10 + (c - '0');
            given[np] = true;
            if (p[np] > 0xFFFFF) p[np] = 0xFFFFF;
        }
    }
    auto arg = [&](std::size_t i, int def) { return i < np && given[i] ? p[i] : def; };

    if (marker == '<') return;  // SGR mouse: not forwarded yet
    if (final == '_' && marker == 0) {
        // win32-input-mode: Vk;Sc;Uc;Kd;Cs;Rc
        InputEvent e;
        e.key.vk = static_cast<uint16_t>(arg(0, 0));
        e.key.scan = static_cast<uint16_t>(arg(1, 0));
        e.key.ch = static_cast<char16_t>(arg(2, 0));
        e.key.down = arg(3, 0) != 0;
        e.key.mods = static_cast<uint32_t>(arg(4, 0));
        e.key.repeat = static_cast<uint16_t>(arg(5, 1));
        if (e.key.vk == 0 && e.key.ch == 0) return;
        if (e.key.vk == 0) e.key.vk = vk::Packet;
        out.push_back(e);
        return;
    }
    if (marker != 0) return;
    if ((final == 'I' || final == 'O') && np <= 1 && !given[0]) {
        InputEvent e;
        e.kind = InputEvent::Kind::Focus;
        e.focus = final == 'I';
        out.push_back(e);
        return;
    }
    if (final == 'Z') {
        press(out, vk::Tab, u'\t', kShift);
        return;
    }
    uint16_t vkey = 0;
    if (final == '~') {
        const int n = arg(0, 0);
        if (n == 200 || n == 201) return;  // bracketed paste markers
        if (!tilde_key(n, vkey)) return;
        press(out, vkey, 0, mods_from_param(arg(1, 1)) | enhanced_for(vkey));
        return;
    }
    if (cursor_key(final, vkey)) press(out, vkey, 0, mods_from_param(arg(1, 1)) | enhanced_for(vkey));
}

}  // namespace

void InputDecoder::feed(std::string_view bytes, std::vector<InputEvent>& out) {
    buf_.append(bytes);
    parse(out, false);
}

void InputDecoder::flush(std::vector<InputEvent>& out) { parse(out, true); }

void InputDecoder::parse(std::vector<InputEvent>& out, bool final) {
    const std::string_view s = buf_;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B) {
            if (i + 1 >= s.size()) {
                if (!final) break;
                press(out, vk::Escape, 0x1B, 0);
                ++i;
                continue;
            }
            const char n = s[i + 1];
            if (n == '[') {
                std::size_t j = i + 2;
                while (j < s.size() && static_cast<unsigned char>(s[j]) >= 0x20 &&
                       static_cast<unsigned char>(s[j]) <= 0x3F) {
                    ++j;
                }
                if (j >= s.size()) {
                    if (!final) break;
                    press(out, vk::Escape, 0x1B, 0);  // incomplete: take the bytes as typed
                    ++i;
                    continue;
                }
                const char f = s[j];
                if (static_cast<unsigned char>(f) >= 0x40 && static_cast<unsigned char>(f) <= 0x7E) {
                    csi(out, s.substr(i + 2, j - i - 2), f);
                    i = j + 1;
                } else {
                    press(out, vk::Escape, 0x1B, 0);
                    ++i;
                }
                continue;
            }
            if (n == 'O') {
                if (i + 2 >= s.size()) {
                    if (!final) break;
                    press(out, 0, u'O', kLeftAlt);
                    i += 2;
                    continue;
                }
                uint16_t vkey = 0;
                if (cursor_key(s[i + 2], vkey)) press(out, vkey, 0, enhanced_for(vkey));
                i += 3;
                continue;
            }
            if (n == 0x1B) {
                press(out, vk::Escape, 0x1B, 0);
                ++i;
                continue;
            }
            const auto nb = static_cast<unsigned char>(n);
            if (nb < 0x20 || nb == 0x7F) {
                control_char(out, nb, kLeftAlt);
                i += 2;
                continue;
            }
            char32_t cp = 0;
            const std::size_t len = utf8_at(s, i + 1, cp);
            if (len == 0) {
                if (!final) break;
                press(out, vk::Escape, 0x1B, 0);
                ++i;
                continue;
            }
            text_char(out, cp, kLeftAlt);
            i += 1 + len;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            control_char(out, c, 0);
            ++i;
            continue;
        }
        char32_t cp = 0;
        const std::size_t len = utf8_at(s, i, cp);
        if (len == 0) {
            if (!final) break;
            text_char(out, 0xFFFD, 0);
            i = s.size();
            continue;
        }
        text_char(out, cp, 0);
        i += len;
    }
    buf_.erase(0, i);
}

}  // namespace kt::pty::agent
