// Theme tokens of the UI: the chrome derives its colors from the terminal
// theme, so the sidebar and the panes read as one surface (the Macterm rule).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "ui/geometry.h"

namespace kt::ui {

enum class Scheme : uint8_t { dark, light };

// What the theme module gives the toolkit: the terminal's colors.
struct ThemeSource {
    uint32_t background = 0x302F3A;           // 0xRRGGBB
    uint32_t foreground = 0xE0DEF4;
    std::optional<uint32_t> accent;           // theme accent; else OS accent; else blue
    std::array<uint32_t, 16> palette{};       // ANSI 0..15
    float unfocused_split_opacity = 0.7f;     // window.unfocused_split_opacity
};

enum class ProjectColor : uint8_t { none, red, orange, yellow, green, teal, blue, purple, pink, gray };

struct FontRole {
    float size = 13;        // DIP, before ui.font_scale
    bool bold = false;      // semibold where the face has it, else bold
    bool mono = false;      // the UI monospace font (paths, key caps)
    bool operator==(const FontRole&) const = default;
};

struct Theme {
    Scheme scheme = Scheme::dark;
    Color bg, fg, fg_muted, fg_dim, surface, surface_raised, border, hover, selection, selection_inactive;
    Color accent, accent_soft, on_accent, success, warning, failure, scrim, shadow, dim_overlay;
    Color sidebar_bg;          // Macterm's sidebar material: a neutral gray, whatever the theme
    Color sidebar_border;      // the line between the sidebar and the content
    std::array<Color, 10> tags{};   // ProjectColor, per scheme

    // Metrics (DIP).
    float radius_control = 6, radius_panel = 10, radius_keycap = 4;
    float row_height = 26, control_height = 24, titlebar_height = 38, status_height = 24;
    float pane_header_height = 24, sidebar_width = 220, sidebar_min = 160, sidebar_max = 480;
    float font_scale = 1.0f;

    FontRole body() const { return {13 * font_scale, false, false}; }
    FontRole body_bold() const { return {13 * font_scale, true, false}; }
    FontRole small() const { return {11 * font_scale, false, false}; }
    FontRole small_bold() const { return {11 * font_scale, true, false}; }
    FontRole title() const { return {15 * font_scale, true, false}; }
    FontRole large_title() const { return {20 * font_scale, true, false}; }
    FontRole mono_small() const { return {11 * font_scale, false, true}; }

    Color tag(ProjectColor c) const { return tags[static_cast<size_t>(c)]; }

    // Tokens from a terminal theme. fg_muted is raised until it reaches 4.5:1 on bg.
    static Theme derive(const ThemeSource&);
    static Theme dark();    // from "Keepterm Dark"
    static Theme light();   // from the built-in light terminal colors
};

// The light terminal colors used until the theme module provides "Keepterm Light".
ThemeSource builtin_light_source();
ThemeSource builtin_dark_source();

}  // namespace kt::ui
