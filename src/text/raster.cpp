#include "text/raster.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include FT_OUTLINE_H
#include FT_SYNTHESIS_H

namespace kt::text::detail {

namespace {

int16_t clamp16(FT_Int v) {
    return static_cast<int16_t>(std::clamp<FT_Int>(v, std::numeric_limits<int16_t>::min(),
                                                   std::numeric_limits<int16_t>::max()));
}

struct TransformGuard {
    FT_Face face;
    bool active = false;
    ~TransformGuard() {
        if (active) FT_Set_Transform(face, nullptr, nullptr);
    }
};

}  // namespace

Result<GlyphBitmap> rasterize_glyph(Face& face, uint32_t glyph_id, RenderMode mode, uint16_t scale_q) {
    (void)mode;   // LCD rendering is not built yet; every mode renders grayscale for now.
    FT_Face ft = face.ft();
    TransformGuard guard{ft};
    if (scale_q != 0 && scale_q != 4096) {
        FT_Matrix m;
        m.xx = static_cast<FT_Fixed>(scale_q) * 16;   // 16.16
        m.xy = 0;
        m.yx = 0;
        m.yy = 0x10000;
        FT_Set_Transform(ft, &m, nullptr);
        guard.active = true;
    }
    FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
    flags |= static_cast<FT_Int32>(face.hinting() == Hinting::light ? FT_LOAD_TARGET_LIGHT : FT_LOAD_NO_HINTING);
    FT_Error err = FT_Load_Glyph(ft, glyph_id, flags);
    if (err) return make_error(Errc::corrupt, "FT_Load_Glyph failed", err);

    FT_GlyphSlot slot = ft->glyph;
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (face.thicken_26_6 > 0) {
            // font.thicken: grow every stroke around its center; the advance stays on the cell.
            FT_Outline_Embolden(&slot->outline, static_cast<FT_Pos>(face.thicken_26_6));
        }
        if (face.synth() & kSynthBold) {
            // strength = size_px / 24 in x, 0 in y: the advance stays on the cell.
            const FT_Pos strength = static_cast<FT_Pos>(face.size_26_6() / 24);
            FT_Outline_EmboldenXY(&slot->outline, strength, 0);
        }
        if (face.synth() & kSynthItalic) {
            FT_Matrix shear;
            shear.xx = 0x10000;
            shear.xy = 0x3333;   // 0.2
            shear.yx = 0;
            shear.yy = 0x10000;
            FT_Outline_Transform(&slot->outline, &shear);
        }
    }
    err = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
    if (err) return make_error(Errc::corrupt, "FT_Render_Glyph failed", err);

    const FT_Bitmap& src = slot->bitmap;
    GlyphBitmap out;
    out.format = PageFormat::a8;
    out.bearing_x = clamp16(slot->bitmap_left);
    out.bearing_y = clamp16(slot->bitmap_top);
    if (src.width == 0 || src.rows == 0) return out;
    if (src.width > 0xFFFF || src.rows > 0xFFFF) return make_error(Errc::invalid_argument, "glyph too large");
    out.w = static_cast<uint16_t>(src.width);
    out.h = static_cast<uint16_t>(src.rows);
    out.pixels.resize(size_t{out.w} * out.h);
    const unsigned pitch = static_cast<unsigned>(std::abs(src.pitch));
    for (unsigned row = 0; row < src.rows; ++row) {
        const unsigned src_row = src.pitch >= 0 ? row : src.rows - 1 - row;
        const unsigned char* s = src.buffer + size_t{src_row} * pitch;
        uint8_t* d = &out.pixels[size_t{row} * out.w];
        if (src.pixel_mode == FT_PIXEL_MODE_GRAY) {
            std::memcpy(d, s, out.w);
        } else if (src.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (unsigned x = 0; x < src.width; ++x) d[x] = (s[x >> 3] & (0x80u >> (x & 7))) ? 255 : 0;
        } else {
            return make_error(Errc::not_supported, "unsupported glyph pixel mode");
        }
    }
    return out;
}

}  // namespace kt::text::detail
