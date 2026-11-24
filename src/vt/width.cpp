#include "vt/width.h"

#include "vt/width_tables.h"

namespace kt::vt::width {

W classify(char32_t cp) {
    const auto c = static_cast<uint32_t>(cp);
    if (c >= 0x110000u) return W::Narrow;
    const uint32_t block = detail::kStage1[c >> detail::kBlockBits];
    const uint32_t offset = c & ((1u << detail::kBlockBits) - 1u);
    const uint32_t byte = detail::kStage2[(block << (detail::kBlockBits - 2u)) + (offset >> 2u)];
    return static_cast<W>((byte >> ((offset & 3u) * 2u)) & 3u);
}

int cell_width(char32_t cp, bool ambiguous_is_wide) {
    if (cp >= 0x20 && cp < 0x7F) return 1;  // printable ASCII: the common case
    switch (classify(cp)) {
        case W::Zero: return 0;
        case W::Narrow: return 1;
        case W::Wide: return 2;
        case W::Ambiguous: return ambiguous_is_wide ? 2 : 1;
    }
    return 1;
}

bool is_emoji_presentation(char32_t cp) {
    const auto c = static_cast<uint32_t>(cp);
    uint32_t lo = 0, hi = detail::kEmojiPresentationCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const auto& r = detail::kEmojiPresentation[mid];
        if (c < r.first) {
            hi = mid;
        } else if (c > r.last) {
            lo = mid + 1u;
        } else {
            return true;
        }
    }
    return false;
}

const char* unicode_version() { return detail::kUnicodeVersion; }

}  // namespace kt::vt::width
