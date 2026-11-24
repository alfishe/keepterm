// Layout-aware matching: platform key event → the keys a binding can name.
// Pure functions; the platform layer already filled KeyEvent::logical with the
// layout's base character (macOS: the ASCII-capable character for non-Latin layouts; Linux:
// the first group's base keysym).
#pragma once

#include <cstdint>

#include "commands/keys.h"
#include "platform_gui/input.h"

namespace kt::commands {

struct EventChords {
    Chord logical;        // named key, or what the key types (rule 1-4); empty for modifier keys
    Chord physical;       // phys:<code> (rule 5)
    Chord fallback;       // the US-position key when it differs from `logical` (AZERTY digit
                          // row, non-Latin letters); empty otherwise
    bool modifier_key = false;
};

// Chord modifier bits of platform mods (AltGr without text counts as Ctrl+Alt).
std::uint8_t chord_mods(std::uint16_t platform_mods);

// The key at a physical position on a US layout (letters, digits, symbols), or empty.
Key us_key_of(platform::Key k);

EventChords map_event(const platform::KeyEvent& e, Os os);

// AltGr guard (Windows, Linux): a chord with both Ctrl and Alt (or AltGr) that types a
// printable character in the current layout is text, not a command.
bool altgr_types_text(const platform::KeyEvent& e, Os os);

}  // namespace kt::commands
