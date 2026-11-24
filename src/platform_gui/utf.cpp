#include "platform_gui/utf.h"

namespace kt::platform::detail {

void utf8_append(std::u8string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char8_t>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char8_t>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char8_t>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char8_t>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    }
}

std::u8string utf16_to_utf8(std::u16string_view s) {
    std::u8string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00);
                ++i;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        utf8_append(out, c);
    }
    return out;
}

char32_t utf8_first(std::u8string_view s, size_t* len) {
    size_t n = 0;
    char32_t cp = 0;
    if (s.empty()) {
        if (len) *len = 0;
        return 0;
    }
    const auto b0 = static_cast<uint8_t>(s[0]);
    auto cont = [&](size_t i) -> int {
        if (i >= s.size()) return -1;
        const auto b = static_cast<uint8_t>(s[i]);
        return (b & 0xC0) == 0x80 ? (b & 0x3F) : -1;
    };
    if (b0 < 0x80) {
        cp = b0;
        n = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        const int c1 = cont(1);
        if (c1 < 0) { n = 1; cp = 0xFFFD; }
        else { cp = (static_cast<char32_t>(b0 & 0x1F) << 6) | static_cast<char32_t>(c1); n = 2; if (cp < 0x80) cp = 0xFFFD; }
    } else if ((b0 & 0xF0) == 0xE0) {
        const int c1 = cont(1), c2 = cont(2);
        if (c1 < 0 || c2 < 0) { n = 1; cp = 0xFFFD; }
        else {
            cp = (static_cast<char32_t>(b0 & 0x0F) << 12) | (static_cast<char32_t>(c1) << 6) | static_cast<char32_t>(c2);
            n = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        }
    } else if ((b0 & 0xF8) == 0xF0) {
        const int c1 = cont(1), c2 = cont(2), c3 = cont(3);
        if (c1 < 0 || c2 < 0 || c3 < 0) { n = 1; cp = 0xFFFD; }
        else {
            cp = (static_cast<char32_t>(b0 & 0x07) << 18) | (static_cast<char32_t>(c1) << 12) |
                 (static_cast<char32_t>(c2) << 6) | static_cast<char32_t>(c3);
            n = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
        }
    } else {
        n = 1;
        cp = 0xFFFD;
    }
    if (len) *len = n;
    return cp;
}

std::u16string utf8_to_utf16(std::u8string_view s) {
    std::u16string out;
    out.reserve(s.size());
    while (!s.empty()) {
        size_t n = 0;
        const char32_t cp = utf8_first(s, &n);
        s.remove_prefix(n);
        if (cp >= 0x10000) {
            const char32_t v = cp - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

uint32_t utf8_offset_of_utf16_index(std::u16string_view s16, size_t idx16) {
    uint32_t bytes = 0;
    for (size_t i = 0; i < s16.size() && i < idx16; ++i) {
        const char16_t c = s16[i];
        if (c < 0x80) bytes += 1;
        else if (c < 0x800) bytes += 2;
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s16.size() && s16[i + 1] >= 0xDC00 && s16[i + 1] <= 0xDFFF) {
            bytes += 4;
            ++i;
        } else bytes += 3;
    }
    return bytes;
}

std::u8string latin1_to_utf8(std::string_view s) {
    std::u8string out;
    out.reserve(s.size());
    for (char c : s) utf8_append(out, static_cast<char32_t>(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> paths_from_uri_list(std::string_view list) {
    std::vector<std::string> out;
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    while (!list.empty()) {
        const size_t nl = list.find('\n');
        std::string_view line = list.substr(0, nl);
        list = nl == std::string_view::npos ? std::string_view() : list.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#' || !line.starts_with("file://")) continue;
        line.remove_prefix(7);
        const size_t slash = line.find('/');
        if (slash == std::string_view::npos) continue;
        const std::string_view host = line.substr(0, slash);
        if (!host.empty() && host != "localhost") continue;
        std::string path;
        for (size_t k = slash; k < line.size(); ++k) {
            if (line[k] == '%') {
                const int a = k + 1 < line.size() ? hex(line[k + 1]) : -1, b = k + 2 < line.size() ? hex(line[k + 2]) : -1;
                if (a >= 0 && b >= 0) {
                    path += static_cast<char>(a * 16 + b);
                    k += 2;
                    continue;
                }
            }
            path += line[k];
        }
        if (path.find('\0') == std::string::npos) out.push_back(std::move(path));
    }
    return out;
}

}  // namespace kt::platform::detail
