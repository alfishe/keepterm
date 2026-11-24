// Geometry and color types of the UI toolkit. All geometry is in DIPs.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace kt::ui {

inline constexpr float kInf = std::numeric_limits<float>::infinity();

struct Vec2 {
    float x = 0, y = 0;
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    bool operator==(const Vec2&) const = default;
};

struct Size {
    float w = 0, h = 0;
    bool operator==(const Size&) const = default;
};

struct Insets {
    float l = 0, t = 0, r = 0, b = 0;
    static constexpr Insets all(float v) { return {v, v, v, v}; }
    static constexpr Insets hv(float h, float v) { return {h, v, h, v}; }
    float hsum() const { return l + r; }
    float vsum() const { return t + b; }
    bool operator==(const Insets&) const = default;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Vec2 origin() const { return {x, y}; }
    Size size() const { return {w, h}; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    Rect intersect(const Rect& o) const {
        const float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
        return {x0, y0, x1 - x0, y1 - y0};
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const float x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        const float x1 = std::max(right(), o.right()), y1 = std::max(bottom(), o.bottom());
        return {x0, y0, x1 - x0, y1 - y0};
    }
    Rect inset(const Insets& i) const { return {x + i.l, y + i.t, std::max(0.0f, w - i.hsum()), std::max(0.0f, h - i.vsum())}; }
    Rect inflate(float d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
    Rect translated(Vec2 d) const { return {x + d.x, y + d.y, w, h}; }
    bool operator==(const Rect&) const = default;
};

// Straight (not premultiplied) sRGB color, components 0..1. Premultiplied at paint time.
struct Color {
    float r = 0, g = 0, b = 0, a = 0;
    static constexpr Color rgb(uint32_t rgb, float a = 1.0f) {
        return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f, a};
    }
    Color with_alpha(float na) const { return {r, g, b, na}; }
    Color fade(float k) const { return {r, g, b, a * k}; }
    bool operator==(const Color&) const = default;
};

// a over b (both straight), opaque result when b is opaque.
inline Color over(Color a, Color b) {
    const float oa = a.a + b.a * (1 - a.a);
    if (oa <= 0) return {};
    auto ch = [&](float ca, float cb) { return (ca * a.a + cb * b.a * (1 - a.a)) / oa; };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), oa};
}

inline Color lerp(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// Premultiplied RGBA8 as render::pack_rgba packs it, with an extra opacity factor.
inline uint32_t to_render_color(Color c, float opacity = 1.0f) {
    const float a = std::clamp(c.a * opacity, 0.0f, 1.0f);
    auto q = [&](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * a * 255.0f)); };
    const uint32_t a8 = static_cast<uint32_t>(std::lround(a * 255.0f));
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (a8 << 24);
}

// WCAG relative luminance and contrast ratio.
float luminance(Color c);
float contrast(Color a, Color b);

using WidgetId = std::uint64_t;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::nanoseconds;

}  // namespace kt::ui
