#include "ui/icons.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kt::ui {

namespace {

constexpr float kPi = 3.14159265358979f;

struct Prim {
    enum Kind : uint8_t { seg, arc, dot, box, fill_box } kind;
    float a, b, c, d, e = 0, f = 0;   // seg: x0 y0 x1 y1; arc: cx cy r deg0 deg1; dot: cx cy r; box: x y w h r
};

using Shape = std::vector<Prim>;

Prim S(float x0, float y0, float x1, float y1) { return {Prim::seg, x0, y0, x1, y1}; }
Prim A(float cx, float cy, float r, float d0, float d1) { return {Prim::arc, cx, cy, r, d0, d1}; }
Prim C(float cx, float cy, float r) { return {Prim::arc, cx, cy, r, 0, 360}; }
Prim D(float cx, float cy, float r) { return {Prim::dot, cx, cy, r, 0}; }
Prim B(float x, float y, float w, float h, float r) { return {Prim::box, x, y, w, h, r}; }
Prim FB(float x, float y, float w, float h, float r) { return {Prim::fill_box, x, y, w, h, r}; }

void poly(Shape& s, std::initializer_list<std::array<float, 2>> pts, bool closed) {
    const std::array<float, 2>* prev = nullptr;
    for (const auto& p : pts) {
        if (prev) s.push_back(S((*prev)[0], (*prev)[1], p[0], p[1]));
        prev = &p;
    }
    if (closed && pts.size() > 2) s.push_back(S((*prev)[0], (*prev)[1], pts.begin()[0][0], pts.begin()[0][1]));
}

Shape shape_of(Icon i) {
    Shape s;
    switch (i) {
        case Icon::none:
        case Icon::count_: break;
        case Icon::folder: poly(s, {{3, 5.5f}, {9, 5.5f}, {11.5f, 8}, {21, 8}, {21, 19}, {3, 19}}, true); break;
        case Icon::terminal:
            s.push_back(B(3, 4, 18, 16, 2.5f));
            poly(s, {{7, 9}, {10, 12}, {7, 15}}, false);
            s.push_back(S(12, 15, 16.5f, 15));
            break;
        case Icon::plus: s = {S(12, 5, 12, 19), S(5, 12, 19, 12)}; break;
        case Icon::close: s = {S(6.5f, 6.5f, 17.5f, 17.5f), S(17.5f, 6.5f, 6.5f, 17.5f)}; break;
        case Icon::chevron_right: poly(s, {{9.5f, 6}, {15.5f, 12}, {9.5f, 18}}, false); break;
        case Icon::chevron_down: poly(s, {{6, 9.5f}, {12, 15.5f}, {18, 9.5f}}, false); break;
        case Icon::sidebar: s = {B(3, 4, 18, 16, 2.5f), S(9.5f, 4, 9.5f, 20)}; break;
        case Icon::split_right: s = {B(3, 4, 18, 16, 2.5f), S(12, 4, 12, 20)}; break;
        case Icon::split_down: s = {B(3, 4, 18, 16, 2.5f), S(3, 12, 21, 12)}; break;
        case Icon::zoom:
            poly(s, {{14.5f, 4}, {20, 4}, {20, 9.5f}}, false);
            s.push_back(S(20, 4, 14, 10));
            poly(s, {{9.5f, 20}, {4, 20}, {4, 14.5f}}, false);
            s.push_back(S(4, 20, 10, 14));
            break;
        case Icon::unzoom:
            poly(s, {{14, 4.5f}, {14, 10}, {19.5f, 10}}, false);
            s.push_back(S(14, 10, 20, 4));
            poly(s, {{10, 19.5f}, {10, 14}, {4.5f, 14}}, false);
            s.push_back(S(10, 14, 4, 20));
            break;
        case Icon::memory:
            s.push_back(B(4, 7.5f, 16, 9, 1.5f));
            for (float x : {8.0f, 12.0f, 16.0f}) {
                s.push_back(S(x, 3.5f, x, 7.5f));
                s.push_back(S(x, 16.5f, x, 20.5f));
            }
            break;
        case Icon::cpu:
            s.push_back(B(6, 6, 12, 12, 1.5f));
            s.push_back(FB(10, 10, 4, 4, 0.5f));
            for (float v : {9.0f, 12.0f, 15.0f}) {
                s.push_back(S(v, 3, v, 6));
                s.push_back(S(v, 18, v, 21));
                s.push_back(S(3, v, 6, v));
                s.push_back(S(18, v, 21, v));
            }
            break;
        case Icon::gear:
            s.push_back(C(12, 12, 3));
            s.push_back(C(12, 12, 7));
            for (int k = 0; k < 8; ++k) {
                const float a = static_cast<float>(k) * kPi / 4;
                s.push_back(S(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 9.5f * std::cos(a), 12 + 9.5f * std::sin(a)));
            }
            break;
        case Icon::search: s = {C(10.5f, 10.5f, 6), S(15, 15, 20, 20)}; break;
        case Icon::dot: s = {D(12, 12, 5)}; break;
        case Icon::agent:
            s = {S(12, 3.5f, 12, 20.5f), S(3.5f, 12, 20.5f, 12), S(6, 6, 18, 18), S(18, 6, 6, 18)};
            break;
        case Icon::spinner: s = {A(12, 12, 8, 0, 270)}; break;
        case Icon::check: poly(s, {{5, 12.5f}, {10, 17}, {19, 7}}, false); break;
        case Icon::alert: s = {C(12, 12, 9), S(12, 7.5f, 12, 12.5f), D(12, 16.2f, 1.4f)}; break;
        case Icon::storage: s = {B(3, 6, 18, 12, 2.5f), S(3, 13, 21, 13), D(17, 15.5f, 1.1f), D(13.5f, 15.5f, 1.1f)}; break;
        case Icon::bell:
            s.push_back(A(12, 11, 6, 180, 360));
            poly(s, {{6, 11}, {6, 15.5f}, {4, 17.5f}, {20, 17.5f}, {18, 15.5f}, {18, 11}}, false);
            s.push_back(S(10, 20.5f, 14, 20.5f));
            break;
        case Icon::pin:
            poly(s, {{8.5f, 3.5f}, {15.5f, 3.5f}, {15, 9}, {18, 13}, {6, 13}, {9, 9}}, true);
            s.push_back(S(12, 13, 12, 20.5f));
            break;
        case Icon::history: s = {C(12, 12, 8.5f), S(12, 7, 12, 12), S(12, 12, 15.5f, 14)}; break;
        case Icon::more: s = {D(6, 12, 1.7f), D(12, 12, 1.7f), D(18, 12, 1.7f)}; break;
        case Icon::copy:
            s.push_back(B(8, 8, 12, 12, 2));
            poly(s, {{16, 8}, {16, 4}, {4, 4}, {4, 16}, {8, 16}}, false);
            break;
        case Icon::menu: s = {S(4, 7, 20, 7), S(4, 12, 20, 12), S(4, 17, 20, 17)}; break;
        case Icon::minimize: s = {S(5, 12, 19, 12)}; break;
        case Icon::maximize: s = {B(5, 5, 14, 14, 1)}; break;
        case Icon::palette: s = {C(12, 12, 9), D(8, 10, 1.4f), D(12, 7.5f, 1.4f), D(16, 10, 1.4f), D(14.5f, 15, 1.4f)}; break;
        case Icon::trash:
            s.push_back(S(4, 7, 20, 7));
            poly(s, {{6, 7}, {7, 20}, {17, 20}, {18, 7}}, false);
            poly(s, {{9, 7}, {9, 4}, {15, 4}, {15, 7}}, false);
            break;
        case Icon::edit: poly(s, {{4, 20}, {4, 16}, {15, 5}, {19, 9}, {8, 20}}, true); break;
        case Icon::keyboard:
            s.push_back(B(2.5f, 6, 19, 12, 2));
            for (float x : {6.5f, 10.0f, 13.5f, 17.0f}) s.push_back(D(x, 10, 0.9f));
            s.push_back(S(7.5f, 14.5f, 16.5f, 14.5f));
            break;
        case Icon::info: s = {C(12, 12, 9), S(12, 11, 12, 16.5f), D(12, 7.8f, 1.4f)}; break;
    }
    return s;
}

float dist_seg(float px, float py, const Prim& p) {
    const float dx = p.c - p.a, dy = p.d - p.b;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - p.a) * dx + (py - p.b) * dy) / l2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = p.a + t * dx - px, ey = p.b + t * dy - py;
    return std::sqrt(ex * ex + ey * ey);
}

