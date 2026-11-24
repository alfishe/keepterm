// Keys, chords and sequences of the keymap, and their text grammar.
//
// Core: no OS headers. A binding names a logical key (what the key types, a named key, or a
// physical position with `phys:`); layout.h maps a platform key event to the same values.
//
// Grammar (case-insensitive, modifier order free):
//   sequence := chord ( " " chord ){0,3}
//   chord    := ( modifier "+" )* key
//   modifier := ctrl | control | shift | alt | opt | option | super | cmd | command | win | meta
//             | primary                 (macOS cmd; Windows/Linux ctrl+shift, and shift then adds alt)
//   key      := letter | digit | named | symbol | "phys:" W3C-code
//   special  := "none"                  (unbind; parse_binding_text reports it)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/expected.h"

namespace kt::commands {

enum class Os : std::uint8_t { MacOS, Windows, Linux };

// The OS this build runs on.
Os current_os();
std::string_view os_name(Os os);            // "macos", "windows", "linux"
std::optional<Os> parse_os(std::string_view name);

// Modifier bits of a chord. Super is Cmd on macOS and the Win key on Windows.
enum Mods : std::uint8_t { Ctrl = 1, Shift = 2, Alt = 4, Super = 8 };

enum class NamedKey : std::uint8_t {
    None = 0,
    Enter, Tab, Space, Escape, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
    Up, Down, Left, Right,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    Kp0, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9,
    KpAdd, KpSubtract, KpMultiply, KpDivide, KpDecimal, KpEqual,
    Menu, PrintScreen, Pause, ScrollLock,
    Count_
};

// A logical key, layout-resolved.
struct Key {
    enum class Kind : std::uint8_t { None, Char, Named, Physical };
    Kind kind = Kind::None;
    char32_t ch = 0;                 // Char: printable ASCII, letters lower case
    NamedKey named = NamedKey::None; // Named
    std::uint16_t physical = 0;      // Physical: USB HID usage (platform::Key), from a W3C code

    static Key character(char32_t c);
    static Key named_key(NamedKey n) { return Key{Kind::Named, 0, n, 0}; }
    static Key phys(std::uint16_t hid) { return Key{Kind::Physical, 0, NamedKey::None, hid}; }

    bool empty() const { return kind == Kind::None; }
    // Dense value for hashing: kind in the top byte.
    std::uint32_t code() const;
    friend bool operator==(const Key& a, const Key& b) { return a.code() == b.code(); }
};

struct Chord {
    std::uint8_t mods = 0;
    Key key;
    std::uint64_t code() const { return (static_cast<std::uint64_t>(mods) << 32) | key.code(); }
    friend bool operator==(const Chord& a, const Chord& b) { return a.code() == b.code(); }
};

inline constexpr std::size_t kMaxChords = 4;
inline constexpr std::int64_t kSequenceTimeoutMs = 1000;

struct Sequence {
    std::array<Chord, kMaxChords> chords{};
    std::uint8_t length = 0;

    static Sequence of(Chord c) {
        Sequence s;
        s.chords[0] = c;
        s.length = 1;
        return s;
    }
    bool empty() const { return length == 0; }
    const Chord& front() const { return chords[0]; }
    // True when `this` is a proper or equal prefix of `other`.
    bool is_prefix_of(const Sequence& other) const;
    friend bool operator==(const Sequence& a, const Sequence& b);
};

struct ParseError {
    std::size_t pos = 0;   // byte offset in the parsed text
    std::string message;   // English
};

// One chord or a sequence. "none" is an error here (see parse_binding_text).
Expected<Sequence, ParseError> parse_sequence(std::string_view text, Os os);

// A binding value: a sequence, or nullopt for "none" (unbind).
Expected<std::optional<Sequence>, ParseError> parse_binding_text(std::string_view text, Os os);

enum class Format : std::uint8_t {
    Config,   // canonical grammar: "ctrl+shift+alt+super+d", "ctrl+grave", "cmd+backslash"
    Text,     // "Ctrl+Shift+D"; macOS "⇧⌘D" (key caps in menus and the palette)
    Words,    // "Ctrl+Shift+D"; macOS "Shift+Cmd+D" (logs, CLI, Markdown docs)
};
std::string format_chord(const Chord& c, Os os, Format f);
std::string format_sequence(const Sequence& s, Os os, Format f);

// Key names. key_from_name accepts every alias of the grammar ("return", "pgup", "kp_enter"
// folds to enter, "phys:KeyQ"); nullopt for an unknown name.
std::optional<Key> key_from_name(std::string_view name);
// Canonical config name of a named key ("pageup", "f11", "kp_add").
std::string_view named_key_name(NamedKey n);
// W3C code ("KeyQ", "Digit1", "Backquote") of a HID usage, or "" when it has none.
std::string_view w3c_code_name(std::uint16_t hid);
std::optional<std::uint16_t> hid_from_w3c_code(std::string_view code);

}  // namespace kt::commands
