#include "commands/layout.h"

namespace kt::commands {

using PKey = platform::Key;   // physical (HID) key of the platform layer
using CKey = commands::Key;

namespace {

bool is_modifier_hid(PKey k) {
    const auto v = static_cast<std::uint16_t>(k);
    return (v >= 224 && v <= 231) || k == PKey::caps_lock || k == PKey::num_lock;
}

NamedKey named_of(PKey k) {
    const auto v = static_cast<std::uint16_t>(k);
    switch (k) {
        case PKey::enter:
        case PKey::kp_enter: return NamedKey::Enter;   // one binding covers both Return keys
        case PKey::escape: return NamedKey::Escape;
        case PKey::backspace: return NamedKey::Backspace;
        case PKey::tab: return NamedKey::Tab;
        case PKey::space: return NamedKey::Space;
        case PKey::insert: return NamedKey::Insert;
        case PKey::home: return NamedKey::Home;
        case PKey::page_up: return NamedKey::PageUp;
        case PKey::delete_: return NamedKey::Delete;
        case PKey::end: return NamedKey::End;
        case PKey::page_down: return NamedKey::PageDown;
        case PKey::right: return NamedKey::Right;
        case PKey::left: return NamedKey::Left;
        case PKey::down: return NamedKey::Down;
        case PKey::up: return NamedKey::Up;
        case PKey::kp_divide: return NamedKey::KpDivide;
        case PKey::kp_multiply: return NamedKey::KpMultiply;
        case PKey::kp_subtract: return NamedKey::KpSubtract;
        case PKey::kp_add: return NamedKey::KpAdd;
        case PKey::kp_decimal: return NamedKey::KpDecimal;
        case PKey::kp_equal: return NamedKey::KpEqual;
        case PKey::kp_0: return NamedKey::Kp0;
        case PKey::context_menu: return NamedKey::Menu;
        case PKey::print_screen: return NamedKey::PrintScreen;
        case PKey::pause: return NamedKey::Pause;
        case PKey::scroll_lock: return NamedKey::ScrollLock;
        default: break;
    }
    if (v >= static_cast<std::uint16_t>(PKey::f1) && v <= static_cast<std::uint16_t>(PKey::f12))
        return static_cast<NamedKey>(static_cast<std::uint16_t>(NamedKey::F1) + (v - static_cast<std::uint16_t>(PKey::f1)));
    if (v >= static_cast<std::uint16_t>(PKey::f13) && v <= static_cast<std::uint16_t>(PKey::f24))
        return static_cast<NamedKey>(static_cast<std::uint16_t>(NamedKey::F13) + (v - static_cast<std::uint16_t>(PKey::f13)));
    if (v >= static_cast<std::uint16_t>(PKey::kp_1) && v <= static_cast<std::uint16_t>(PKey::kp_9))
        return static_cast<NamedKey>(static_cast<std::uint16_t>(NamedKey::Kp1) + (v - static_cast<std::uint16_t>(PKey::kp_1)));
    return NamedKey::None;
}

bool printable_ascii(char32_t c) { return c > 0x20 && c < 0x7F; }

// Windows virtual-key codes for non-Latin layouts: VK_A..VK_Z and the digits sit at the US
// positions; the OEM codes name the US punctuation keys.
CKey from_windows_vk(std::uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') return CKey::character(vk);
    if (vk >= '0' && vk <= '9') return CKey::character(vk);
    switch (vk) {
        case 0xBA: return CKey::character(';');    // VK_OEM_1
        case 0xBB: return CKey::character('=');    // VK_OEM_PLUS
        case 0xBC: return CKey::character(',');    // VK_OEM_COMMA
        case 0xBD: return CKey::character('-');    // VK_OEM_MINUS
        case 0xBE: return CKey::character('.');    // VK_OEM_PERIOD
        case 0xBF: return CKey::character('/');    // VK_OEM_2
        case 0xC0: return CKey::character('`');    // VK_OEM_3
        case 0xDB: return CKey::character('[');    // VK_OEM_4
        case 0xDC: return CKey::character('\\');   // VK_OEM_5
        case 0xDD: return CKey::character(']');    // VK_OEM_6
        case 0xDE: return CKey::character('\'');   // VK_OEM_7
        default: return {};
    }
}

bool is_digit(const CKey& k) { return k.kind == CKey::Kind::Char && k.ch >= '0' && k.ch <= '9'; }

}  // namespace

std::uint8_t chord_mods(std::uint16_t m) {
    std::uint8_t out = 0;
    if (m & platform::mod_ctrl) out |= Ctrl;
    if (m & platform::mod_shift) out |= Shift;
    if (m & platform::mod_alt) out |= Alt;
    if (m & platform::mod_super) out |= Super;
    if (m & platform::mod_altgr) out |= Ctrl | Alt;   // Windows convention: AltGr is Ctrl+Alt
    return out;
}

Key us_key_of(platform::Key k) {
    const auto v = static_cast<std::uint16_t>(k);
    if (v >= static_cast<std::uint16_t>(PKey::a) && v <= static_cast<std::uint16_t>(PKey::z))
        return CKey::character(U'a' + (v - static_cast<std::uint16_t>(PKey::a)));
    if (v >= static_cast<std::uint16_t>(PKey::d1) && v <= static_cast<std::uint16_t>(PKey::d9))
        return CKey::character(U'1' + (v - static_cast<std::uint16_t>(PKey::d1)));
    switch (k) {
        case PKey::d0: return CKey::character('0');
        case PKey::minus: return CKey::character('-');
        case PKey::equal: return CKey::character('=');
        case PKey::bracket_left: return CKey::character('[');
        case PKey::bracket_right: return CKey::character(']');
        case PKey::backslash:
        case PKey::non_us_hash: return CKey::character('\\');
        case PKey::semicolon: return CKey::character(';');
        case PKey::quote: return CKey::character('\'');
        case PKey::grave: return CKey::character('`');
        case PKey::comma: return CKey::character(',');
        case PKey::period: return CKey::character('.');
        case PKey::slash: return CKey::character('/');
        default: return {};
    }
}

EventChords map_event(const platform::KeyEvent& e, Os os) {
    EventChords out;
    if (is_modifier_hid(e.key)) {
        out.modifier_key = true;
        return out;
    }
    const std::uint8_t mods = chord_mods(e.mods);
    if (e.key != PKey::unknown) out.physical = Chord{mods, CKey::phys(static_cast<std::uint16_t>(e.key))};

    // 1. Named keys by the key itself.
    if (const NamedKey n = named_of(e.key); n != NamedKey::None) {
        out.logical = Chord{mods, CKey::named_key(n)};
        return out;
    }
    const CKey us = us_key_of(e.key);
    // 2./3. What the key types without modifiers (the platform's base character).
    if (printable_ascii(e.logical)) {
        out.logical = Chord{mods, CKey::character(e.logical)};
        // AZERTY and friends: the digit row types symbols unshifted; tab.select and font
        // reset still find the digit at that position.
        if (is_digit(us) && !is_digit(out.logical.key)) out.fallback = Chord{mods, us};
        return out;
    }
    // 4. A non-ASCII character (Cyrillic, Greek, Hebrew ...) or none: the Latin key at the
    // same position. Windows: the virtual-key code; macOS already gave the ASCII-capable
    // layout's character in `logical`; Linux: the first group was tried by the platform.
    CKey latin;
    if (os == Os::Windows) latin = from_windows_vk(e.native_code);
    if (latin.empty()) latin = us;
    if (!latin.empty()) out.logical = Chord{mods, latin};
    return out;
}

bool altgr_types_text(const platform::KeyEvent& e, Os os) {
    if (os == Os::MacOS) return false;   // Option is never AltGr; option_as_alt is the view's business
    const bool altgr = (e.mods & platform::mod_altgr) != 0;
    const bool ctrl_alt = (e.mods & platform::mod_ctrl) && (e.mods & platform::mod_alt);
    if (!altgr && !ctrl_alt) return false;
    if (e.text.empty()) return false;
    const auto c = static_cast<unsigned char>(e.text[0]);
    return c >= 0x20 && c != 0x7F;   // the platform already filters C0 and DEL
}

}  // namespace kt::commands
