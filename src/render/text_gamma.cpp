#include "render/text_gamma.h"

#include <algorithm>
#include <cmath>

#include "render/types.h"

namespace kt::render {

namespace {

// Alpha correction coefficients for gamma 1.0 .. 2.2 in steps of 0.1: the closed-form fit
// used by DirectWrite-style GPU text renderers (as published in Windows Terminal's
// AtlasEngine, MIT), each value divided by 4.
constexpr float kRatios[13][4] = {
    {0.0000f / 4, 0.0000f / 4, 0.0000f / 4, 0.0000f / 4},     // 1.0
    {0.0166f / 4, -0.0807f / 4, 0.2227f / 4, -0.0751f / 4},   // 1.1
    {0.0350f / 4, -0.1760f / 4, 0.4325f / 4, -0.1370f / 4},   // 1.2
    {0.0543f / 4, -0.2821f / 4, 0.6302f / 4, -0.1876f / 4},   // 1.3
    {0.0739f / 4, -0.3963f / 4, 0.8167f / 4, -0.2287f / 4},   // 1.4
    {0.0933f / 4, -0.5161f / 4, 0.9926f / 4, -0.2616f / 4},   // 1.5
    {0.1121f / 4, -0.6395f / 4, 1.1588f / 4, -0.2877f / 4},   // 1.6
    {0.1300f / 4, -0.7649f / 4, 1.3159f / 4, -0.3080f / 4},   // 1.7
    {0.1469f / 4, -0.8911f / 4, 1.4644f / 4, -0.3234f / 4},   // 1.8
    {0.1627f / 4, -1.0170f / 4, 1.6051f / 4, -0.3347f / 4},   // 1.9
    {0.1773f / 4, -1.1420f / 4, 1.7385f / 4, -0.3426f / 4},   // 2.0
    {0.1908f / 4, -1.2652f / 4, 1.8650f / 4, -0.3476f / 4},   // 2.1
    {0.2031f / 4, -1.3864f / 4, 1.9851f / 4, -0.3501f / 4},   // 2.2
};

}  // namespace

std::array<float, 4> gamma_ratios(float gamma) {
    const float t = (std::clamp(gamma, 1.0f, 2.2f) - 1.0f) * 10.0f;
    const int i = std::min(11, static_cast<int>(t));
    const float f = t - static_cast<float>(i);
    std::array<float, 4> g{};
    for (int k = 0; k < 4; ++k) g[static_cast<size_t>(k)] = kRatios[i][k] + (kRatios[i + 1][k] - kRatios[i][k]) * f;
    return g;
}

float enhanced_contrast(float c, float k) { return c * (k + 1.0f) / (c * k + 1.0f); }

float gamma_correct(float a, float L, const std::array<float, 4>& g) {
    const float r = a + a * (1.0f - a) * ((g[0] * L + g[1]) * a + (g[2] * L + g[3]));
    return std::clamp(r, 0.0f, 1.0f);
}

float text_luminance(uint32_t c) {
    const uint32_t a = ch_a(c);
    if (a == 0) return 0.0f;
    auto un = [&](uint32_t v) { return std::min(1.0f, static_cast<float>(v) / static_cast<float>(a)); };
    return 0.25f * un(ch_r(c)) + 0.5f * un(ch_g(c)) + 0.25f * un(ch_b(c));
}

CoverageLut::CoverageLut(const TextGammaParams& p) { build(p); }

void CoverageLut::build(const TextGammaParams& p) {
    params_ = p;
    const auto g = gamma_ratios(p.gamma);
    for (int level = 0; level < kLevels; ++level) {
        const float L = static_cast<float>(level) / static_cast<float>(kLevels - 1);
        auto& row = lut_[static_cast<size_t>(level)];
        for (int c = 0; c < 256; ++c) {
            const float a = gamma_correct(enhanced_contrast(static_cast<float>(c) / 255.0f, p.contrast), L, g);
            row[static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(a * 255.0f));
        }
        row[0] = 0;
        row[255] = 255;
    }
}

int CoverageLut::level_for(uint32_t c) {
    const float L = text_luminance(c);
    return std::clamp(static_cast<int>(std::lround(L * static_cast<float>(kLevels - 1))), 0, kLevels - 1);
}

}  // namespace kt::render
