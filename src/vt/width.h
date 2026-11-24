// Cell widths. The VT core owns them; the text module and the console agent
// use the same tables, so the screen layout never depends on a font.
//
// Self-contained: no includes beyond <cstdint>. The tables are generated from the Unicode
// Character Database by tools/gen_width_tables.py (src/vt/width_tables.cpp).
#pragma once

#include <cstdint>

namespace kt::vt::width {

// Bumped on every Unicode or override change. Stored in snapshots (META) and in HELLO.
inline constexpr uint32_t kTableVersion = 1;

enum class W : uint8_t { Zero, Narrow, Wide, Ambiguous };

// Table lookup, no allocation, any thread. Code points above U+10FFFF are Narrow.
W classify(char32_t cp);

// 0, 1 or 2 (width rules 1-6, without the VS16 rule). Controls are 0.
int cell_width(char32_t cp, bool ambiguous_is_wide);

// Emoji_Presentation=Yes.
bool is_emoji_presentation(char32_t cp);

// Unicode version of the tables, for example "16.0.0".
const char* unicode_version();

}  // namespace kt::vt::width
