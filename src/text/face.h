// Internal: one font face at one pixel size, FreeType + HarfBuzz.
// Only text .cpp files include this header; it pulls in FreeType and HarfBuzz.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include "base/error.h"
#include "text/font.h"

namespace kt::text::detail {

inline constexpr uint8_t kSynthBold = 1;
inline constexpr uint8_t kSynthItalic = 2;

// Vertical metrics in 26.6 pixels at the face's size (y up, descender negative).
struct VMetrics {
    int32_t ascender = 0, descender = 0, line_gap = 0;
    int32_t underline_pos = 0, underline_thickness = 0;   // underline top above the baseline
    int32_t strikeout_pos = 0, strikeout_thickness = 0;   // strikeout top above the baseline
    int32_t x_height = 0;
};

class Face {
public:
    using Blob = std::shared_ptr<const std::vector<uint8_t>>;

    static Result<std::unique_ptr<Face>> open(FT_Library, Blob data, uint32_t index, float size_px,
                                              uint8_t synth, Hinting);
    ~Face();
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;

    FT_Face ft() const { return ft_; }
    hb_font_t* hb() const { return hb_; }

    FaceId id = kNoFace;
    int32_t thicken_26_6 = 0;   // stem growth in 26.6 px (FontSpec::thicken at the set's scale)
    float size_px() const { return size_px_; }
    uint32_t size_26_6() const { return size_26_6_; }
    uint8_t synth() const { return synth_; }
    Hinting hinting() const { return hinting_; }
    const VMetrics& vmetrics() const { return vm_; }

    uint32_t glyph_index(char32_t cp) const;
    // Glyph ids for printable ASCII 0x20..0x7E (the shaper's fast path).
    uint32_t ascii_glyph(char32_t cp) const { return ascii_[cp - 0x20]; }
    // Unhinted advance in 26.6 pixels.
    int32_t advance_26_6(uint32_t gid) const;
    // Maximum advance over printable ASCII in 26.6 pixels.
    int32_t max_ascii_advance_26_6() const;

private:
    Face() = default;

    Blob data_;
    FT_Face ft_ = nullptr;
    hb_font_t* hb_ = nullptr;
    float size_px_ = 0;
    uint32_t size_26_6_ = 0;
    uint8_t synth_ = 0;
    Hinting hinting_ = Hinting::light;
    VMetrics vm_;
    std::array<uint32_t, 95> ascii_{};
};

// Cell metrics of a primary regular face.
CellMetrics compute_cell_metrics(const Face& regular, float line_height, float cell_width = 1.0f);

}  // namespace kt::text::detail
