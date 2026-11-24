// Procedural glyphs: box drawing U+2500..U+257F, block elements U+2580..U+259F,
// and the tofu box. Axis-aligned strokes sit on integer pixel edges and are filled
// directly (FreeType's rasterizer gives the same pixels for such rectangles); arcs and diagonals
// go through FreeType's outline rasterizer (FT_Outline_Get_Bitmap, anti-aliased).
#include "text/raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <vector>

#include FT_BITMAP_H
#include FT_OUTLINE_H

namespace kt::text::detail {

namespace {

struct Span {
    int a, b;
};

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(static_cast<size_t>(w) * static_cast<size_t>(h), 0) {}
    int w() const { return w_; }
    int h() const { return h_; }

    void fill(int x0, int y0, int x1, int y1, uint8_t v = 255) {
        x0 = std::clamp(x0, 0, w_);
        x1 = std::clamp(x1, 0, w_);
        y0 = std::clamp(y0, 0, h_);
        y1 = std::clamp(y1, 0, h_);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                uint8_t& p = px_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)];
                p = std::max(p, v);
            }
        }
    }
    void merge(const std::vector<uint8_t>& other) {
        for (size_t i = 0; i < px_.size(); ++i) px_[i] = std::max(px_[i], other[i]);
    }
    std::vector<uint8_t>& pixels() { return px_; }

private:
    int w_, h_;
    std::vector<uint8_t> px_;
};

// Polygons in cell pixel coordinates (y down), rendered with FreeType in one outline so that
// overlapping parts unite exactly (non-zero winding, every contour with the same orientation).
class Path {
public:
    struct P {
        double x, y;
    };
    void polygon(std::vector<P> pts) {
        if (pts.size() < 3) return;
        double area = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            const P& a = pts[i];
            const P& b = pts[(i + 1) % pts.size()];
            area += a.x * b.y - b.x * a.y;
        }
        if (area < 0) std::reverse(pts.begin(), pts.end());
        for (const P& p : pts) points_.push_back(p);
        ends_.push_back(points_.size() - 1);
    }
    void rect(double x0, double y0, double x1, double y1) { polygon({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}); }
    bool empty() const { return points_.empty(); }

    std::vector<uint8_t> render(FT_Library lib, int w, int h) const {
        std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
        if (points_.empty()) return out;
        std::vector<FT_Vector> pts(points_.size());
        FT_Outline outline{};
        using Tag = std::remove_pointer_t<decltype(outline.tags)>;
        std::vector<Tag> tags(points_.size(), static_cast<Tag>(FT_CURVE_TAG_ON));
        for (size_t i = 0; i < points_.size(); ++i) {
            pts[i].x = static_cast<FT_Pos>(std::lround(points_[i].x * 64.0));
            pts[i].y = static_cast<FT_Pos>(std::lround((h - points_[i].y) * 64.0));
        }
        std::vector<std::remove_pointer_t<decltype(outline.contours)>> contours;
        for (size_t e : ends_) contours.push_back(static_cast<std::remove_pointer_t<decltype(outline.contours)>>(e));
        outline.n_contours = static_cast<decltype(outline.n_contours)>(contours.size());
        outline.n_points = static_cast<decltype(outline.n_points)>(pts.size());
        outline.points = pts.data();
        outline.tags = tags.data();
        outline.contours = contours.data();
        outline.flags = FT_OUTLINE_NONE;

        FT_Bitmap bm;
        FT_Bitmap_Init(&bm);
        bm.rows = static_cast<unsigned>(h);
        bm.width = static_cast<unsigned>(w);
        bm.pitch = w;
        bm.buffer = out.data();
        bm.pixel_mode = FT_PIXEL_MODE_GRAY;
        bm.num_grays = 256;
        FT_Outline_Get_Bitmap(lib, &outline, &bm);
        return out;
    }

private:
    std::vector<P> points_;
    std::vector<size_t> ends_;
};

