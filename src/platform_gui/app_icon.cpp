#include "platform_gui/app_icon.h"

#include <array>

namespace kt::platform {
namespace {

#include "platform_gui/app_icon_rgba.inc"

const std::array<AppIconImage, std::size(kIconImages)>& images() {
    static const auto all = [] {
        std::array<AppIconImage, std::size(kIconImages)> a{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            const int s = kIconImages[i].size;
            a[i] = AppIconImage{s, std::span<const uint8_t>(kIconImages[i].rgba, static_cast<std::size_t>(s) * static_cast<std::size_t>(s) * 4)};
        }
        return a;
    }();
    return all;
}

}  // namespace

std::span<const AppIconImage> app_icon_images() { return images(); }

std::vector<unsigned long> app_icon_net_wm_icon() {
    std::vector<unsigned long> out;
    for (const auto& im : images()) {
        out.push_back(static_cast<unsigned long>(im.size));
        out.push_back(static_cast<unsigned long>(im.size));
        for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
            const uint32_t argb = uint32_t(im.rgba[i + 3]) << 24 | uint32_t(im.rgba[i]) << 16 |
                                  uint32_t(im.rgba[i + 1]) << 8 | uint32_t(im.rgba[i + 2]);
            out.push_back(argb);
        }
    }
    return out;
}

}  // namespace kt::platform