float dist_arc(float px, float py, const Prim& p) {
    const float dx = px - p.a, dy = py - p.b;
    const float r = std::sqrt(dx * dx + dy * dy);
    if (p.e - p.d >= 360) return std::abs(r - p.c);
    float ang = std::atan2(dy, dx) * 180 / kPi;
    if (ang < 0) ang += 360;
    if (ang >= p.d && ang <= p.e) return std::abs(r - p.c);
    float best = 1e9f;
    for (float a : {p.d, p.e}) {
        const float ex = p.a + p.c * std::cos(a * kPi / 180) - px, ey = p.b + p.c * std::sin(a * kPi / 180) - py;
        best = std::min(best, std::sqrt(ex * ex + ey * ey));
    }
    return best;
}

// Signed distance to a rounded box (negative inside).
float sd_box(float px, float py, const Prim& p) {
    const float hw = p.c / 2, hh = p.d / 2, r = std::min(p.e, std::min(hw, hh));
    const float qx = std::abs(px - (p.a + hw)) - (hw - r), qy = std::abs(py - (p.b + hh)) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

}  // namespace

std::string_view icon_name(Icon i) {
    static constexpr std::string_view names[] = {
        "none", "folder", "terminal", "plus", "close", "chevron_right", "chevron_down", "sidebar", "split_right",
        "split_down", "zoom", "unzoom", "memory", "cpu", "gear", "search", "dot", "agent", "spinner", "check",
        "alert", "storage", "bell", "pin", "more", "copy", "menu", "minimize", "maximize", "palette", "trash",
        "edit", "keyboard", "info", "history"};
    static_assert(std::size(names) == static_cast<size_t>(Icon::count_));
    const auto k = static_cast<size_t>(i);
    return k < std::size(names) ? names[k] : "none";
}

