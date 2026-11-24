#include "render/gpu/gpu_common.h"

#include <algorithm>
#include <cmath>

#include "render/damage.h"

namespace kt::render::gpu {

FrameConstants make_constants(PxSize size, float y_sign, const TextGammaParams& tg) {
    FrameConstants c{};
    c.view[0] = size.w > 0 ? 2.0f / static_cast<float>(size.w) : 0.0f;
    c.view[1] = size.h > 0 ? 2.0f / static_cast<float>(size.h) : 0.0f;
    c.view[2] = y_sign;
    c.view[3] = tg.contrast;
    const auto g = gamma_ratios(tg.gamma);
    for (size_t i = 0; i < 4; ++i) c.gamma[i] = g[i];
    return c;
}

void FrameBuilder::begin(PxSize size, const Damage& damage) {
    size_ = size;
    damage_ = normalize_damage(damage, size);
    present_damage_ = damage_;
    clear_pending();
}

void FrameBuilder::add(const DrawList& dl, const TextureSize& tex_size) {
    if (damage_.empty()) return;
    const IRect surface{0, 0, size_.w, size_.h};
    for (const Batch& b : dl.batches) {
        uint16_t tw = 0, th = 0;
        const bool has_tex = b.texture != 0 && tex_size(b.texture, tw, th);
        const auto first = static_cast<uint32_t>(instances.size());
        const uint32_t end = std::min<uint32_t>(b.first + b.count, static_cast<uint32_t>(dl.quads.size()));
        for (uint32_t qi = b.first; qi < end; ++qi) {
            const Quad& q = dl.quads[qi];
            if (q.w <= 0 || q.h <= 0) continue;
            const auto kind = static_cast<QuadKind>(q.kind);
            const bool textured = kind == QuadKind::glyph_a8 || kind == QuadKind::glyph_lcd || kind == QuadKind::image;
            if (textured && !has_tex) continue;
            if (q.kind > static_cast<uint8_t>(QuadKind::shadow)) continue;
            Instance in{};
            in.rect[0] = q.x;
            in.rect[1] = q.y;
            in.rect[2] = q.w;
            in.rect[3] = q.h;
            in.uv[0] = q.u0;
            in.uv[1] = q.v0;
            in.uv[2] = q.u1;
            in.uv[3] = q.v1;
            IRect clip = surface;
            if (q.clip != 0 && q.clip < dl.clips.size()) clip = intersect(clip, dl.clips[q.clip].rect);   // rect only
            in.clip[0] = static_cast<float>(clip.x);
            in.clip[1] = static_cast<float>(clip.y);
            in.clip[2] = static_cast<float>(clip.right());
            in.clip[3] = static_cast<float>(clip.bottom());
            in.color = q.color;
            in.color2 = q.color2;
            in.kind_flags = uint32_t{q.kind} | (uint32_t{q.flags} << 8);
            in.tex_size = textured ? (uint32_t{tw} | (uint32_t{th} << 16)) : 0u;
            in.radius = q.radius;
            in.param = q.param;
            in.opacity = q.param > 0.0f && q.param < 1.0f ? std::round(q.param * 255.0f) : 255.0f;
            in.luminance = static_cast<float>(CoverageLut::level_for(q.color)) / static_cast<float>(CoverageLut::kLevels - 1);
            instances.push_back(in);
        }
        const auto count = static_cast<uint32_t>(instances.size()) - first;
        if (count == 0) continue;
        for (const IRect& d : damage_) {
            const IRect sc = intersect(b.scissor, d);
            if (sc.empty()) continue;
            draws.push_back(Draw{has_tex ? b.texture : 0, first, count, sc});
        }
    }
}

bool clip_upload(IRect& region, const uint8_t*& pixels, uint32_t stride, uint16_t tw, uint16_t th, uint32_t bpp) {
    if (!pixels) return false;
    const IRect r = intersect(region, IRect{0, 0, tw, th});
    if (r.empty()) return false;
    pixels += static_cast<size_t>(r.y - region.y) * stride + static_cast<size_t>(r.x - region.x) * bpp;
    region = r;
    return true;
}

}  // namespace kt::render::gpu
