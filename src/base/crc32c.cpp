#include "base/crc32c.h"

#include <array>

namespace kt {
namespace {

// Slicing-by-8 tables: kTable[0] is the classic byte table, kTable[k][i] advances it k bytes.
constexpr std::array<std::array<uint32_t, 256>, 8> make_tables() {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        t[0][i] = c;
    }
    for (std::size_t k = 1; k < 8; ++k) {
        for (uint32_t i = 0; i < 256; ++i) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFFu];
    }
    return t;
}

constexpr auto kTable = make_tables();

}  // namespace

// Table-driven, slicing-by-8 (portable, byte order independent). A hardware path (SSE4.2
// crc32, ARMv8 CRC) can replace it later without changing results.
uint32_t crc32c(std::span<const std::byte> data, uint32_t crc) {
    uint32_t c = ~crc;
    const std::byte* p = data.data();
    std::size_t n = data.size();
    const auto b = [&](std::size_t i) { return static_cast<uint32_t>(p[i]); };
    while (n >= 8) {
        const uint32_t lo = c ^ (b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24));
        c = kTable[7][lo & 0xFFu] ^ kTable[6][(lo >> 8) & 0xFFu] ^ kTable[5][(lo >> 16) & 0xFFu] ^
            kTable[4][lo >> 24] ^ kTable[3][b(4)] ^ kTable[2][b(5)] ^ kTable[1][b(6)] ^ kTable[0][b(7)];
        p += 8;
        n -= 8;
    }
    for (std::size_t i = 0; i < n; ++i) c = kTable[0][(c ^ b(i)) & 0xFFu] ^ (c >> 8);
    return ~c;
}

}  // namespace kt
