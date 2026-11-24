#include "ui/theme.h"

#include <cmath>

namespace kt::ui {

namespace {

float lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

}  // namespace

float luminance(Color c) { return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b); }

float contrast(Color a, Color b) {
    const float la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

ThemeSource builtin_dark_source() {
    ThemeSource s;
    // "Keepterm Dark" (src/theme/builtin): Macterm's dark look.
    s.background = 0x302F3A;
    s.foreground = 0xE0DEF4;
    s.accent = 0x5290FF;
    s.palette = {0x3A3946, 0xEE5D55, 0x4EBB65, 0xF4C10D, 0x5290FF, 0xBF7AF0, 0x56C2D6, 0xC9C7DC,
                 0x6E6C80, 0xFF7A70, 0x6CD483, 0xFFD54A, 0x7AA8FF, 0xD49CF8, 0x7DD8E8, 0xF4F2FF};
    return s;
}

ThemeSource builtin_light_source() {
    ThemeSource s;
    s.background = 0xFAFAFB;
    s.foreground = 0x24272E;
    s.palette = {0x24272E, 0xC9343B, 0x2F8F46, 0xA06B00, 0x2563EB, 0x8B47C9, 0x16838C, 0xD5D8DE,
                 0x6B7280, 0xE0464D, 0x3BA457, 0xB98300, 0x3B82F6, 0xA25FDD, 0x1E9AA4, 0xFFFFFF};
    return s;
}

Theme Theme::derive(const ThemeSource& s) {
    Theme t;
    t.bg = Color::rgb(s.background);
    t.fg = Color::rgb(s.foreground);
    // Macterm: fall back to black or white when the theme's foreground misses 4.5:1.
    if (contrast(t.fg, t.bg) < 4.5f) t.fg = luminance(t.bg) > 0.5f ? Color::rgb(0x000000) : Color::rgb(0xFFFFFF);
    t.scheme = luminance(t.bg) > 0.5f ? Scheme::light : Scheme::dark;
    const bool light = t.scheme == Scheme::light;

    float muted = 0.65f;
    while (muted < 1.0f && contrast(over(t.fg.with_alpha(muted), t.bg), t.bg) < 4.5f) muted += 0.05f;
    t.fg_muted = t.fg.with_alpha(std::min(muted, 1.0f));
    t.fg_dim = t.fg.with_alpha(0.40f);
    t.surface = over(t.fg.with_alpha(0.08f), t.bg);
    t.surface_raised = over(t.fg.with_alpha(0.12f), t.bg);
    t.border = t.fg.with_alpha(0.12f);
    t.hover = t.fg.with_alpha(0.06f);
    t.accent = s.accent ? Color::rgb(*s.accent) : (s.palette[4] ? Color::rgb(s.palette[4]) : Color::rgb(0x3B82F6));
    t.accent_soft = t.accent.with_alpha(0.10f);
    t.on_accent = luminance(t.accent) > 0.45f ? Color::rgb(0x000000) : Color::rgb(0xFFFFFF);
    t.selection = t.fg.with_alpha(light ? 0.10f : 0.11f);
    t.selection_inactive = t.fg.with_alpha(0.07f);
    t.success = Color::rgb(s.palette[2] ? s.palette[2] : 0x22C55E);
    t.warning = Color::rgb(s.palette[3] ? s.palette[3] : 0xEAB308);
    t.failure = Color::rgb(s.palette[1] ? s.palette[1] : 0xEF4444);
    t.scrim = Color::rgb(0x000000, light ? 0.15f : 0.25f);
    t.shadow = Color::rgb(0x000000, light ? 0.18f : 0.35f);
    t.dim_overlay = t.bg.with_alpha(1.0f - std::clamp(s.unfocused_split_opacity, 0.15f, 1.0f));
    // Macterm's sidebar is the system's neutral sidebar material, not a tint of the terminal:
    // a mid gray in dark mode (lighter than the content), a light gray in light mode, whatever
    // the theme. A black hairline (dark) separates it from the content.
    t.sidebar_bg = light ? Color::rgb(0xE9E8EA) : Color::rgb(0x4B4A4D);
    t.sidebar_border = light ? Color::rgb(0x000000, 0.12f) : Color::rgb(0x000000, 0.85f);
    const uint32_t dark_tags[10] = {0, 0xF87171, 0xFB923C, 0xFACC15, 0x4ADE80, 0x2DD4BF, 0x60A5FA, 0xC084FC, 0xF472B6, 0x9CA3AF};
    const uint32_t light_tags[10] = {0, 0xDC2626, 0xEA580C, 0xCA8A04, 0x16A34A, 0x0D9488, 0x2563EB, 0x9333EA, 0xDB2777, 0x6B7280};
    for (size_t i = 0; i < 10; ++i) t.tags[i] = i == 0 ? t.fg_muted : Color::rgb(light ? light_tags[i] : dark_tags[i]);
    return t;
}

Theme Theme::dark() { return derive(builtin_dark_source()); }
Theme Theme::light() { return derive(builtin_light_source()); }

}  // namespace kt::ui
