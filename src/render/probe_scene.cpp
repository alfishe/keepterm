#include "render/probe_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kt::render {

namespace {

Quad quad(QuadKind kind, float x, float y, float w, float h, uint32_t color, uint32_t color2 = 0) {
    Quad q{};
    q.x = x;
    q.y = y;
    q.w = w;
    q.h = h;
    q.color = color;
    q.color2 = color2;
    q.kind = static_cast<uint8_t>(kind);
    return q;
}

Quad textured(QuadKind kind, float x, float y, uint16_t u0, uint16_t v0, uint16_t w, uint16_t h, uint32_t color) {
    Quad q = quad(kind, x, y, w, h, color);
    q.u0 = u0;
    q.v0 = v0;
    q.u1 = static_cast<uint16_t>(u0 + w);
    q.v1 = static_cast<uint16_t>(v0 + h);
    return q;
}

// 64 x 32 coverage atlas: a soft disc (0..31) and a ramp with hard steps (32..63).
std::vector<uint8_t> make_a8() {
    std::vector<uint8_t> px(64 * 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float d = std::sqrt(static_cast<float>((x - 16) * (x - 16) + (y - 16) * (y - 16)));
            const float c = std::clamp(13.0f - d, 0.0f, 1.0f) * 255.0f;
            px[static_cast<size_t>(y * 64 + x)] = static_cast<uint8_t>(std::lround(c));
            px[static_cast<size_t>(y * 64 + 32 + x)] = static_cast<uint8_t>(((x * 8) ^ (y * 3)) & 0xFF);
        }
    }
    return px;
}

// 16 x 16 premultiplied BGRA image: four colored quadrants with a translucent one.
std::vector<uint32_t> make_bgra() {
    std::vector<uint32_t> px(16 * 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint32_t b = 0, g = 0, r = 0, a = 255;
            if (x < 8 && y < 8) r = 230;
            else if (x >= 8 && y < 8) g = 200;
            else if (x < 8) b = 220;
            else {
                a = 128;
                r = g = b = 100;   // premultiplied: <= alpha
            }
            px[static_cast<size_t>(y * 16 + x)] = b | g << 8 | r << 16 | a << 24;   // BGRA in memory (little endian)
        }
    }
    return px;
}

// 16 x 16 LCD coverage (RGBA, alpha = gray coverage).
std::vector<uint32_t> make_lcd() {
    std::vector<uint32_t> px(16 * 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uint32_t c = static_cast<uint32_t>(std::min(255, (x + y) * 9));
            px[static_cast<size_t>(y * 16 + x)] = c | c << 8 | c << 16 | c << 24;
        }
    }
    return px;
}

}  // namespace