Icon icon_from_name(std::string_view n) {
    for (size_t k = 0; k < static_cast<size_t>(Icon::count_); ++k) {
        if (icon_name(static_cast<Icon>(k)) == n) return static_cast<Icon>(k);
    }
    return Icon::none;
}

std::vector<uint8_t> rasterize_icon(Icon icon, int px) {
    px = std::clamp(px, 4, 256);
    std::vector<uint8_t> out(static_cast<size_t>(px) * static_cast<size_t>(px), 0);
    const Shape s = shape_of(icon);
    const float unit = 24.0f / static_cast<float>(px);          // grid units per pixel
    const float hw = std::max(1.0f, 0.62f * unit);              // half stroke: 2 units, at least ~1.25 px
    for (int y = 0; y < px; ++y) {
        for (int x = 0; x < px; ++x) {
            const float gx = (static_cast<float>(x) + 0.5f) * unit, gy = (static_cast<float>(y) + 0.5f) * unit;
            float cov = 0;
            for (const Prim& p : s) {
                float d;   // signed distance to the painted area, grid units
                switch (p.kind) {
                    case Prim::seg: d = dist_seg(gx, gy, p) - hw; break;
                    case Prim::arc: d = dist_arc(gx, gy, p) - hw; break;
                    case Prim::dot: d = std::sqrt((gx - p.a) * (gx - p.a) + (gy - p.b) * (gy - p.b)) - p.c; break;
                    case Prim::box: d = std::abs(sd_box(gx, gy, p)) - hw; break;
                    case Prim::fill_box: d = sd_box(gx, gy, p); break;
                    default: d = 1e9f;
                }
                cov = std::max(cov, std::clamp(0.5f - d / unit, 0.0f, 1.0f));
                if (cov >= 1) break;
            }
            out[static_cast<size_t>(y) * static_cast<size_t>(px) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::lround(cov * 255.0f));
        }
    }
    return out;
}

}  // namespace kt::ui
