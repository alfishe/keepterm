#include "app/png_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

namespace kt::app {

namespace {

uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const std::vector<uint8_t>& v) {
    uint32_t a = 1, b = 0;
    for (const uint8_t x : v) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace

Result<void> write_png_rgb(const std::string& path, const render::Image& img) {
    if (img.w <= 0 || img.h <= 0) return make_error(Errc::invalid_argument, "empty image");
    const auto w = static_cast<size_t>(img.w), h = static_cast<size_t>(img.h);
    std::vector<uint8_t> raw;
    raw.reserve(h * (w * 3 + 1));
    for (int32_t y = 0; y < img.h; ++y) {
        raw.push_back(0);   // filter: none
        for (int32_t x = 0; x < img.w; ++x) {
            const uint32_t c = img.rgba_at(x, y);
            raw.push_back(static_cast<uint8_t>(render::ch_r(c)));
            raw.push_back(static_cast<uint8_t>(render::ch_g(c)));
            raw.push_back(static_cast<uint8_t>(render::ch_b(c)));
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t off = 0;;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (last) break;
    }
    put32(z, adler32(raw));

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(img.w));
    put32(ihdr, static_cast<uint32_t>(img.h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!f) return make_error(Errc::os_error, "cannot write " + path);
    return {};
}

}  // namespace kt::app
