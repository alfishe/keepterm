// Terminal colors of the view.
//
// Colors are straight sRGB 0xRRGGBB. The 256-color palette is the theme's 16 ANSI colors plus
// the xterm 6x6x6 cube and the 24-step gray ramp. Theme parsing and the other built-in themes
// belong to the config; the view ships "Keepterm Dark" so that it works without config.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "vt/cell.h"

namespace kt::view {

struct Theme {
    std::string name;
    std::array<uint32_t, 16> palette{};   // ANSI 0..15
    uint32_t foreground = 0xE0DEF4;
    uint32_t background = 0x302F3A;
    uint32_t cursor = 0xE0DEF4;
    uint32_t cursor_text = 0x302F3A;
    uint32_t selection_background = 0x33405A;
    std::optional<uint32_t> selection_foreground;   // empty: keep the cell colors
    std::optional<uint32_t> bold;                   // bold text in the default foreground; empty: foreground
    bool bold_is_bright = false;                    // bold text in ANSI 0-7 uses 8-15 (theme.bold_is_bright)

    // The built-in "Keepterm Dark".
    static const Theme& keepterm_dark();

    // 0xRRGGBB of palette index i.
    uint32_t color256(uint8_t i) const;
    // A cell color: Default resolves to `def`.
    uint32_t resolve(vt::Color c, uint32_t def) const;
    // A cell's foreground with the bold rules (bold color, bold is bright).
    uint32_t resolve_fg(vt::Color c, bool is_bold) const;

    friend bool operator==(const Theme&, const Theme&) = default;
};

// 0xRRGGBB to a draw-list color (premultiplied RGBA8, opaque).
uint32_t render_color(uint32_t rgb);
// a + (b - a) * t per channel, t in [0, 1].
uint32_t mix_rgb(uint32_t a, uint32_t b, float t);

}  // namespace kt::view
