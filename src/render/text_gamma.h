// Text coverage correction shared by all backends:
//   c' = enhanced_contrast(c, k)        k = 0.5 default (1.0 for LCD)
//   a  = gamma_correct(c', L, gamma)    gamma = 1.8 default
// GPU backends get gamma_ratios() as 4 floats in a constant buffer; the software backend uses
// a 16 x 256 LUT built from the same formula, so all backends agree within +-1.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kt::render {

struct TextGammaParams {
    float contrast = 0.5f;   // k
    float gamma = 1.8f;      // 1.0 .. 2.2
};

// The 4 coefficients of the DirectWrite-style alpha correction for a gamma in [1.0, 2.2],
// interpolated between the tabulated values at steps of 0.1.
std::array<float, 4> gamma_ratios(float gamma);

float enhanced_contrast(float coverage, float k);
// a + a (1 - a) ((g0 L + g1) a + (g2 L + g3)), clamped to [0, 1]; L = text luminance in [0, 1].
float gamma_correct(float coverage, float luminance, const std::array<float, 4>& g);

// Luminance of a premultiplied RGBA text color: dot(rgb, (0.25, 0.5, 0.25)) on the
// sRGB-encoded, unpremultiplied components, in [0, 1].
float text_luminance(uint32_t premul_rgba);

class CoverageLut {
public:
    static constexpr int kLevels = 16;
    explicit CoverageLut(const TextGammaParams& = {});
    void build(const TextGammaParams&);
    static int level_for(uint32_t premul_rgba);   // luminance bucket 0..15
    const uint8_t* row(int level) const { return lut_[static_cast<std::size_t>(level)].data(); }
    uint8_t at(int level, uint8_t coverage) const { return lut_[static_cast<std::size_t>(level)][coverage]; }
    const TextGammaParams& params() const { return params_; }

private:
    TextGammaParams params_;
    std::array<std::array<uint8_t, 256>, kLevels> lut_{};
};

}  // namespace kt::render
