// A byte range of UTF-8 text from an untrusted source (a store hit, an offset from another
// rendering of the line), made safe to mark: inside the text and on character boundaries.
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace kt {

// Clamps [at, at + len) to the text and widens it to whole characters (a continuation byte at
// either end moves the edge out to its character). Nothing when the range is empty or outside.
inline std::optional<std::pair<std::size_t, std::size_t>> utf8_whole_range(std::string_view text, std::size_t at,
                                                                           std::size_t len) {
    if (len == 0 || at >= text.size()) return std::nullopt;
    std::size_t end = len > text.size() - at ? text.size() : at + len;
    const auto cont = [&](std::size_t i) { return (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u; };
    while (at > 0 && cont(at)) --at;
    while (end < text.size() && cont(end)) ++end;
    return std::make_pair(at, end - at);
}

}  // namespace kt
