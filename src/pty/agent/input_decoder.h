// Console agent input decoder: input bytes -> key events. Portable.
//
// Handles win32-input-mode (CSI Vk;Sc;Uc;Kd;Cs;Rc _), the VT key sequences of the encoder
// (arrows, Home/End, Insert/Delete, PgUp/PgDn, F1-F12, with xterm modifiers), Shift+Tab, focus
// reports, Alt as an ESC prefix, C0 controls as Ctrl+letter, DEL as Backspace and UTF-8 text.
// Mouse reports (CSI < ... M/m) and bracketed paste markers are consumed and dropped.
//
// A lone ESC at the end of a chunk is held: it may start a sequence. The agent calls flush()
// when no byte followed within 50 ms, and the ESC becomes the Escape key.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kt::pty::agent {

// dwControlKeyState bits (wincon.h).
inline constexpr uint32_t kRightAlt = 0x0001;
inline constexpr uint32_t kLeftAlt = 0x0002;
inline constexpr uint32_t kRightCtrl = 0x0004;
inline constexpr uint32_t kLeftCtrl = 0x0008;
inline constexpr uint32_t kShift = 0x0010;
inline constexpr uint32_t kEnhanced = 0x0100;

// Virtual key codes (winuser.h).
namespace vk {
inline constexpr uint16_t Cancel = 0x03, Back = 0x08, Tab = 0x09, Return = 0x0D, Escape = 0x1B, Space = 0x20;
inline constexpr uint16_t Prior = 0x21, Next = 0x22, End = 0x23, Home = 0x24;
inline constexpr uint16_t Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
inline constexpr uint16_t Insert = 0x2D, Delete = 0x2E, F1 = 0x70, Packet = 0xE7;
}  // namespace vk

struct KeyInput {
    bool down = true;
    uint16_t vk = 0;    // 0: derive from ch with the keyboard layout (VkKeyScanW)
    uint16_t scan = 0;  // 0: derive from vk (MapVirtualKeyW)
    char16_t ch = 0;
    uint32_t mods = 0;  // control key state
    uint16_t repeat = 1;
    friend bool operator==(const KeyInput&, const KeyInput&) = default;
};

struct InputEvent {
    enum class Kind : uint8_t { Key, Focus } kind = Kind::Key;
    KeyInput key;
    bool focus = false;
};

class InputDecoder {
public:
    void feed(std::string_view bytes, std::vector<InputEvent>& out);
    // True when bytes are held (a lone ESC or an incomplete sequence).
    bool pending() const { return !buf_.empty(); }
    // Called after the ESC timeout: the held bytes are taken as they are.
    void flush(std::vector<InputEvent>& out);

private:
    void parse(std::vector<InputEvent>& out, bool final);
    std::string buf_;
};

}  // namespace kt::pty::agent
