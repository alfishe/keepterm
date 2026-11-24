// DPI and scale math for Windows, macOS and X11. Pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "platform_gui/geometry.h"

namespace kt::platform::detail {

inline constexpr uint32_t kBaseDpi = 96;

// Windows: scale = DPI / 96, DPI clamped to the supported 96..480 (100..500 %).
float scale_from_dpi(uint32_t dpi);
// Back to a DPI value (rounded), for APIs that take one.
uint32_t dpi_from_scale(float scale);

// Physical pixels for a DIP length: rounded to nearest, never negative.
int32_t dip_to_px(float dip, float scale);
float   px_to_dip(int32_t px, float scale);
PxSize  to_px(DipSize, float scale);
DipSize to_dip(PxSize, float scale);
DipRect to_dip(int32_t x, int32_t y, int32_t w, int32_t h, float scale);

// WM_GETDPISCALEDSIZE: the client size at `new_dpi` that keeps the DIP size of `client` at
// `old_dpi` exactly (instead of the OS's linear guess of the whole window).
PxSize rescale_client(PxSize client, uint32_t old_dpi, uint32_t new_dpi);

// X11: Xft.dpi from the RESOURCE_MANAGER string ("Xft.dpi:\t120\n..."), if present and sane.
std::optional<float> parse_xft_dpi(std::string_view resources);
// X11 scale: GDK_SCALE (a positive integer) wins, else Xft.dpi / 96, else 1. Clamped to 0.5..5.
float x11_scale(std::string_view resources, const char* gdk_scale_env);

// Wheel: WM_MOUSEWHEEL deltas in 1/120 notches. A delta that is not a multiple of 120 comes
// from a high-resolution device.
bool  wheel_is_precise(int32_t delta);
float wheel_lines(int32_t delta, uint32_t lines_per_notch);

}  // namespace kt::platform::detail
