// The input encoder: keys, mouse, paste and focus to bytes for the PTY.
//
// Pure functions, any thread, no OS calls. The event structs are the VT module's own. They mirror
// the platform input events closely enough that the adapter is a copy:
//   - Key uses the same values as platform::Key (USB HID usage page 7, US layout names);
//   - Mods uses the xterm parameter order (Shift 1, Alt 2, Ctrl 4, Super 8), so the
//     adapter maps the platform bits once;
//   - KeyEvent::logical is platform::KeyEvent::logical (layout base character, or 0).
//
// Not here yet: win32-input-mode (9001) and the kitty keyboard protocol. Keys are encoded in
// the legacy xterm forms when those modes are set. With 9001 set, or
// EncoderOptions::console_reader, modifyOtherKeys is not used: the reader is a Windows console,
// which does not decode it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "vt/types.h"

namespace kt::vt {

// Physical key, numbered like USB HID usage page 7 (= platform::Key).
enum class Key : uint16_t {
    unknown = 0,
    a = 4, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,  // 4..29
    d1 = 30, d2, d3, d4, d5, d6, d7, d8, d9, d0,                                        // 30..39
    enter = 40, escape, backspace, tab, space,                                          // 40..44
    minus = 45, equal, left_bracket, right_bracket, backslash, non_us_hash,             // 45..50
    semicolon = 51, apostrophe, grave, comma, period, slash, caps_lock,                 // 51..57
    f1 = 58, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,                             // 58..69
    print_screen = 70, scroll_lock, pause, insert, home, page_up, del, end, page_down,  // 70..78
    right = 79, left, down, up,                                                         // 79..82
    num_lock = 83, kp_divide, kp_multiply, kp_minus, kp_plus, kp_enter,                 // 83..88
    kp_1 = 89, kp_2, kp_3, kp_4, kp_5, kp_6, kp_7, kp_8, kp_9, kp_0, kp_decimal,         // 89..99
    non_us_backslash = 100, application, power, kp_equal,                               // 100..103
    f13 = 104, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24,                   // 104..115
    kp_comma = 133,
};

// Modifier bits in xterm order: the modifier parameter is 1 + (mods & 15).
enum Mods : uint8_t {
    kModNone = 0,
    kModShift = 1,
    kModAlt = 2,
    kModCtrl = 4,
    kModSuper = 8,
    kModCapsLock = 16,
    kModNumLock = 32,
};

enum class KeyAction : uint8_t { Press, Repeat, Release };

struct KeyEvent {
    Key key = Key::unknown;
    uint8_t mods = kModNone;        // Mods bits
    KeyAction action = KeyAction::Press;
    std::u8string_view text;        // what the layout typed (after dead keys and IME); may be empty
    char32_t logical = 0;           // layout base character without modifiers, or 0
    uint16_t native_vk = 0;         // Windows virtual key, if known (win32-input-mode, later)
    uint16_t native_scan = 0;       // hardware scan code, if known
};

enum class MouseAction : uint8_t { Press, Release, Move, Wheel };

// X11 button numbers: Left 1, Middle 2, Right 3, wheel 4..7, Back 8, Forward 9, 10, 11.
enum class MouseButton : uint8_t {
    None, Left, Middle, Right, WheelUp, WheelDown, WheelLeft, WheelRight, Back, Forward, B10, B11,
};

struct MouseEvent {
    MouseAction action = MouseAction::Press;
    MouseButton button = MouseButton::None;  // Move: the button held, or None
    uint8_t mods = kModNone;                 // Shift, Alt, Ctrl count; the view handles the Shift bypass
    uint16_t col = 0, row = 0;               // 0-based cells in the viewport
    uint16_t px = 0, py = 0;                 // 0-based pixels inside the grid (mode 1016)
};

struct EncoderOptions {
    bool alt_sends_esc = true;     // macOS: the "option as alt" setting
    bool backspace_is_del = true;  // Backspace sends DEL (0x7F), else BS (0x08); DECBKM flips it
    // A Windows console reads the input (every Windows backend: ConPTY, the agent, OpenConsole),
    // not the program. It decodes the legacy key forms only, so modifyOtherKeys is not used.
    // Set 9001 (win32-input-mode) means the same.
    bool console_reader = false;
};

class InputEncoder {
public:
    // All functions append to `out`. They return false when the event produces no bytes; the view
    // then uses the event itself (a release, a mouse move without 1003, a selection drag).
    static bool encode_key(const KeyEvent&, const InputModes&, const EncoderOptions&, std::string& out);
    static bool encode_mouse(const MouseEvent&, const InputModes&, std::string& out);
    // Normalizes line ends to CR, strips C0 (except HT and CR), DEL and C1 controls, so a pasted
    // `ESC [ 2 0 1 ~` cannot end bracketed paste early; wraps in CSI 200~ / CSI 201~ with 2004.
    static void encode_paste(std::u8string_view text, const InputModes&, std::string& out);
    static void encode_focus(bool focused, const InputModes&, std::string& out);
};

}  // namespace kt::vt
