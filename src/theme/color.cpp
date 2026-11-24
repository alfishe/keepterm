#include "theme/color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kt::theme {

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<uint64_t> hex_value(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) {
        const int d = hex_digit(c);
        if (d < 0) return std::nullopt;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v;
}

double linear(uint32_t channel) {
    const double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

}  // namespace

std::optional<Rgb> parse_rgb(std::string_view s) {
    if (s.starts_with('#')) s.remove_prefix(1);
    if (s.size() == 3) {
        auto v = hex_value(s);
        if (!v) return std::nullopt;
        const auto r = static_cast<uint32_t>((*v >> 8) & 0xF), g = static_cast<uint32_t>((*v >> 4) & 0xF),
                   b = static_cast<uint32_t>(*v & 0xF);
        return (r * 17u << 16) | (g * 17u << 8) | (b * 17u);
    }
    if (s.size() != 6) return std::nullopt;
    auto v = hex_value(s);
    if (!v) return std::nullopt;
    return static_cast<Rgb>(*v);
}

std::optional<Rgba> parse_rgba(std::string_view s) {
    std::string_view h = s;
    if (h.starts_with('#')) h.remove_prefix(1);
    if (h.size() == 8) {
        auto v = hex_value(h);
        if (!v) return std::nullopt;
        return static_cast<Rgba>(*v);
    }
    auto rgb = parse_rgb(s);
    if (!rgb) return std::nullopt;
    return (*rgb << 8) | 0xFFu;
}

std::string format_rgb(Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%06x", c & 0xFFFFFFu);
    return buf;
}

std::string format_rgba(Rgba c) {
    if ((c & 0xFFu) == 0xFFu) return format_rgb(c >> 8);
    char buf[10];
    std::snprintf(buf, sizeof buf, "#%08x", c);
    return buf;
}

double luminance(Rgb c) {
    return 0.2126 * linear((c >> 16) & 0xFF) + 0.7152 * linear((c >> 8) & 0xFF) + 0.0722 * linear(c & 0xFF);
}

double contrast(Rgb a, Rgb b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Rgb mix(Rgb a, Rgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    Rgb out = 0;
    for (int s = 0; s <= 16; s += 8) {
        const double ca = static_cast<double>((a >> s) & 0xFF), cb = static_cast<double>((b >> s) & 0xFF);
        out |= static_cast<Rgb>(std::lround(ca + (cb - ca) * t)) << s;
    }
    return out;
}

Rgb ensure_contrast(Rgb fg, Rgb bg, double ratio) {
    if (ratio <= 1.0 || contrast(fg, bg) >= ratio) return fg;
    // Toward the end that can reach more contrast; bisect the mix amount.
    const Rgb target = contrast(0xFFFFFF, bg) >= contrast(0x000000, bg) ? 0xFFFFFFu : 0x000000u;
    if (contrast(target, bg) < ratio) return target;
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 16; ++i) {
        const double mid = (lo + hi) / 2;
        if (contrast(mix(fg, target, mid), bg) >= ratio) hi = mid;
        else lo = mid;
    }
    return mix(fg, target, hi);
}

}  // namespace kt::theme
