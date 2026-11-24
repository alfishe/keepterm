// Cells, colors and styles.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kt::vt {

enum class CellWidth : uint8_t {
    Narrow = 0,
    WideLead = 1,
    WideTail = 2,  // spacer right of a WideLead
    Empty = 3,     // padding left at the last column when a wide character wrapped
};

// 8 bytes, trivially copyable. The design writes the first word as bitfields; it is packed by hand
// here so that the layout is the same on every compiler and stores need no narrowing casts:
//   bits 0..20  code point, or GraphemePool index if `cluster`
//   bits 21..22 CellWidth
//   bit  23     cluster
//   bit  24     protected (DECSCA)
// A zero cell is a blank (code point 0, narrow, default style): it reads as a space.
struct Cell {
    static constexpr uint32_t kCpMask = 0x1FFFFFu;
    static constexpr uint32_t kWidthShift = 21;
    static constexpr uint32_t kClusterBit = 1u << 23;
    static constexpr uint32_t kProtBit = 1u << 24;

    uint32_t bits = 0;
    uint16_t style = 0;  // StyleTable index; 0 = default style
    uint16_t link = 0;   // LinkTable index; 0 = none

    static constexpr Cell make(char32_t cp, CellWidth w, uint16_t style, uint16_t link = 0) {
        Cell c;
        c.bits = (static_cast<uint32_t>(cp) & kCpMask) | (static_cast<uint32_t>(w) << kWidthShift);
        c.style = style;
        c.link = link;
        return c;
    }
    static constexpr Cell blank(uint16_t style) { return make(0, CellWidth::Narrow, style); }

    constexpr char32_t cp() const { return static_cast<char32_t>(bits & kCpMask); }
    constexpr CellWidth width() const { return static_cast<CellWidth>((bits >> kWidthShift) & 3u); }
    constexpr bool cluster() const { return (bits & kClusterBit) != 0; }
    constexpr bool prot() const { return (bits & kProtBit) != 0; }
    // True for a cell that shows nothing: a narrow blank or wrap padding. A WideTail is not blank.
    constexpr bool is_blank() const {
        const CellWidth w = width();
        return w == CellWidth::Empty || (w == CellWidth::Narrow && (bits & (kCpMask | kClusterBit)) == 0);
    }

    constexpr void set_prot(bool on) { bits = on ? (bits | kProtBit) : (bits & ~kProtBit); }
    constexpr void set_cluster(uint32_t index) {
        bits = (bits & ~kCpMask) | (index & kCpMask) | kClusterBit;
    }

    friend constexpr bool operator==(const Cell&, const Cell&) = default;
};
static_assert(sizeof(Cell) == 8);
static_assert(std::is_trivially_copyable_v<Cell>);

// A color: the tag is in the top byte, the value in the low 24 bits.
struct Color {
    enum Tag : uint32_t { Default = 0, Palette = 1, Rgb = 2 };
    uint32_t v = 0;

    static constexpr Color def() { return {}; }
    static constexpr Color palette(uint8_t index) { return {(uint32_t{Palette} << 24) | index}; }
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
        return {(uint32_t{Rgb} << 24) | (uint32_t{r} << 16) | (uint32_t{g} << 8) | b};
    }
    constexpr Tag tag() const { return static_cast<Tag>(v >> 24); }
    constexpr uint8_t index() const { return static_cast<uint8_t>(v & 0xFFu); }
    constexpr uint8_t r() const { return static_cast<uint8_t>((v >> 16) & 0xFFu); }
    constexpr uint8_t g() const { return static_cast<uint8_t>((v >> 8) & 0xFFu); }
    constexpr uint8_t b() const { return static_cast<uint8_t>(v & 0xFFu); }

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

enum Attr : uint16_t {
    kBold = 1u << 0,
    kFaint = 1u << 1,
    kItalic = 1u << 2,
    kBlink = 1u << 3,
    kInverse = 1u << 4,
    kInvisible = 1u << 5,
    kStrike = 1u << 6,
    kOverline = 1u << 7,
};

enum class Underline : uint8_t { None, Single, Double, Curly, Dotted, Dashed };

struct Style {  // 16 bytes
    Color fg, bg, underline_color;
    uint16_t attrs = 0;  // Attr bits
    Underline underline = Underline::None;
    uint8_t spare = 0;

    constexpr bool has(Attr a) const { return (attrs & a) != 0; }
    constexpr void set(Attr a, bool on) {
        const uint32_t v = attrs;
        const auto bit = static_cast<uint32_t>(a);
        attrs = static_cast<uint16_t>(on ? (v | bit) : (v & ~bit));
    }

    friend constexpr bool operator==(const Style&, const Style&) = default;
};
static_assert(sizeof(Style) == 16);

// Row flags (FrameRow::flags).
enum RowFlag : uint16_t {
    kRowWrapped = 1u << 0,      // soft-wrapped into the next row
    kRowPromptStart = 1u << 1,  // OSC 133 A
    kRowInputStart = 1u << 2,   // OSC 133 B
    kRowOutputStart = 1u << 3,  // OSC 133 C
    kRowHasLink = 1u << 4,
    kRowHasGrapheme = 1u << 5,
};

}  // namespace kt::vt
