// Input events of the platform layer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform_gui/geometry.h"

namespace kt::platform {

// Physical key, position-based, named after the US layout (USB HID usage page 7).
enum class Key : uint16_t {
    unknown = 0,
    a = 4, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,   // 4..29
    d1 = 30, d2, d3, d4, d5, d6, d7, d8, d9, d0,                                     // 30..39
    enter = 40, escape, backspace, tab, space,                                       // 40..44
    minus = 45, equal, bracket_left, bracket_right, backslash, non_us_hash,          // 45..50
    semicolon = 51, quote, grave, comma, period, slash,                              // 51..56
    caps_lock = 57,
    f1 = 58, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,                          // 58..69
    print_screen = 70, scroll_lock, pause, insert, home, page_up,                    // 70..75
    delete_ = 76, end, page_down, right, left, down, up,                             // 76..82
    num_lock = 83, kp_divide, kp_multiply, kp_subtract, kp_add, kp_enter,            // 83..88
    kp_1 = 89, kp_2, kp_3, kp_4, kp_5, kp_6, kp_7, kp_8, kp_9, kp_0, kp_decimal,     // 89..99
    non_us_backslash = 100, context_menu, power, kp_equal,                           // 100..103
    f13 = 104, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24,                // 104..115
    help = 117,
    mute = 127, volume_up, volume_down,                                              // 127..129
    kp_comma = 133,
    intl_ro = 135, kana_mode, intl_yen, convert, non_convert,                        // 135..139
    lang1 = 144, lang2,                                                              // 144..145
    left_ctrl = 224, left_shift, left_alt, left_super,                               // 224..227
    right_ctrl = 228, right_shift, right_alt, right_super,                           // 228..231
};

// Short stable name for diagnostics and tests ("a", "enter", "kp_1", "left_ctrl"); "unknown"
// for values without a name.
std::string_view key_name(Key);
bool is_modifier_key(Key);

enum Mods : uint16_t {
    mod_none = 0,
    mod_shift = 1 << 0, mod_ctrl = 1 << 1, mod_alt = 1 << 2, mod_super = 1 << 3,
    mod_altgr = 1 << 4, mod_caps  = 1 << 5, mod_num = 1 << 6,
    mod_lshift = 1 << 8, mod_rshift = 1 << 9, mod_lctrl = 1 << 10, mod_rctrl = 1 << 11,
    mod_lalt = 1 << 12, mod_ralt = 1 << 13, mod_lsuper = 1 << 14, mod_rsuper = 1 << 15,
};

// "ctrl+shift" style text for diagnostics.
std::string mods_to_string(uint16_t mods);

enum class KeyAction : uint8_t { press, repeat, release };

// Text rules shared by all OSes: `text` and on_text never carry C0 controls or DEL (Enter, Tab,
// Backspace, Escape, ctrl+letter); those keys are encoded from `key` and `mods` by the VT input
// encoder.
struct KeyEvent {
    Key        key = Key::unknown;   // physical
    char32_t   logical = 0;          // layout-dependent base character without modifiers, or 0
    uint32_t   native_code = 0;      // VK / keyCode / X11 keycode, for diagnostics and keymap escapes
    uint16_t   mods = 0;
    KeyAction  action = KeyAction::press;
    std::u8string text;              // characters the OS would type (after dead keys), may be empty
    bool       composing = false;    // IME or dead key in progress; keymap must not act on it
};

struct Composition {                 // IME preedit
    std::u8string text;              // empty = composition ended
    uint32_t caret = 0;              // byte offset in text
    struct Segment { uint32_t begin = 0, end = 0; bool selected = false; };
    std::vector<Segment> segments;
};

enum class MouseButton : uint8_t { none, left, middle, right, back, forward };

struct MouseEvent {
    enum class Type : uint8_t { move, press, release, enter, leave };
    Type        type = Type::move;
    MouseButton button = MouseButton::none;
    uint8_t     click_count = 0;     // 1, 2, 3 ... for press; 0 otherwise
    uint16_t    mods = 0;
    DipPoint    pos;                 // client area, top-left origin
};

// dy > 0 scrolls toward the top of the content (wheel away from the user), dx > 0 toward the
// left edge, as AppKit and WM_MOUSEWHEEL report it.
struct ScrollEvent {
    float dx = 0, dy = 0;            // in lines when !precise, in DIPs when precise
    bool  precise = false;           // touchpad / high-resolution wheel
    enum class Phase : uint8_t { none, begin, update, end, momentum };
    Phase    phase = Phase::none;
    uint16_t mods = 0;
    DipPoint pos;
};

struct DropEvent {
    std::vector<std::filesystem::path> paths;
    std::u8string text;              // when the drop is text only
    DipPoint pos;
};

struct DragOverEvent {
    DipPoint pos;
    bool has_paths = false, has_text = false;
    uint16_t mods = 0;
};

enum class DropEffect : uint8_t { none, copy };

}  // namespace kt::platform
