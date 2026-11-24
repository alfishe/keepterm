#include "platform_gui/keyboard.h"

#include <array>
#include <utility>

namespace kt::platform {

namespace {

struct KeyNameEntry {
    Key key;
    std::string_view name;
};

constexpr KeyNameEntry kKeyNames[] = {
    {Key::a, "a"}, {Key::b, "b"}, {Key::c, "c"}, {Key::d, "d"}, {Key::e, "e"}, {Key::f, "f"},
    {Key::g, "g"}, {Key::h, "h"}, {Key::i, "i"}, {Key::j, "j"}, {Key::k, "k"}, {Key::l, "l"},
    {Key::m, "m"}, {Key::n, "n"}, {Key::o, "o"}, {Key::p, "p"}, {Key::q, "q"}, {Key::r, "r"},
    {Key::s, "s"}, {Key::t, "t"}, {Key::u, "u"}, {Key::v, "v"}, {Key::w, "w"}, {Key::x, "x"},
    {Key::y, "y"}, {Key::z, "z"},
    {Key::d1, "1"}, {Key::d2, "2"}, {Key::d3, "3"}, {Key::d4, "4"}, {Key::d5, "5"},
    {Key::d6, "6"}, {Key::d7, "7"}, {Key::d8, "8"}, {Key::d9, "9"}, {Key::d0, "0"},
    {Key::enter, "enter"}, {Key::escape, "escape"}, {Key::backspace, "backspace"}, {Key::tab, "tab"},
    {Key::space, "space"}, {Key::minus, "minus"}, {Key::equal, "equal"},
    {Key::bracket_left, "bracket_left"}, {Key::bracket_right, "bracket_right"},
    {Key::backslash, "backslash"}, {Key::non_us_hash, "non_us_hash"}, {Key::semicolon, "semicolon"},
    {Key::quote, "quote"}, {Key::grave, "grave"}, {Key::comma, "comma"}, {Key::period, "period"},
    {Key::slash, "slash"}, {Key::caps_lock, "caps_lock"},
    {Key::f1, "f1"}, {Key::f2, "f2"}, {Key::f3, "f3"}, {Key::f4, "f4"}, {Key::f5, "f5"}, {Key::f6, "f6"},
    {Key::f7, "f7"}, {Key::f8, "f8"}, {Key::f9, "f9"}, {Key::f10, "f10"}, {Key::f11, "f11"}, {Key::f12, "f12"},
    {Key::print_screen, "print_screen"}, {Key::scroll_lock, "scroll_lock"}, {Key::pause, "pause"},
    {Key::insert, "insert"}, {Key::home, "home"}, {Key::page_up, "page_up"}, {Key::delete_, "delete"},
    {Key::end, "end"}, {Key::page_down, "page_down"}, {Key::right, "right"}, {Key::left, "left"},
    {Key::down, "down"}, {Key::up, "up"}, {Key::num_lock, "num_lock"}, {Key::kp_divide, "kp_divide"},
    {Key::kp_multiply, "kp_multiply"}, {Key::kp_subtract, "kp_subtract"}, {Key::kp_add, "kp_add"},
    {Key::kp_enter, "kp_enter"}, {Key::kp_1, "kp_1"}, {Key::kp_2, "kp_2"}, {Key::kp_3, "kp_3"},
    {Key::kp_4, "kp_4"}, {Key::kp_5, "kp_5"}, {Key::kp_6, "kp_6"}, {Key::kp_7, "kp_7"}, {Key::kp_8, "kp_8"},
    {Key::kp_9, "kp_9"}, {Key::kp_0, "kp_0"}, {Key::kp_decimal, "kp_decimal"},
    {Key::non_us_backslash, "non_us_backslash"}, {Key::context_menu, "context_menu"}, {Key::power, "power"},
    {Key::kp_equal, "kp_equal"},
    {Key::f13, "f13"}, {Key::f14, "f14"}, {Key::f15, "f15"}, {Key::f16, "f16"}, {Key::f17, "f17"},
    {Key::f18, "f18"}, {Key::f19, "f19"}, {Key::f20, "f20"}, {Key::f21, "f21"}, {Key::f22, "f22"},
    {Key::f23, "f23"}, {Key::f24, "f24"}, {Key::help, "help"},
    {Key::mute, "mute"}, {Key::volume_up, "volume_up"}, {Key::volume_down, "volume_down"},
    {Key::kp_comma, "kp_comma"}, {Key::intl_ro, "intl_ro"}, {Key::kana_mode, "kana_mode"},
    {Key::intl_yen, "intl_yen"}, {Key::convert, "convert"}, {Key::non_convert, "non_convert"},
    {Key::lang1, "lang1"}, {Key::lang2, "lang2"},
    {Key::left_ctrl, "left_ctrl"}, {Key::left_shift, "left_shift"}, {Key::left_alt, "left_alt"},
    {Key::left_super, "left_super"}, {Key::right_ctrl, "right_ctrl"}, {Key::right_shift, "right_shift"},
    {Key::right_alt, "right_alt"}, {Key::right_super, "right_super"},
};

}  // namespace

std::string_view key_name(Key k) {
    for (const auto& e : kKeyNames) {
        if (e.key == k) return e.name;
    }
    return "unknown";
}

bool is_modifier_key(Key k) {
    return (k >= Key::left_ctrl && k <= Key::right_super) || k == Key::caps_lock || k == Key::num_lock;
}

std::string mods_to_string(uint16_t mods) {
    std::string s;
    auto add = [&](uint16_t bit, const char* name) {
        if (!(mods & bit)) return;
        if (!s.empty()) s += '+';
        s += name;
    };
    add(mod_ctrl, "ctrl");
    add(mod_alt, "alt");
    add(mod_altgr, "altgr");
    add(mod_shift, "shift");
    add(mod_super, "super");
    add(mod_caps, "caps");
    add(mod_num, "num");
    return s;
}

}  // namespace kt::platform

