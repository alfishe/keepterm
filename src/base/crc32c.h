// CRC-32C (Castagnoli), used by the KTSN snapshot format and the history journal.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace kt {

// Continues a CRC: pass the previous result as `crc` (0 to start).
uint32_t crc32c(std::span<const std::byte> data, uint32_t crc = 0);

}  // namespace kt
