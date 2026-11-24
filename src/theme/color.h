// Colors of themes: parsing, formatting, luminance and contrast.
//
// A color is 0xRRGGBB (sRGB, straight). UI tokens may carry alpha: 0xRRGGBBAA in a separate
// type so the two never mix.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kt::theme {

using Rgb = uint32_t;    // 0xRRGGBB
using Rgba = uint32_t;   // 0xRRGGBBAA

// "#RRGGBB", "RRGGBB", "#RGB" (Ghostty also writes colors without '#').
std::optional<Rgb> parse_rgb(std::string_view s);
// As parse_rgb, plus "#RRGGBBAA"; colors without alpha get AA = FF.
std::optional<Rgba> parse_rgba(std::string_view s);

std::string format_rgb(Rgb c);     // "#rrggbb"
std::string format_rgba(Rgba c);   // "#rrggbbaa", or "#rrggbb" when opaque

// WCAG relative luminance, 0..1.
double luminance(Rgb c);
// WCAG contrast ratio, 1..21.
double contrast(Rgb a, Rgb b);
// a + (b - a) * t per channel.
Rgb mix(Rgb a, Rgb b, double t);

// `fg` moved toward black or white until it reaches `ratio` against `bg` (theme.minimum_contrast);
// unchanged when it already does or ratio <= 1.
Rgb ensure_contrast(Rgb fg, Rgb bg, double ratio);

}  // namespace kt::theme
