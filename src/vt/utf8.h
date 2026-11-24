// UTF-8 encoding helper for the VT module (internal).
#pragma once

#include <cstdint>
#include <string>

namespace kt::vt {

template <class Str>
inline void append_utf8(Str& out, char32_t cp) {
    using C = typename Str::value_type;
    const auto c = static_cast<uint32_t>(cp);
    if (c < 0x80) {
        out.push_back(static_cast<C>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<C>(0xC0u | (c >> 6)));
        out.push_back(static_cast<C>(0x80u | (c & 0x3Fu)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<C>(0xE0u | (c >> 12)));
        out.push_back(static_cast<C>(0x80u | ((c >> 6) & 0x3Fu)));
        out.push_back(static_cast<C>(0x80u | (c & 0x3Fu)));
    } else {
        out.push_back(static_cast<C>(0xF0u | (c >> 18)));
        out.push_back(static_cast<C>(0x80u | ((c >> 12) & 0x3Fu)));
        out.push_back(static_cast<C>(0x80u | ((c >> 6) & 0x3Fu)));
        out.push_back(static_cast<C>(0x80u | (c & 0x3Fu)));
    }
}

}  // namespace kt::vt
