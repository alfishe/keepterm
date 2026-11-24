#include "platform_gui/dpi.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace kt::platform::detail {

float scale_from_dpi(uint32_t dpi) {
    dpi = std::clamp<uint32_t>(dpi, 96, 480);
    return static_cast<float>(dpi) / static_cast<float>(kBaseDpi);
}

uint32_t dpi_from_scale(float scale) {
    if (!(scale > 0)) return kBaseDpi;
    return static_cast<uint32_t>(std::lround(scale * static_cast<float>(kBaseDpi)));
}

int32_t dip_to_px(float dip, float scale) {
    const float v = dip * scale;
    if (!(v > 0)) return 0;
    return static_cast<int32_t>(std::lround(v));
}

float px_to_dip(int32_t px, float scale) {
    if (!(scale > 0)) return static_cast<float>(px);
    return static_cast<float>(px) / scale;
}

PxSize to_px(DipSize s, float scale) { return {dip_to_px(s.w, scale), dip_to_px(s.h, scale)}; }

DipSize to_dip(PxSize s, float scale) { return {px_to_dip(s.w, scale), px_to_dip(s.h, scale)}; }

DipRect to_dip(int32_t x, int32_t y, int32_t w, int32_t h, float scale) {
    return {px_to_dip(x, scale), px_to_dip(y, scale), px_to_dip(w, scale), px_to_dip(h, scale)};
}

PxSize rescale_client(PxSize c, uint32_t old_dpi, uint32_t new_dpi) {
    if (old_dpi == 0) return c;
    auto f = [&](int32_t v) {
        return static_cast<int32_t>(std::lround(static_cast<double>(v) * new_dpi / old_dpi));
    };
    return {f(c.w), f(c.h)};
}

std::optional<float> parse_xft_dpi(std::string_view res) {
    constexpr std::string_view kKey = "Xft.dpi:";
    size_t pos = 0;
    while (pos < res.size()) {
        size_t eol = res.find('\n', pos);
        if (eol == std::string_view::npos) eol = res.size();
        std::string_view line = res.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.substr(0, kKey.size()) != kKey) continue;
        line.remove_prefix(kKey.size());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        const std::string num(line);
        char* end = nullptr;
        const double v = std::strtod(num.c_str(), &end);
        if (end == num.c_str() || !(v >= 24.0 && v <= 960.0)) return std::nullopt;
        return static_cast<float>(v);
    }
    return std::nullopt;
}

float x11_scale(std::string_view resources, const char* gdk_scale_env) {
    if (gdk_scale_env && *gdk_scale_env) {
        char* end = nullptr;
        const long v = std::strtol(gdk_scale_env, &end, 10);
        if (end != gdk_scale_env && *end == '\0' && v >= 1 && v <= 5) return static_cast<float>(v);
    }
    float s = 1.0f;
    if (auto dpi = parse_xft_dpi(resources)) s = *dpi / static_cast<float>(kBaseDpi);
    return std::clamp(s, 0.5f, 5.0f);
}

bool wheel_is_precise(int32_t delta) { return delta % 120 != 0; }

float wheel_lines(int32_t delta, uint32_t lines_per_notch) {
    return static_cast<float>(delta) / 120.0f * static_cast<float>(lines_per_notch);
}

}  // namespace kt::platform::detail
