#include "store/fold.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace kt::store {

namespace {

struct FoldRun {
    uint32_t first, last;
    int32_t delta;
    uint32_t step;
};

constexpr FoldRun kRuns[] = {
#include "store/casefold_table.inc"
};

}  // namespace

char32_t fold_cp(char32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    const auto c = static_cast<uint32_t>(cp);
    const auto* it = std::upper_bound(std::begin(kRuns), std::end(kRuns), c,
                                      [](uint32_t v, const FoldRun& r) { return v < r.first; });
    if (it == std::begin(kRuns)) return cp;
    --it;
    if (c > it->last || (c - it->first) % it->step != 0) return cp;
    return static_cast<char32_t>(static_cast<int64_t>(c) + it->delta);
}

void append_utf8(std::string& out, char32_t cp) {
    const auto c = static_cast<uint32_t>(cp);
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

void fold_utf8(std::string_view in, std::string& out) {
    out.reserve(out.size() + in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        const unsigned char b = *p;
        if (b < 0x80) {
            // An ASCII run: lower-cased in place, without per-byte appends.
            const auto* q = p;
            while (q < end && *q < 0x80) ++q;
            const std::size_t at = out.size(), n = static_cast<std::size_t>(q - p);
            out.resize(at + n);
            char* o = out.data() + at;
            for (std::size_t i = 0; i < n; ++i) {
                const unsigned char c = p[i];
                o[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            p = q;
            continue;
        }
        int len = 0;
        uint32_t cp = 0;
        if ((b & 0xE0) == 0xC0) {
            len = 2;
            cp = b & 0x1Fu;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
            cp = b & 0x0Fu;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4;
            cp = b & 0x07u;
        }
        bool ok = len > 0 && end - p >= len;
        for (int i = 1; ok && i < len; ++i) {
            if ((p[i] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (p[i] & 0x3Fu);
        }
        if (!ok) {
            out.push_back(static_cast<char>(b));
            ++p;
            continue;
        }
        const char32_t f = fold_cp(static_cast<char32_t>(cp));
        if (f == static_cast<char32_t>(cp)) {
            out.append(reinterpret_cast<const char*>(p), static_cast<std::size_t>(len));
        } else {
            append_utf8(out, f);
        }
        p += len;
    }
}

}  // namespace kt::store
