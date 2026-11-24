// Value types of the public VT interface.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kt::vt {

struct GridSize {
    uint16_t cols = 80, rows = 24;          // cells
    uint16_t cell_w_px = 0, cell_h_px = 0;  // for XTWINOPS 14/16 and SGR-pixel mouse; 0 = unknown

    friend bool operator==(const GridSize&, const GridSize&) = default;
};

enum class Osc52Policy : uint8_t { Off, WriteOnly, ReadWrite };

struct TerminalOptions {
    uint32_t scrollback_lines = 10'000;
    uint32_t scrollback_max_bytes = 8u << 20;   // heap of the scrollback rows (Scrollback::bytes)
    bool ambiguous_is_wide = false;             // East Asian Ambiguous width
    bool grapheme_clusters = false;             // initial value of mode 2027; off by default
    Osc52Policy osc52 = Osc52Policy::WriteOnly;
    bool allow_title_query = false;             // XTWINOPS 21 is a known injection vector
    uint32_t max_osc_bytes = 1u << 20;          // OSC 52 payloads can be large
    std::u8string answerback;                   // ENQ reply, empty by default
    // DECRQCRA (rectangle checksum) lets a program read the screen cell by cell; test hosts
    // (esctest) turn it on, terminals in use leave it off.
    bool allow_rect_checksum = false;
};

enum class CursorShape : uint8_t { Block, Underline, Bar };

struct CursorState {
    uint16_t col = 0, row = 0;
    CursorShape shape = CursorShape::Block;
    bool visible = true;
    bool blink = false;
    bool pending_wrap = false;

    friend bool operator==(const CursorState&, const CursorState&) = default;
};

enum class MouseTracking : uint8_t { None, X10, Normal, Button, Any };  // 9, 1000, 1002, 1003
enum class MouseFormat : uint8_t { Default, Utf8, Sgr, Urxvt, SgrPixels };  // 1005, 1006, 1015, 1016

struct InputModes {
    bool cursor_keys_app = false;    // DECCKM
    bool keypad_app = false;         // DECKPAM / DECNKM
    bool bracketed_paste = false;    // 2004
    bool focus_events = false;       // 1004
    bool alt_scroll = false;         // 1007
    bool win32_input = false;        // 9001
    bool backarrow_sends_bs = false; // DECBKM (67)
    uint8_t modify_other_keys = 0;   // XTMODKEYS 4: 0, 1, 2
    MouseTracking mouse_tracking = MouseTracking::None;
    MouseFormat mouse_format = MouseFormat::Default;
    bool alt_screen = false;
    uint8_t kitty_flags = 0;         // stored now, used later

    friend bool operator==(const InputModes&, const InputModes&) = default;
};

enum class ClipboardSel : uint8_t { Clipboard, Primary, Selection };

enum class PromptMark : uint8_t { PromptStart, InputStart, OutputStart, CommandFinished };

enum class ProgressState : uint8_t { Remove, Normal, Error, Indeterminate, Paused };

struct CwdReport {
    enum class Form : uint8_t { Osc7, Osc9_9, Osc633 } form = Form::Osc7;
    std::u8string_view value;  // OSC 7: the file:// URI as sent; OSC 9;9 and 633: the path as sent
    bool side_channel = false; // came through feed_integration, not the PTY stream
};

// Theme colors the GUI sends (SET_OPTION theme.*), as 0xRRGGBB. The terminal answers
// OSC 4/10/11/12 queries from them; unset entries use the built-in defaults: fg #d0d0d0,
// bg #000000, cursor = fg, palette = the xterm 256-color palette.
struct ThemeColors {
    std::optional<uint32_t> fg, bg, cursor;
    std::array<std::optional<uint32_t>, 256> palette{};

    friend bool operator==(const ThemeColors&, const ThemeColors&) = default;
};

}  // namespace kt::vt