Result<void> draw_probe_scene(IRenderer& r) {
    auto a8 = r.create_texture(TextureDesc{64, 32, TexFormat::a8, false});
    if (!a8) return Unexpected<Error>(a8.error());
    auto img = r.create_texture(TextureDesc{16, 16, TexFormat::bgra8_premul, false});
    if (!img) return Unexpected<Error>(img.error());
    auto lcd = r.create_texture(TextureDesc{16, 16, TexFormat::rgba8_lcd, false});
    if (!lcd) return Unexpected<Error>(lcd.error());
    const auto a8_px = make_a8();
    const auto img_px = make_bgra();
    const auto lcd_px = make_lcd();
    r.update_texture(*a8, IRect{0, 0, 64, 32}, a8_px.data(), 64);
    r.update_texture(*img, IRect{0, 0, 16, 16}, img_px.data(), 64);
    r.update_texture(*lcd, IRect{0, 0, 16, 16}, lcd_px.data(), 64);

    const float W = static_cast<float>(kProbeSceneSize.w), H = static_cast<float>(kProbeSceneSize.h);
    DrawList dl;
    dl.quad(solid_quad(0, 0, W, H, rgba(24, 26, 33)));
    dl.quad(solid_quad(128, 0, 128, 80, rgba(236, 232, 220)));   // light panel for dark text

    // Gradients.
    dl.quad(quad(QuadKind::gradient, 4, 4, 56, 30, rgba(200, 40, 40), rgba(40, 40, 200)));
    Quad gh = quad(QuadKind::gradient, 64, 4, 56, 30, rgba(250, 220, 30), rgba(20, 160, 90));
    gh.flags = kQuadGradientHorizontal;
    dl.quad(gh);

    // Fractional solids and translucent overlays.
    dl.quad(solid_quad(4.5f, 40.25f, 20.5f, 10.75f, rgba(90, 200, 250)));
    dl.quad(solid_quad(30.3f, 40.6f, 0.7f, 12.0f, rgba(250, 250, 250)));
    dl.quad(solid_quad(36, 38, 30, 20, rgba(0, 0, 0, 0) /* transparent */));
    dl.quad(solid_quad(40, 44, 40, 16, rgba(64, 128, 32, 128)));   // premultiplied, 50 %

    // Coverage glyphs: light on dark, dark on light, several colors (luminance buckets).
    const uint32_t fg[] = {rgba(230, 230, 230), rgba(240, 90, 80), rgba(120, 220, 110), rgba(90, 150, 255)};
    for (int i = 0; i < 4; ++i) {
        dl.quad(textured(QuadKind::glyph_a8, 4.0f + 30.0f * static_cast<float>(i), 64, 0, 0, 32, 32, fg[i]), *a8);
        dl.quad(textured(QuadKind::glyph_a8, 132.0f + 30.0f * static_cast<float>(i), 8, 0, 0, 32, 32, rgba(static_cast<uint8_t>(30 + 40 * i), 30, 40)),
                *a8);
    }
    dl.quad(textured(QuadKind::glyph_a8, 132, 44, 32, 0, 32, 32, rgba(20, 20, 20)), *a8);
    dl.quad(textured(QuadKind::glyph_a8, 4, 100, 32, 0, 32, 32, rgba(250, 250, 250)), *a8);
    dl.quad(textured(QuadKind::glyph_lcd, 170, 50, 0, 0, 16, 16, rgba(10, 10, 60)), *lcd);
    dl.quad(textured(QuadKind::glyph_lcd, 40, 104, 0, 0, 16, 16, rgba(250, 230, 120)), *lcd);

    // Images: full and half opacity.
    dl.quad(textured(QuadKind::image, 64, 100, 0, 0, 16, 16, 0), *img);
    Quad half = textured(QuadKind::image, 84, 100, 0, 0, 16, 16, 0);
    half.param = 0.5f;
    dl.quad(half, *img);
    Quad big = textured(QuadKind::image, 196, 50, 0, 0, 16, 16, 0);
    dl.quad(big, *img);

    // Rounded rectangles: fill, border only, fill + border; a shadow under a card.
    Quad sh = quad(QuadKind::shadow, 140, 100, 50, 30, rgba(0, 0, 0, 160));
    sh.radius = 6;
    sh.param = 4;
    dl.quad(sh);
    Quad card = quad(QuadKind::rounded, 140, 98, 50, 30, rgba(70, 76, 96));
    card.radius = 6;
    dl.quad(card);
    Quad border = quad(QuadKind::rounded, 200, 96, 48, 26, 0, rgba(250, 200, 60));
    border.flags = kQuadBorderOnly;
    border.radius = 8;
    border.param = 2;
    dl.quad(border);
    Quad both = quad(QuadKind::rounded, 200.5f, 128.5f, 47, 26, rgba(40, 120, 200, 200), rgba(255, 255, 255));
    both.radius = 10;
    both.param = 1.5f;
    dl.quad(both);

    // A rectangular clip cutting a solid, a gradient and a glyph.
    dl.push_clip(IRect{104, 132, 30, 20});
    dl.quad(solid_quad(96, 126, 50, 30, rgba(180, 60, 160)));
    dl.quad(textured(QuadKind::glyph_a8, 110, 128, 0, 0, 32, 32, rgba(255, 255, 255)), *a8);
    dl.pop_clip();

    auto frame = [&](const Damage& damage, const DrawList& list, bool copy) -> Result<void> {
        auto begun = r.begin_frame(damage);
        if (!begun) return Unexpected<Error>(begun.error());
        if (!*begun) return make_error(Errc::unavailable, "the surface is not ready");
        if (copy) r.copy_rect(IRect{4, 4, 116, 30}, 8, 136);   // the gradients, into the bottom-left corner
        r.submit(list);
        if (auto ok = r.end_frame(); !ok) return ok;
        return r.present();
    };
    Result<void> ok = frame(Damage{{}, true}, dl, false);

    // Frame 2: partial damage (two rectangles) with new content, plus a copy.
    if (ok) {
        DrawList d2;
        d2.quad(solid_quad(0, 0, W, H, rgba(50, 20, 20)));   // only visible inside the damage
        d2.quad(textured(QuadKind::glyph_a8, 70, 66, 0, 0, 32, 32, rgba(255, 220, 120)), *a8);
        const IRect rects[2] = {IRect{60, 60, 50, 36}, IRect{0, 136, 4, 20}};
        ok = frame(Damage{rects, false}, d2, true);
    }
    r.destroy_texture(*a8);
    r.destroy_texture(*img);
    r.destroy_texture(*lcd);
    return ok;
}

}  // namespace kt::render