namespace kt::platform::detail {

namespace {

using K = Key;

// evdev codes 0..127 (linux/input-event-codes.h).
constexpr std::array<Key, 128> kEvdev = [] {
    std::array<Key, 128> t{};
    t[1] = K::escape;
    t[2] = K::d1; t[3] = K::d2; t[4] = K::d3; t[5] = K::d4; t[6] = K::d5;
    t[7] = K::d6; t[8] = K::d7; t[9] = K::d8; t[10] = K::d9; t[11] = K::d0;
    t[12] = K::minus; t[13] = K::equal; t[14] = K::backspace; t[15] = K::tab;
    t[16] = K::q; t[17] = K::w; t[18] = K::e; t[19] = K::r; t[20] = K::t; t[21] = K::y;
    t[22] = K::u; t[23] = K::i; t[24] = K::o; t[25] = K::p;
    t[26] = K::bracket_left; t[27] = K::bracket_right; t[28] = K::enter; t[29] = K::left_ctrl;
    t[30] = K::a; t[31] = K::s; t[32] = K::d; t[33] = K::f; t[34] = K::g; t[35] = K::h;
    t[36] = K::j; t[37] = K::k; t[38] = K::l;
    t[39] = K::semicolon; t[40] = K::quote; t[41] = K::grave; t[42] = K::left_shift; t[43] = K::backslash;
    t[44] = K::z; t[45] = K::x; t[46] = K::c; t[47] = K::v; t[48] = K::b; t[49] = K::n; t[50] = K::m;
    t[51] = K::comma; t[52] = K::period; t[53] = K::slash; t[54] = K::right_shift;
    t[55] = K::kp_multiply; t[56] = K::left_alt; t[57] = K::space; t[58] = K::caps_lock;
    t[59] = K::f1; t[60] = K::f2; t[61] = K::f3; t[62] = K::f4; t[63] = K::f5;
    t[64] = K::f6; t[65] = K::f7; t[66] = K::f8; t[67] = K::f9; t[68] = K::f10;
    t[69] = K::num_lock; t[70] = K::scroll_lock;
    t[71] = K::kp_7; t[72] = K::kp_8; t[73] = K::kp_9; t[74] = K::kp_subtract;
    t[75] = K::kp_4; t[76] = K::kp_5; t[77] = K::kp_6; t[78] = K::kp_add;
    t[79] = K::kp_1; t[80] = K::kp_2; t[81] = K::kp_3; t[82] = K::kp_0; t[83] = K::kp_decimal;
    t[85] = K::lang1;            // KEY_ZENKAKUHANKAKU (no HID equivalent; closest)
    t[86] = K::non_us_backslash; // KEY_102ND
    t[87] = K::f11; t[88] = K::f12;
    t[89] = K::intl_ro;          // KEY_RO
    t[90] = K::lang1;            // KEY_KATAKANA
    t[91] = K::lang2;            // KEY_HIRAGANA
    t[92] = K::convert;          // KEY_HENKAN
    t[93] = K::kana_mode;        // KEY_KATAKANAHIRAGANA
    t[94] = K::non_convert;      // KEY_MUHENKAN
    t[95] = K::kp_comma;         // KEY_KPJPCOMMA
    t[96] = K::kp_enter; t[97] = K::right_ctrl; t[98] = K::kp_divide; t[99] = K::print_screen;
    t[100] = K::right_alt;
    t[102] = K::home; t[103] = K::up; t[104] = K::page_up; t[105] = K::left; t[106] = K::right;
    t[107] = K::end; t[108] = K::down; t[109] = K::page_down; t[110] = K::insert; t[111] = K::delete_;
    t[113] = K::mute; t[114] = K::volume_down; t[115] = K::volume_up; t[116] = K::power;
    t[117] = K::kp_equal; t[119] = K::pause; t[121] = K::kp_comma;
    t[122] = K::lang1;           // KEY_HANGEUL
    t[123] = K::lang2;           // KEY_HANJA
    t[124] = K::intl_yen;
    t[125] = K::left_super; t[126] = K::right_super; t[127] = K::context_menu;   // KEY_COMPOSE
    return t;
}();

// Windows set-1 scan codes without the E0 prefix, 0x00..0x7F.
constexpr std::array<Key, 128> kWinScan = [] {
    std::array<Key, 128> t{};
    t[0x01] = K::escape;
    t[0x02] = K::d1; t[0x03] = K::d2; t[0x04] = K::d3; t[0x05] = K::d4; t[0x06] = K::d5;
    t[0x07] = K::d6; t[0x08] = K::d7; t[0x09] = K::d8; t[0x0A] = K::d9; t[0x0B] = K::d0;
    t[0x0C] = K::minus; t[0x0D] = K::equal; t[0x0E] = K::backspace; t[0x0F] = K::tab;
    t[0x10] = K::q; t[0x11] = K::w; t[0x12] = K::e; t[0x13] = K::r; t[0x14] = K::t; t[0x15] = K::y;
    t[0x16] = K::u; t[0x17] = K::i; t[0x18] = K::o; t[0x19] = K::p;
    t[0x1A] = K::bracket_left; t[0x1B] = K::bracket_right; t[0x1C] = K::enter; t[0x1D] = K::left_ctrl;
    t[0x1E] = K::a; t[0x1F] = K::s; t[0x20] = K::d; t[0x21] = K::f; t[0x22] = K::g; t[0x23] = K::h;
    t[0x24] = K::j; t[0x25] = K::k; t[0x26] = K::l;
    t[0x27] = K::semicolon; t[0x28] = K::quote; t[0x29] = K::grave; t[0x2A] = K::left_shift;
    t[0x2B] = K::backslash;
    t[0x2C] = K::z; t[0x2D] = K::x; t[0x2E] = K::c; t[0x2F] = K::v; t[0x30] = K::b; t[0x31] = K::n;
    t[0x32] = K::m; t[0x33] = K::comma; t[0x34] = K::period; t[0x35] = K::slash; t[0x36] = K::right_shift;
    t[0x37] = K::kp_multiply; t[0x38] = K::left_alt; t[0x39] = K::space; t[0x3A] = K::caps_lock;
    t[0x3B] = K::f1; t[0x3C] = K::f2; t[0x3D] = K::f3; t[0x3E] = K::f4; t[0x3F] = K::f5;
    t[0x40] = K::f6; t[0x41] = K::f7; t[0x42] = K::f8; t[0x43] = K::f9; t[0x44] = K::f10;
    t[0x45] = K::pause;          // non-extended 0x45 is Pause (E1 prefix); NumLock is E0 0x45
    t[0x46] = K::scroll_lock;
    t[0x47] = K::kp_7; t[0x48] = K::kp_8; t[0x49] = K::kp_9; t[0x4A] = K::kp_subtract;
    t[0x4B] = K::kp_4; t[0x4C] = K::kp_5; t[0x4D] = K::kp_6; t[0x4E] = K::kp_add;
    t[0x4F] = K::kp_1; t[0x50] = K::kp_2; t[0x51] = K::kp_3; t[0x52] = K::kp_0; t[0x53] = K::kp_decimal;
    t[0x54] = K::print_screen;   // Alt+PrintScreen (SysRq)
    t[0x56] = K::non_us_backslash; t[0x57] = K::f11; t[0x58] = K::f12; t[0x59] = K::kp_equal;
    t[0x64] = K::f13; t[0x65] = K::f14; t[0x66] = K::f15; t[0x67] = K::f16; t[0x68] = K::f17;
    t[0x69] = K::f18; t[0x6A] = K::f19; t[0x6B] = K::f20; t[0x6C] = K::f21; t[0x6D] = K::f22;
    t[0x6E] = K::f23;
    t[0x70] = K::kana_mode; t[0x73] = K::intl_ro; t[0x76] = K::f24; t[0x79] = K::convert;
    t[0x7B] = K::non_convert; t[0x7D] = K::intl_yen; t[0x7E] = K::kp_comma;
    return t;
}();

// macOS kVK_* codes 0x00..0x7F (HIToolbox/Events.h).
constexpr std::array<Key, 128> kMac = [] {
    std::array<Key, 128> t{};
    t[0x00] = K::a; t[0x01] = K::s; t[0x02] = K::d; t[0x03] = K::f; t[0x04] = K::h; t[0x05] = K::g;
    t[0x06] = K::z; t[0x07] = K::x; t[0x08] = K::c; t[0x09] = K::v;
    t[0x0A] = K::non_us_backslash;   // kVK_ISO_Section
    t[0x0B] = K::b; t[0x0C] = K::q; t[0x0D] = K::w; t[0x0E] = K::e; t[0x0F] = K::r;
    t[0x10] = K::y; t[0x11] = K::t; t[0x12] = K::d1; t[0x13] = K::d2; t[0x14] = K::d3; t[0x15] = K::d4;
    t[0x16] = K::d6; t[0x17] = K::d5; t[0x18] = K::equal; t[0x19] = K::d9; t[0x1A] = K::d7;
    t[0x1B] = K::minus; t[0x1C] = K::d8; t[0x1D] = K::d0; t[0x1E] = K::bracket_right; t[0x1F] = K::o;
    t[0x20] = K::u; t[0x21] = K::bracket_left; t[0x22] = K::i; t[0x23] = K::p; t[0x24] = K::enter;
    t[0x25] = K::l; t[0x26] = K::j; t[0x27] = K::quote; t[0x28] = K::k; t[0x29] = K::semicolon;
    t[0x2A] = K::backslash; t[0x2B] = K::comma; t[0x2C] = K::slash; t[0x2D] = K::n; t[0x2E] = K::m;
    t[0x2F] = K::period; t[0x30] = K::tab; t[0x31] = K::space; t[0x32] = K::grave;
    t[0x33] = K::backspace;          // kVK_Delete
    t[0x35] = K::escape;
    t[0x36] = K::right_super; t[0x37] = K::left_super; t[0x38] = K::left_shift; t[0x39] = K::caps_lock;
    t[0x3A] = K::left_alt; t[0x3B] = K::left_ctrl; t[0x3C] = K::right_shift; t[0x3D] = K::right_alt;
    t[0x3E] = K::right_ctrl;
    t[0x40] = K::f17; t[0x41] = K::kp_decimal; t[0x43] = K::kp_multiply; t[0x45] = K::kp_add;
    t[0x47] = K::num_lock;           // kVK_ANSI_KeypadClear
    t[0x48] = K::volume_up; t[0x49] = K::volume_down; t[0x4A] = K::mute;
    t[0x4B] = K::kp_divide; t[0x4C] = K::kp_enter; t[0x4E] = K::kp_subtract; t[0x4F] = K::f18;
    t[0x50] = K::f19; t[0x51] = K::kp_equal;
    t[0x52] = K::kp_0; t[0x53] = K::kp_1; t[0x54] = K::kp_2; t[0x55] = K::kp_3; t[0x56] = K::kp_4;
    t[0x57] = K::kp_5; t[0x58] = K::kp_6; t[0x59] = K::kp_7; t[0x5A] = K::f20; t[0x5B] = K::kp_8;
    t[0x5C] = K::kp_9; t[0x5D] = K::intl_yen; t[0x5E] = K::intl_ro; t[0x5F] = K::kp_comma;
    t[0x60] = K::f5; t[0x61] = K::f6; t[0x62] = K::f7; t[0x63] = K::f3; t[0x64] = K::f8; t[0x65] = K::f9;
    t[0x66] = K::lang2;              // kVK_JIS_Eisu
    t[0x67] = K::f11;
    t[0x68] = K::lang1;              // kVK_JIS_Kana
    t[0x69] = K::f13; t[0x6A] = K::f16; t[0x6B] = K::f14; t[0x6D] = K::f10;
    t[0x6E] = K::context_menu;       // kVK_ContextualMenu
    t[0x6F] = K::f12; t[0x71] = K::f15;
    t[0x72] = K::insert;             // kVK_Help: the Insert position on PC keyboards
    t[0x73] = K::home; t[0x74] = K::page_up; t[0x75] = K::delete_; t[0x76] = K::f4; t[0x77] = K::end;
    t[0x78] = K::f2; t[0x79] = K::page_down; t[0x7A] = K::f1; t[0x7B] = K::left; t[0x7C] = K::right;
    t[0x7D] = K::down; t[0x7E] = K::up;
    return t;
}();

}  // namespace

Key key_from_evdev(uint32_t code) {
    if (code < kEvdev.size()) return kEvdev[code];
    if (code >= 183 && code <= 194) return static_cast<Key>(static_cast<uint16_t>(Key::f13) + (code - 183));
    if (code == 138) return Key::help;
    return Key::unknown;
}

Key key_from_win_scancode(uint32_t sc, bool extended) {
    sc &= 0xFF;
    if (!extended) {
        if (sc < kWinScan.size()) return kWinScan[sc];
        if (sc == 0xF1) return Key::lang2;   // Hanja
        if (sc == 0xF2) return Key::lang1;   // Hangul
        return Key::unknown;
    }
    switch (sc) {
        case 0x1C: return Key::kp_enter;
        case 0x1D: return Key::right_ctrl;
        case 0x20: return Key::mute;
        case 0x2E: return Key::volume_down;
        case 0x30: return Key::volume_up;
        case 0x35: return Key::kp_divide;
        case 0x37: return Key::print_screen;
        case 0x38: return Key::right_alt;
        case 0x45: return Key::num_lock;
        case 0x46: return Key::pause;       // Ctrl+Break
        case 0x47: return Key::home;
        case 0x48: return Key::up;
        case 0x49: return Key::page_up;
        case 0x4B: return Key::left;
        case 0x4D: return Key::right;
        case 0x4F: return Key::end;
        case 0x50: return Key::down;
        case 0x51: return Key::page_down;
        case 0x52: return Key::insert;
        case 0x53: return Key::delete_;
        case 0x5B: return Key::left_super;
        case 0x5C: return Key::right_super;
        case 0x5D: return Key::context_menu;
        case 0x5E: return Key::power;
        default: return Key::unknown;
    }
}

Key key_from_mac_keycode(uint16_t keycode) { return keycode < kMac.size() ? kMac[keycode] : Key::unknown; }

uint16_t side_mod_of(Key k) {
    switch (k) {
        case Key::left_shift: return mod_lshift;
        case Key::right_shift: return mod_rshift;
        case Key::left_ctrl: return mod_lctrl;
        case Key::right_ctrl: return mod_rctrl;
        case Key::left_alt: return mod_lalt;
        case Key::right_alt: return mod_ralt;
        case Key::left_super: return mod_lsuper;
        case Key::right_super: return mod_rsuper;
        default: return 0;
    }
}

uint16_t update_side_mods(uint16_t sides, Key key, bool pressed) {
    const uint16_t bit = side_mod_of(key);
    if (!bit) return sides;
    return static_cast<uint16_t>(pressed ? (sides | bit) : (sides & ~bit));
}

uint16_t generic_from_sides(uint16_t sides) {
    uint16_t m = 0;
    if (sides & (mod_lshift | mod_rshift)) m |= mod_shift;
    if (sides & (mod_lctrl | mod_rctrl)) m |= mod_ctrl;
    if (sides & (mod_lalt | mod_ralt)) m |= mod_alt;
    if (sides & (mod_lsuper | mod_rsuper)) m |= mod_super;
    return m;
}

namespace {
// NSEventModifierFlag* and the NX_DEVICE* masks of IOKit/hidsystem/IOLLEvent.h.
constexpr uint64_t kMacCaps = 1u << 16, kMacShift = 1u << 17, kMacCtrl = 1u << 18, kMacOption = 1u << 19,
                   kMacCommand = 1u << 20;
constexpr uint64_t kDevLCtrl = 0x0001, kDevLShift = 0x0002, kDevRShift = 0x0004, kDevLCmd = 0x0008,
                   kDevRCmd = 0x0010, kDevLAlt = 0x0020, kDevRAlt = 0x0040, kDevRCtrl = 0x2000;
}  // namespace

uint16_t mods_from_mac_flags(uint64_t f) {
    uint16_t m = 0;
    if (f & kMacShift) m |= mod_shift;
    if (f & kMacCtrl) m |= mod_ctrl;
    if (f & kMacOption) m |= mod_alt;
    if (f & kMacCommand) m |= mod_super;
    if (f & kMacCaps) m |= mod_caps;
    if (f & kDevLShift) m |= mod_lshift;
    if (f & kDevRShift) m |= mod_rshift;
    if (f & kDevLCtrl) m |= mod_lctrl;
    if (f & kDevRCtrl) m |= mod_rctrl;
    if (f & kDevLAlt) m |= mod_lalt;
    if (f & kDevRAlt) m |= mod_ralt;
    if (f & kDevLCmd) m |= mod_lsuper;
    if (f & kDevRCmd) m |= mod_rsuper;
    // Synthetic events may carry only the generic bits: report the left side then.
    if ((m & mod_shift) && !(m & (mod_lshift | mod_rshift))) m |= mod_lshift;
    if ((m & mod_ctrl) && !(m & (mod_lctrl | mod_rctrl))) m |= mod_lctrl;
    if ((m & mod_alt) && !(m & (mod_lalt | mod_ralt))) m |= mod_lalt;
    if ((m & mod_super) && !(m & (mod_lsuper | mod_rsuper))) m |= mod_lsuper;
    return m;
}

MacOptionSides mac_option_sides(uint64_t f) {
    MacOptionSides s{(f & kDevLAlt) != 0, (f & kDevRAlt) != 0};
    if ((f & kMacOption) && !s.left && !s.right) s.left = true;
    return s;
}

uint16_t mods_from_x11_state(uint32_t state, uint16_t sides) {
    constexpr uint32_t kShift = 1, kLock = 2, kControl = 4, kMod1 = 8, kMod2 = 16, kMod4 = 64, kMod5 = 128;
    uint16_t m = 0;
    if (state & kShift) m |= mod_shift;
    if (state & kControl) m |= mod_ctrl;
    if (state & kMod1) m |= mod_alt;
    if (state & kMod4) m |= mod_super;
    if (state & kMod5) m |= mod_altgr;
    if (state & kLock) m |= mod_caps;
    if (state & kMod2) m |= mod_num;
    // Side bits only for modifiers that the state confirms (a missed release must not stick).
    if (m & mod_shift) m |= static_cast<uint16_t>(sides & (mod_lshift | mod_rshift));
    if (m & mod_ctrl) m |= static_cast<uint16_t>(sides & (mod_lctrl | mod_rctrl));
    if (m & mod_alt) m |= static_cast<uint16_t>(sides & (mod_lalt | mod_ralt));
    if (m & mod_super) m |= static_cast<uint16_t>(sides & (mod_lsuper | mod_rsuper));
    return m;
}

uint16_t mods_from_win(const WinKeyState& s) {
    uint16_t m = 0;
    const bool lctrl = s.lctrl && !s.altgr;   // the synthesized LCtrl of AltGr is not Ctrl
    const bool ralt = s.ralt && !s.altgr;
    if (s.lshift) m |= mod_lshift | mod_shift;
    if (s.rshift) m |= mod_rshift | mod_shift;
    if (lctrl) m |= mod_lctrl | mod_ctrl;
    if (s.rctrl) m |= mod_rctrl | mod_ctrl;
    if (s.lalt) m |= mod_lalt | mod_alt;
    if (ralt) m |= mod_ralt | mod_alt;
    if (s.lwin) m |= mod_lsuper | mod_super;
    if (s.rwin) m |= mod_rsuper | mod_super;
    if (s.altgr) m |= mod_altgr;
    if (s.caps) m |= mod_caps;
    if (s.num) m |= mod_num;
    return m;
}

bool is_altgr_lctrl(const RawKeyMsg& ctrl, const RawKeyMsg& next) {
    constexpr uint32_t kKeyDown = 0x100, kKeyUp = 0x101, kSysKeyDown = 0x104, kSysKeyUp = 0x105;
    constexpr uint32_t kVkControl = 0x11, kVkMenu = 0x12;
    if (ctrl.vk != kVkControl || ctrl.extended) return false;
    if (next.vk != kVkMenu || !next.extended || next.time != ctrl.time) return false;
    const bool ctrl_down = ctrl.message == kKeyDown || ctrl.message == kSysKeyDown;
    const bool ctrl_up = ctrl.message == kKeyUp || ctrl.message == kSysKeyUp;
    const bool next_down = next.message == kKeyDown || next.message == kSysKeyDown;
    const bool next_up = next.message == kKeyUp || next.message == kSysKeyUp;
    return (ctrl_down && next_down) || (ctrl_up && next_up);
}

bool is_text_char(char32_t c) {
    if (c < 0x20 || c == 0x7F) return false;
    if (c >= 0x80 && c < 0xA0) return false;
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    if (c >= 0xF700 && c <= 0xF8FF) return false;
    return c <= 0x10FFFF;
}

char32_t keysym_to_ucs(uint32_t ks) {
    if ((ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF)) return ks;
    if ((ks & 0xFF000000u) == 0x01000000u) {
        const char32_t c = ks & 0x00FFFFFFu;
        return c <= 0x10FFFF ? c : 0;
    }
    if (ks >= 0xFFB0 && ks <= 0xFFB9) return U'0' + (ks - 0xFFB0);   // XK_KP_0..XK_KP_9
    switch (ks) {
        case 0xFF80: return U' ';    // XK_KP_Space
        case 0xFFAA: return U'*';    // XK_KP_Multiply
        case 0xFFAB: return U'+';    // XK_KP_Add
        case 0xFFAC: return U',';    // XK_KP_Separator
        case 0xFFAD: return U'-';    // XK_KP_Subtract
        case 0xFFAE: return U'.';    // XK_KP_Decimal
        case 0xFFAF: return U'/';    // XK_KP_Divide
        case 0xFFBD: return U'=';    // XK_KP_Equal
        case 0x20AC: return 0x20AC;  // XK_EuroSign
        default: return 0;
    }
}

char32_t ascii_lower(char32_t c) { return (c >= U'A' && c <= U'Z') ? c + 32 : c; }

}  // namespace kt::platform::detail
