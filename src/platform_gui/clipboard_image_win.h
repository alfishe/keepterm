// Clipboard images on Windows: a packed DIB (CF_DIB) re-encoded as PNG through WIC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kt::platform::detail {

// PNG bytes of the packed DIB `dib` (BITMAPINFOHEADER or later, optional masks and colour
// table, then the pixels); empty when WIC cannot read it. Initialises COM for the call.
std::vector<uint8_t> dib_to_png(const uint8_t* dib, size_t size);

}  // namespace kt::platform::detail
