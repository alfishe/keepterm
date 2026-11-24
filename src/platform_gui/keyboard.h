// Keyboard translation tables and modifier mapping. Pure functions shared by
// the per-OS translators and the unit tests; no OS headers.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "platform_gui/input.h"

namespace kt::platform::detail {

// ---------------------------------------------------------------- physical keys
// Linux evdev code (X11 keycode - 8, Wayland wl_keyboard.key) to Key.
Key key_from_evdev(uint32_t code);
// Windows set-1 scan code ((lParam >> 16) & 0xFF) and the extended-key bit (lParam bit 24).
Key key_from_win_scancode(uint32_t scancode, bool extended);
// macOS virtual key code (Carbon kVK_*, NSEvent.keyCode).
Key key_from_mac_keycode(uint16_t keycode);

// ---------------------------------------------------------------- modifiers
// The side bit of a modifier key (mod_lshift for left_shift, ...), 0 for other keys.
uint16_t side_mod_of(Key);
// Tracks left/right modifier state from key presses and releases (X11, Wayland).
uint16_t update_side_mods(uint16_t sides, Key key, bool pressed);
// Generic bits (mod_shift, mod_ctrl, mod_alt, mod_super) implied by side bits.
uint16_t generic_from_sides(uint16_t sides);

// NSEvent.modifierFlags, including the device-dependent NX_DEVICE* bits for left/right.
// AppKit has no AltGr: Option stays mod_alt.
uint16_t mods_from_mac_flags(uint64_t flags);
// Which Option key, if any, is down according to the device-dependent bits.
struct MacOptionSides { bool left = false, right = false; };
MacOptionSides mac_option_sides(uint64_t flags);

// X11 core state (ShiftMask = 1, LockMask = 2, ControlMask = 4, Mod1 = 8 ...). Mod1 is Alt,
// Mod2 NumLock, Mod4 Super, Mod5 ISO_Level3_Shift (AltGr) under every common XKB config.
// `sides` comes from update_side_mods.
uint16_t mods_from_x11_state(uint32_t state, uint16_t sides);

// Windows: snapshot of GetKeyState for the keys that matter.
struct WinKeyState {
    bool lshift = false, rshift = false, lctrl = false, rctrl = false;
    bool lalt = false, ralt = false, lwin = false, rwin = false;
    bool caps = false, num = false;
    bool altgr = false;   // RAlt with the synthesized LCtrl (see is_altgr_lctrl)
};
uint16_t mods_from_win(const WinKeyState&);

// AltGr on Windows arrives as a synthesized LCtrl press followed by RAlt (extended VK_MENU)
// with the same message time. Returns true when `ctrl` is that synthesized LCtrl.
struct RawKeyMsg {
    uint32_t message = 0;     // WM_KEYDOWN 0x100, WM_KEYUP 0x101, WM_SYSKEYDOWN 0x104, WM_SYSKEYUP 0x105
    uint32_t vk = 0;          // VK_CONTROL 0x11, VK_MENU 0x12
    bool     extended = false;
    uint32_t time = 0;        // GetMessageTime()
};
bool is_altgr_lctrl(const RawKeyMsg& ctrl, const RawKeyMsg& next);

// ---------------------------------------------------------------- text
// True for characters that belong in KeyEvent::text / on_text: not C0, not DEL, not C1,
// not the AppKit function-key private-use range U+F700..U+F8FF, not a lone surrogate.
bool is_text_char(char32_t);
// X11 keysym to Unicode: Latin-1 keysyms, the 0x01000000 + UCS range, and the keypad digits.
// Returns 0 for function keys and legacy non-Latin-1 keysyms.
char32_t keysym_to_ucs(uint32_t keysym);
// Lower-case ASCII letters; leaves everything else as is (logical keys are unshifted).
char32_t ascii_lower(char32_t);

}  // namespace kt::platform::detail
