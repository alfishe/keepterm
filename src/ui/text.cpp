#include "ui/text.h"

#include <cmath>

namespace kt::ui {

size_t utf8_next(std::string_view s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

size_t utf8_prev(std::string_view s, size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    --i;
    while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t utf8_length(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i = utf8_next(s, i)) ++n;
    return n;
}

char32_t utf8_decode(std::string_view s, size_t i) {
    if (i >= s.size()) return 0;
    const auto b0 = static_cast<uint8_t>(s[i]);
    auto cont = [&](size_t k) -> uint32_t {
        return i + k < s.size() ? static_cast<uint32_t>(static_cast<uint8_t>(s[i + k]) & 0x3F) : 0;
    };
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0) return static_cast<char32_t>(((b0 & 0x1Fu) << 6) | cont(1));
    if ((b0 & 0xF0) == 0xE0) return static_cast<char32_t>(((b0 & 0x0Fu) << 12) | (cont(1) << 6) | cont(2));
    if ((b0 & 0xF8) == 0xF0) return static_cast<char32_t>(((b0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3));
    return 0xFFFD;
}

void utf8_append(std::string& out, char32_t cp) {
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

TextRunPtr ITextService::ellipsize(std::string_view s, FontRole role, float scale, float max_w, bool middle) {
    TextRunPtr full = shape(s, role, scale);
    if (full->width() <= max_w || s.empty()) return full;
    static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    // Widths of prefixes from the glyph clusters: binary search on the cut.
    const size_t n = utf8_length(s);
    auto build = [&](size_t keep) {
        std::string out;
        if (!middle) {
            size_t i = 0;
            for (size_t k = 0; k < keep; ++k) i = utf8_next(s, i);
            out.assign(s.substr(0, i));
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out.append(kEllipsis);
        } else {
            const size_t head_n = (keep + 1) / 2, tail_n = keep / 2;
            size_t h = 0;
            for (size_t k = 0; k < head_n; ++k) h = utf8_next(s, h);
            size_t t = s.size();
            for (size_t k = 0; k < tail_n; ++k) t = utf8_prev(s, t);
            out.assign(s.substr(0, h));
            out.append(kEllipsis);
            out.append(s.substr(std::max(t, h)));
        }
        return out;
    };
    size_t lo = 0, hi = n;   // lo fits (an ellipsis alone always "fits"), hi does not
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (shape(build(mid), role, scale)->width() <= max_w) lo = mid;
        else hi = mid;
    }
    return shape(build(lo), role, scale);
}

TextRunPtr FixedTextService::shape(std::string_view s, FontRole role, float scale) {
    ++shapes;
    auto run = std::make_shared<TextRun>();
    run->role = role;
    run->scale = scale;
    const float adv = std::round(role.size * k_ * scale);
    float x = 0;
    for (size_t i = 0; i < s.size(); i = utf8_next(s, i)) {
        run->glyphs.push_back(ShapedGlyph{0, static_cast<uint32_t>(utf8_decode(s, i)), x, 0, static_cast<uint32_t>(i)});
        x += adv;
    }
    run->width_px = x;
    run->ascent_px = std::round(role.size * 0.8f * scale);
    run->descent_px = std::round(role.size * 0.2f * scale);
    return run;
}

FontMetrics FixedTextService::metrics(FontRole role, float scale) {
    const float a = std::round(role.size * 0.8f * scale) / scale, d = std::round(role.size * 0.2f * scale) / scale;
    return {a, d, std::round(role.size * 1.25f * scale) / scale};
}

}  // namespace kt::ui