// "LRUD" weights per box-drawing code point: 0 none, 1 light, 2 heavy, 3 double.
constexpr const char* kLines2500[0x4C] = {
    "1100", "2200", "0011", "0022", "", "", "", "", "", "", "", "",                  // 2500..250B
    "0101", "0201", "0102", "0202", "1001", "2001", "1002", "2002",                  // 250C..2513
    "0110", "0210", "0120", "0220", "1010", "2010", "1020", "2020",                  // 2514..251B
    "0111", "0211", "0121", "0112", "0122", "0221", "0212", "0222",                  // 251C..2523
    "1011", "2011", "1021", "1012", "1022", "2021", "2012", "2022",                  // 2524..252B
    "1101", "2101", "1201", "2201", "1102", "2102", "1202", "2202",                  // 252C..2533
    "1110", "2110", "1210", "2210", "1120", "2120", "1220", "2220",                  // 2534..253B
    "1111", "2111", "1211", "2211", "1121", "1112", "1122", "2121",                  // 253C..2543
    "1221", "2112", "1212", "2221", "2212", "2122", "1222", "2222",                  // 2544..254B
};
constexpr const char* kLines2550[0x1D] = {
    "3300", "0033", "0301", "0103", "0303", "3001", "1003", "3003",                  // 2550..2557
    "0310", "0130", "0330", "3010", "1030", "3030", "0311", "0133",                  // 2558..255F
    "0333", "3011", "1033", "3033", "3301", "1103", "3303", "3310",                  // 2560..2567
    "1130", "3330", "3311", "1133", "3333",                                          // 2568..256C
};
constexpr const char* kLines2574[0x0C] = {
    "1000", "0010", "0100", "0001", "2000", "0020", "0200", "0002", "1200", "0012", "2100", "0021",
};

const char* line_spec(char32_t cp) {
    if (cp >= 0x2500 && cp < 0x254C) return kLines2500[cp - 0x2500];
    if (cp >= 0x2550 && cp < 0x256D) return kLines2550[cp - 0x2550];
    if (cp >= 0x2574 && cp < 0x2580) return kLines2574[cp - 0x2574];
    return "";
}

struct Geometry {
    int w, h, light, heavy;
    int thick(int weight) const { return weight == 2 ? heavy : light; }
    Span vs(int t) const { int x0 = (w - t) / 2; return {x0, x0 + t}; }
    Span hs(int t) const { int y0 = (h - t) / 2; return {y0, y0 + t}; }
    Span vl() const { int x0 = (w - 3 * light) / 2; return {x0, x0 + light}; }
    Span vr() const { int x0 = (w - 3 * light) / 2 + 2 * light; return {x0, x0 + light}; }
    Span ht() const { int y0 = (h - 3 * light) / 2; return {y0, y0 + light}; }
    Span hb() const { int y0 = (h - 3 * light) / 2 + 2 * light; return {y0, y0 + light}; }
};

