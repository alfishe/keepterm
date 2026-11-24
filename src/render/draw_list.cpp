#include "render/draw_list.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace kt::render {

Quad solid_quad(float x, float y, float w, float h, uint32_t color) {
    Quad q{};
    q.x = x;
    q.y = y;
    q.w = w;
    q.h = h;
    q.color = color;
    q.kind = static_cast<uint8_t>(QuadKind::solid);
    return q;
}

Quad glyph_quad(float x, float y, uint16_t u0, uint16_t v0, uint16_t w, uint16_t h, uint32_t color) {
    Quad q{};
    q.x = x;
    q.y = y;
    q.w = w;
    q.h = h;
    q.u0 = u0;
    q.v0 = v0;
    q.u1 = static_cast<uint16_t>(u0 + w);
    q.v1 = static_cast<uint16_t>(v0 + h);
    q.color = color;
    q.kind = static_cast<uint8_t>(QuadKind::glyph_a8);
    return q;
}

DrawList::DrawList() { clear(); }

void DrawList::clear() {
    quads.clear();
    batches.clear();
    clips.assign(1, Clip{kNoScissor, 0.0f});   // index 0 = none
    layers.clear();
    clip_stack_.clear();
    layer_stack_.clear();
    alpha_stack_.clear();
    break_batch_ = true;
}

namespace {

uint32_t scale_rgba(uint32_t c, float k) {
    auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFFu) * k;
        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))) << shift;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

}  // namespace

IRect DrawList::current_scissor() const {
    return clip_stack_.empty() ? kNoScissor : clips[clip_stack_.back()].rect;
}

void DrawList::push_clip(IRect r, float radius) {
    // Nested clips intersect; the rounded radius is that of the innermost clip.
    const IRect parent = current_scissor();
    IRect clipped = intersect(parent, r);
    if (clipped.empty()) clipped = IRect{r.x, r.y, 0, 0};
    clips.push_back(Clip{clipped, radius});
    clip_stack_.push_back(static_cast<uint16_t>(clips.size() - 1));
    break_batch_ = true;
}

void DrawList::pop_clip() {
    assert(!clip_stack_.empty());
    if (!clip_stack_.empty()) clip_stack_.pop_back();
    break_batch_ = true;
}

void DrawList::push_layer(IRect bounds, float opacity) {
    layers.push_back(Layer{bounds, opacity, static_cast<uint32_t>(batches.size()), 0});
    layer_stack_.push_back(static_cast<uint32_t>(layers.size() - 1));
    alpha_stack_.push_back(layer_alpha() * std::clamp(opacity, 0.0f, 1.0f));
    break_batch_ = true;
}

void DrawList::pop_layer() {
    assert(!layer_stack_.empty());
    if (layer_stack_.empty()) return;
    Layer& l = layers[layer_stack_.back()];
    l.batch_count = static_cast<uint32_t>(batches.size()) - l.first_batch;
    layer_stack_.pop_back();
    alpha_stack_.pop_back();
    break_batch_ = true;
}

void DrawList::quad(const Quad& q, TextureId tex) {
    Quad copy = q;
    copy.clip = current_clip();
    if (const float a = layer_alpha(); a < 1.0f) {
        // Colors are premultiplied, so opacity scales every channel. Images carry it in param.
        copy.color = scale_rgba(copy.color, a);
        copy.color2 = scale_rgba(copy.color2, a);
        if (copy.kind == static_cast<uint8_t>(QuadKind::image)) copy.param = (copy.param > 0 ? copy.param : 1.0f) * a;
    }
    const IRect scissor = current_scissor();
    const bool textured = copy.kind == static_cast<uint8_t>(QuadKind::glyph_a8) ||
                          copy.kind == static_cast<uint8_t>(QuadKind::glyph_lcd) ||
                          copy.kind == static_cast<uint8_t>(QuadKind::image);
    const TextureId t = textured ? tex : 0;
    // Untextured quads join any batch (the backend binds no texture for them); textured quads
    // need the batch's texture. A batch changes only on texture or scissor.
    bool merge = !break_batch_ && !batches.empty();
    if (merge) {
        Batch& b = batches.back();
        merge = b.scissor == scissor && (t == 0 || b.texture == 0 || b.texture == t);
        if (merge && t != 0) b.texture = t;
    }
    if (!merge) batches.push_back(Batch{t, static_cast<uint32_t>(quads.size()), 0, scissor});
    batches.back().count++;
    quads.push_back(copy);
    break_batch_ = false;
}

}  // namespace kt::render
