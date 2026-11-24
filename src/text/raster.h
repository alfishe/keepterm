// Internal: glyph rasterization with FreeType and procedural glyphs.
#pragma once

#include <cstdint>

#include "base/error.h"
#include "text/atlas.h"
#include "text/face.h"
#include "text/font.h"

namespace kt::text::detail {

// Grayscale coverage (FT_LOAD_TARGET_LIGHT + FT_RENDER_MODE_NORMAL) into an a8 bitmap.
// scale_q != 0 scales the outline horizontally by scale_q / 4096 (glyph wider than its span).
Result<GlyphBitmap> rasterize_glyph(Face&, uint32_t glyph_id, RenderMode, uint16_t scale_q);

// Code points drawn by keepterm instead of the font: box drawing U+2500..U+257F and
// block elements U+2580..U+259F (braille, powerline and legacy computing come later).
bool is_procedural(char32_t cp);

// A cell-sized a8 bitmap (span cells wide); bearing_x = 0, bearing_y = baseline, so that the
// bitmap's top-left lands on the cell's top-left.
Result<GlyphBitmap> rasterize_procedural(FT_Library, char32_t cp, const CellMetrics&);

// Last resort: a box with the code point in hex.
GlyphBitmap rasterize_tofu(char32_t cp, uint8_t span, const CellMetrics&);

// The atlas key variant for procedural glyphs and tofu.
uint32_t procedural_variant(const CellMetrics&, uint8_t span);

}  // namespace kt::text::detail