void draw_lines(Canvas& c, const Geometry& g, const char* spec) {
    const int L = spec[0] - '0', R = spec[1] - '0', U = spec[2] - '0', D = spec[3] - '0';
    auto ext = [&](int weight, bool vertical) -> Span {
        if (weight == 3) return vertical ? Span{g.vl().a, g.vr().b} : Span{g.ht().a, g.hb().b};
        return vertical ? g.vs(g.thick(weight)) : g.hs(g.thick(weight));
    };
    auto unite = [](Span a, Span b) { return Span{std::min(a.a, b.a), std::max(a.b, b.b)}; };
    Span vext{g.w, 0}, hext{g.h, 0};
    if (U) vext = unite(vext, ext(U, true));
    if (D) vext = unite(vext, ext(D, true));
    if (L) hext = unite(hext, ext(L, false));
    if (R) hext = unite(hext, ext(R, false));
    const bool vert = U || D, horz = L || R;
    const bool vdbl = U == 3 || D == 3, hdbl = L == 3 || R == 3;
    const bool vthrough = U == 3 && D == 3, hthrough = L == 3 && R == 3;
    auto single_t = [&](int a, int b) { return std::max(a && a < 3 ? g.thick(a) : 0, b && b < 3 ? g.thick(b) : 0); };

    // Left arm.
    if (L == 3) {
        for (int k = 0; k < 2; ++k) {
            const Span s = k == 0 ? g.ht() : g.hb();
            int end;
            if (vdbl) end = (k == 0 ? U == 3 : D == 3) ? g.vl().b : g.vr().b;
            else if (vert) end = vext.b;
            else end = R ? g.w : g.vr().b;
            c.fill(0, s.a, end, s.b);
        }
    } else if (L) {
        const Span s = g.hs(g.thick(L));
        int end;
        if (vert) end = (vthrough && !R) ? g.vl().b : vext.b;
        else end = g.vs(single_t(L, R)).b;
        c.fill(0, s.a, end, s.b);
    }
    // Right arm.
    if (R == 3) {
        for (int k = 0; k < 2; ++k) {
            const Span s = k == 0 ? g.ht() : g.hb();
            int start;
            if (vdbl) start = (k == 0 ? U == 3 : D == 3) ? g.vr().a : g.vl().a;
            else if (vert) start = vext.a;
            else start = L ? 0 : g.vl().a;
            c.fill(start, s.a, g.w, s.b);
        }
    } else if (R) {
        const Span s = g.hs(g.thick(R));
        int start;
        if (vert) start = (vthrough && !L) ? g.vr().a : vext.a;
        else start = g.vs(single_t(L, R)).a;
        c.fill(start, s.a, g.w, s.b);
    }
    // Up arm.
    if (U == 3) {
        for (int k = 0; k < 2; ++k) {
            const Span s = k == 0 ? g.vl() : g.vr();
            int end;
            if (hdbl) end = (k == 0 ? L == 3 : R == 3) ? g.ht().b : g.hb().b;
            else if (horz) end = hext.b;
            else end = D ? g.h : g.hb().b;
            c.fill(s.a, 0, s.b, end);
        }
    } else if (U) {
        const Span s = g.vs(g.thick(U));
        int end;
        if (horz) end = (hthrough && !D) ? g.ht().b : hext.b;
        else end = g.hs(single_t(U, D)).b;
        c.fill(s.a, 0, s.b, end);
    }
    // Down arm.
    if (D == 3) {
        for (int k = 0; k < 2; ++k) {
            const Span s = k == 0 ? g.vl() : g.vr();
            int start;
            if (hdbl) start = (k == 0 ? L == 3 : R == 3) ? g.hb().a : g.ht().a;
            else if (horz) start = hext.a;
            else start = U ? 0 : g.ht().a;
            c.fill(s.a, start, s.b, g.h);
        }
    } else if (D) {
        const Span s = g.vs(g.thick(D));
        int start;
        if (horz) start = (hthrough && !U) ? g.hb().a : hext.a;
        else start = g.hs(single_t(U, D)).a;
        c.fill(s.a, start, s.b, g.h);
    }
}

void draw_dashes(Canvas& c, const Geometry& g, bool vertical, int weight, int n) {
    const int t = g.thick(weight);
    const int len = vertical ? g.h : g.w;
    for (int i = 0; i < n; ++i) {
        const int s0 = (i * len) / n, s1 = ((i + 1) * len) / n;
        const int gap = std::max(1, (s1 - s0 + 2) / 4);
        const int a = s0 + gap / 2, b = s1 - (gap - gap / 2);
        if (b <= a) continue;
        if (vertical) {
            const Span x = g.vs(t);
            c.fill(x.a, a, x.b, b);
        } else {
            const Span y = g.hs(t);
            c.fill(a, y.a, b, y.b);
        }
    }
}

void draw_arc(Path& p, const Geometry& g, int sx, int sy) {
    const double t = g.light;
    const Span vx = g.vs(g.light), hy = g.hs(g.light);
    const double cx = vx.a + t / 2.0, cy = hy.a + t / 2.0;
    const double r = std::max(t, std::min({cx, g.w - cx, cy, g.h - cy}));
    const double ox = cx + sx * r, oy = cy + sy * r;   // arc center
    const double ro = r + t / 2.0, ri = std::max(0.0, r - t / 2.0);
    constexpr int kSteps = 24;
    constexpr double kHalfPi = 1.57079632679489661923;
    std::vector<Path::P> pts;
    for (int i = 0; i <= kSteps; ++i) {
        const double phi = kHalfPi * i / kSteps;
        pts.push_back({ox - sx * ro * std::cos(phi), oy - sy * ro * std::sin(phi)});
    }
    for (int i = kSteps; i >= 0; --i) {
        const double phi = kHalfPi * i / kSteps;
        pts.push_back({ox - sx * ri * std::cos(phi), oy - sy * ri * std::sin(phi)});
    }
    p.polygon(std::move(pts));
    // Straight parts from the arc ends to the cell edges.
    const double ex = cx + sx * r;   // horizontal part starts here
    if (sx > 0) p.rect(ex, hy.a, g.w, hy.b);
    else p.rect(0, hy.a, ex, hy.b);
    const double ey = cy + sy * r;   // vertical part starts here
    if (sy > 0) p.rect(vx.a, ey, vx.b, g.h);
    else p.rect(vx.a, 0, vx.b, ey);
}

