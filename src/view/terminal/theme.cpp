#include "view/terminal/theme.h"

#include <algorithm>
#include <cmath>

#include "render/types.h"

namespace kt::view {

const Theme& Theme::keepterm_dark() {
    static const Theme t = [] {
        Theme th;
        th.name = "Keepterm Dark";
        th.palette = {0x3A3946, 0xEE5D55, 0x4EBB65, 0xF4C10D, 0x5290FF, 0xBF7AF0, 0x56C2D6, 0xC9C7DC,
                      0x6E6C80, 0xFF7A70, 0x6CD483, 0xFFD54A, 0x7AA8FF, 0xD49CF8, 0x7DD8E8, 0xF4F2FF};
        th.foreground = 0xE0DEF4;
        th.background = 0x302F3A;
        th.cursor = 0xE0DEF4;
        th.cursor_text = 0x302F3A;
        th.selection_background = 0x4A4960;
        return th;
    }();
    return t;
}

uint32_t Theme::color256(uint8_t i) const {
    if (i < 16) return palette[i];
    if (i < 232) {
        static constexpr uint32_t kLevels[6] = {0x00, 0x5F, 0x87, 0xAF, 0xD7, 0xFF};
        const uint32_t v = i - 16u;
        return (kLevels[v / 36] << 16) | (kLevels[(v / 6) % 6] << 8) | kLevels[v % 6];
    }
    const uint32_t g = 8u + 10u * (i - 232u);
    return (g << 16) | (g << 8) | g;
}

uint32_t Theme::resolve(vt::Color c, uint32_t def) const {
    switch (c.tag()) {
        case vt::Color::Palette: return color256(c.index());
        case vt::Color::Rgb: return (uint32_t{c.r()} << 16) | (uint32_t{c.g()} << 8) | c.b();
        case vt::Color::Default: break;
    }
    return def;
}

uint32_t Theme::resolve_fg(vt::Color c, bool is_bold) const {
    if (!is_bold) return resolve(c, foreground);
    if (bold_is_bright && c.tag() == vt::Color::Palette && c.index() < 8)
        return color256(static_cast<uint8_t>(c.index() + 8));
    return resolve(c, bold.value_or(foreground));
}

uint32_t render_color(uint32_t rgb) { return render::rgba((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        const float ca = static_cast<float>((a >> s) & 0xFF), cb = static_cast<float>((b >> s) & 0xFF);
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << s;
    }
    return out;
}

}  // namespace kt::view
