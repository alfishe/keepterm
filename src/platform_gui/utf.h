// Small UTF-8 / UTF-16 helpers of the platform layer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kt::platform::detail {

void utf8_append(std::u8string& out, char32_t cp);
// Joins surrogate pairs; lone surrogates become U+FFFD.
std::u8string utf16_to_utf8(std::u16string_view s);
// Invalid sequences become U+FFFD.
std::u16string utf8_to_utf16(std::u8string_view s);
// Decodes the first code point; returns 0 for empty input. `len` receives the bytes used.
char32_t utf8_first(std::u8string_view s, size_t* len = nullptr);
// The UTF-8 byte offset of UTF-16 index `idx16` in `s16` (clamped to the end).
uint32_t utf8_offset_of_utf16_index(std::u16string_view s16, size_t idx16);
// Latin-1 bytes to UTF-8 (X11 STRING targets).
std::u8string latin1_to_utf8(std::string_view s);
// The local file paths of a text/uri-list (RFC 2483; XDND, Wayland): `file://` URIs with an
// empty or "localhost" host, percent-decoded. Comments, other schemes and remote hosts are skipped.
std::vector<std::string> paths_from_uri_list(std::string_view list);

}  // namespace kt::platform::detail