void draw_diagonal(Path& p, const Geometry& g, bool rising) {
    // rising: lower left to upper right (U+2571); else upper left to lower right (U+2572).
    const double x0 = 0, x1 = g.w;
    const double y0 = rising ? g.h : 0, y1 = rising ? 0 : g.h;
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    const double ux = dx / len, uy = dy / len;
    const double nx = -uy * g.light / 2.0, ny = ux * g.light / 2.0;
    const double ex = ux * g.light, ey = uy * g.light;   // overshoot, clipped by the cell
    p.polygon({{x0 - ex + nx, y0 - ey + ny},
               {x1 + ex + nx, y1 + ey + ny},
               {x1 + ex - nx, y1 + ey - ny},
               {x0 - ex - nx, y0 - ey - ny}});
}

void draw_block(Canvas& c, char32_t cp) {
    const int w = c.w(), h = c.h();
    auto ey = [&](int k) { return static_cast<int>(std::lround(k * h / 8.0)); };   // eighth boundaries
    auto ex = [&](int k) { return static_cast<int>(std::lround(k * w / 8.0)); };
    const int mx = ex(4), my = ey(4);
    switch (cp) {
        case 0x2580: c.fill(0, 0, w, my); break;
        case 0x2588: c.fill(0, 0, w, h); break;
        case 0x2590: c.fill(mx, 0, w, h); break;
        case 0x2591: c.fill(0, 0, w, h, 64); break;
        case 0x2592: c.fill(0, 0, w, h, 128); break;
        case 0x2593: c.fill(0, 0, w, h, 191); break;
        case 0x2594: c.fill(0, 0, w, ey(1)); break;
        case 0x2595: c.fill(ex(7), 0, w, h); break;
        default: break;
    }
    if (cp >= 0x2581 && cp <= 0x2587) c.fill(0, ey(8 - static_cast<int>(cp - 0x2580)), w, h);   // lower k/8
    if (cp >= 0x2589 && cp <= 0x258F) c.fill(0, 0, ex(8 - static_cast<int>(cp - 0x2588)), h);   // left k/8
    // Quadrants: bit 0 upper left, 1 upper right, 2 lower left, 3 lower right.
    static constexpr uint8_t kQuad[10] = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};
    if (cp >= 0x2596 && cp <= 0x259F) {
        const uint8_t q = kQuad[cp - 0x2596];
        if (q & 1) c.fill(0, 0, mx, my);
        if (q & 2) c.fill(mx, 0, w, my);
        if (q & 4) c.fill(0, my, mx, h);
        if (q & 8) c.fill(mx, my, w, h);
    }
}

// 3x5 hex digits for the tofu box, rows top to bottom, 3 bits each (MSB = left).
constexpr uint16_t kHexDigits[16] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
    0x7BEF, 0x7BCF, 0x7BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4,
};

}  // namespace

bool is_procedural(char32_t cp) { return cp >= 0x2500 && cp <= 0x259F; }

uint32_t procedural_variant(const CellMetrics& m, uint8_t span) {
    return (static_cast<uint32_t>(m.cell_w) & 0x3FFu) | ((static_cast<uint32_t>(m.cell_h) & 0x3FFu) << 10) |
           ((static_cast<uint32_t>(m.box_light) & 0x3Fu) << 20) | ((uint32_t{span} & 0x3Fu) << 26);
}

