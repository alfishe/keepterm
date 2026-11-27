// The built-in icon set (first step): monochrome line icons on a 24x24 grid with
// 2-unit strokes, drawn by keepterm from a handful of primitives (segments, arcs, dots, boxes)
// and rasterized into 8-bit coverage at the exact pixel size. No icon font, no third-party art.
// Icons are tinted at draw time, so a theme change re-rasterizes nothing.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace kt::ui {

enum class Icon : uint8_t {
    none,
    folder, terminal, plus, close, chevron_right, chevron_down, sidebar, split_right, split_down,
    zoom, unzoom, memory, cpu, gear, search, dot, agent, spinner, check, alert, storage, bell, pin,
    more, copy, menu, minimize, maximize, palette, trash, edit, keyboard, info, history,
    count_,
};

std::string_view icon_name(Icon);
Icon icon_from_name(std::string_view);   // Icon::none when unknown

// Coverage of `icon` at `px` x `px` pixels, row-major, 0..255.
std::vector<uint8_t> rasterize_icon(Icon icon, int px);

}  // namespace kt::ui