Result<GlyphBitmap> rasterize_procedural(FT_Library lib, char32_t cp, const CellMetrics& m) {
    if (!is_procedural(cp)) return make_error(Errc::invalid_argument, "not a procedural glyph");
    if (m.cell_w <= 0 || m.cell_h <= 0 || m.cell_w > 1000 || m.cell_h > 1000)
        return make_error(Errc::invalid_argument, "bad cell metrics");
    Canvas c(m.cell_w, m.cell_h);
    const Geometry g{m.cell_w, m.cell_h, std::max(1, m.box_light), std::max(1, m.box_heavy)};
    Path path;
    if (cp >= 0x2580) {
        draw_block(c, cp);
    } else if (const char* spec = line_spec(cp); spec[0] != '\0') {
        draw_lines(c, g, spec);
    } else {
        switch (cp) {
            case 0x2504: draw_dashes(c, g, false, 1, 3); break;
            case 0x2505: draw_dashes(c, g, false, 2, 3); break;
            case 0x2506: draw_dashes(c, g, true, 1, 3); break;
            case 0x2507: draw_dashes(c, g, true, 2, 3); break;
            case 0x2508: draw_dashes(c, g, false, 1, 4); break;
            case 0x2509: draw_dashes(c, g, false, 2, 4); break;
            case 0x250A: draw_dashes(c, g, true, 1, 4); break;
            case 0x250B: draw_dashes(c, g, true, 2, 4); break;
            case 0x254C: draw_dashes(c, g, false, 1, 2); break;
            case 0x254D: draw_dashes(c, g, false, 2, 2); break;
            case 0x254E: draw_dashes(c, g, true, 1, 2); break;
            case 0x254F: draw_dashes(c, g, true, 2, 2); break;
            case 0x256D: draw_arc(path, g, +1, +1); break;   // down and right
            case 0x256E: draw_arc(path, g, -1, +1); break;   // down and left
            case 0x256F: draw_arc(path, g, -1, -1); break;   // up and left
            case 0x2570: draw_arc(path, g, +1, -1); break;   // up and right
            case 0x2571: draw_diagonal(path, g, true); break;
            case 0x2572: draw_diagonal(path, g, false); break;
            case 0x2573:
                draw_diagonal(path, g, true);
                draw_diagonal(path, g, false);
                break;
            default: break;
        }
    }
    if (!path.empty()) c.merge(path.render(lib, m.cell_w, m.cell_h));

    GlyphBitmap out;
    out.w = static_cast<uint16_t>(m.cell_w);
    out.h = static_cast<uint16_t>(m.cell_h);
    out.bearing_x = 0;
    out.bearing_y = static_cast<int16_t>(m.baseline);
    out.format = PageFormat::a8;
    out.pixels = std::move(c.pixels());
    return out;
}

GlyphBitmap rasterize_tofu(char32_t cp, uint8_t span, const CellMetrics& m) {
    const int w = std::max(1, m.cell_w * std::max<int>(1, span));
    const int h = std::max(1, m.cell_h);
    Canvas c(w, h);
    const int t = std::max(1, m.box_light);
    const int inset = w >= 6 ? 1 : 0;
    // Frame.
    c.fill(inset, inset, w - inset, inset + t);
    c.fill(inset, h - inset - t, w - inset, h - inset);
    c.fill(inset, inset, inset + t, h - inset);
    c.fill(w - inset - t, inset, w - inset, h - inset);
    // Hex code point: 2x2 digits up to U+FFFF, 3x2 above.
    const int n = cp > 0xFFFF ? 6 : 4, cols = n / 2;
    const int in_w = w - 2 * (inset + t + 1), in_h = h - 2 * (inset + t + 1);
    const int s = std::min((in_w + 1) / (cols * 4), (in_h + 1) / 12);
    if (s >= 1) {
        const int tw = (cols * 4 - 1) * s, th = 11 * s;
        const int ox = (w - tw) / 2, oy = (h - th) / 2;
        for (int i = 0; i < n; ++i) {
            const uint32_t nibble = (static_cast<uint32_t>(cp) >> (4 * (n - 1 - i))) & 0xFu;
            const uint16_t bits = kHexDigits[nibble];
            const int gx = ox + (i % cols) * 4 * s, gy = oy + (i / cols) * 6 * s;
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if (bits & (1u << (14 - (row * 3 + col))))
                        c.fill(gx + col * s, gy + row * s, gx + (col + 1) * s, gy + (row + 1) * s);
                }
            }
        }
    }
    GlyphBitmap out;
    out.w = static_cast<uint16_t>(w);
    out.h = static_cast<uint16_t>(h);
    out.bearing_x = 0;
    out.bearing_y = static_cast<int16_t>(m.baseline);
    out.format = PageFormat::a8;
    out.pixels = std::move(c.pixels());
    return out;
}

}  // namespace kt::text::detail
