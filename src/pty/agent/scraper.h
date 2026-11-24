// Console agent scraper: console screen buffer -> VT.
//
// Portable: the console is reached through IConsole, so the diff logic runs and is tested on
// every OS with a fake console. The real console lives in console_agent_win.cpp.
//
// The scraper keeps a model of the host's screen: the window size, the buffer row that the
// host's top row shows (`top`), every cell, the SGR state, the cursor and its visibility.
//
//   - Main buffer (buffer taller than the window): when the window moved down by k rows, the
//     rows that left the window are emitted first (repainted if they changed, or painted if
//     the host never saw them) and scrolled off with k line feeds at the bottom row, so they
//     become host scrollback. The window moving up (cls) clears the host screen and repaints.
//   - Alternate buffer (buffer height == window height): CSI ? 1049 h/l on the switch, rows are
//     repainted in place, no scrollback.
//   - Per row only the changed column range is emitted; trailing blanks become EL.
//   - 16 colors: console attributes map to SGR 30-37/90-97 and 40-47/100-107; foreground 7
//     and background 0 are the defaults (SGR 39/49), reverse and underscore map to SGR 7 and 4.
//   - Wide characters: LEADING/TRAILING flags, the same code unit twice in a row, and
//     surrogate pairs in two cells are emitted once.
//   - The update is wrapped in CSI ? 2026 h/l (synchronized output).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kt::pty::agent {

// CHAR_INFO attribute bits (wincon.h), mirrored so this builds without <windows.h>.
inline constexpr uint16_t kAttrDefault = 0x0007;
inline constexpr uint16_t kAttrLeading = 0x0100;
inline constexpr uint16_t kAttrTrailing = 0x0200;
inline constexpr uint16_t kAttrReverse = 0x4000;
inline constexpr uint16_t kAttrUnderscore = 0x8000;

struct ConCell {
    char16_t ch = u' ';
    uint16_t attr = kAttrDefault;
    friend bool operator==(const ConCell&, const ConCell&) = default;
};

struct ConInfo {
    int16_t buf_w = 0, buf_h = 0;
    int16_t win_left = 0, win_top = 0, win_w = 0, win_h = 0;
    int16_t cur_x = 0, cur_y = 0;
    bool cur_visible = true;
    friend bool operator==(const ConInfo&, const ConInfo&) = default;
};

class IConsole {
public:
    virtual ~IConsole() = default;
    // The active screen buffer's geometry and cursor.
    virtual bool info(ConInfo& out) = 0;
    // Buffer rows [top, top + rows), columns [left, left + cols); out.size() == rows * cols.
    virtual bool read(int16_t top, int16_t rows, int16_t left, int16_t cols, std::span<ConCell> out) = 0;
    virtual std::u16string title() = 0;
};

class Scraper {
public:
    // Bytes the host gets once, before the first scrape (input mode requests).
    static std::string start();

    // Reads the console and returns the VT update against the host model; empty when nothing changed.
    std::string scrape(IConsole&);

    // Cheap check of the geometry, the cursor and the rows around it: true when scrape() is due.
    bool probe(IConsole&);

    // The next scrape clears the host screen and repaints the window (after a resize).
    void resync() { valid_ = false; }

    // The agent moved the window content to buffer row `top` itself (buffer recycling): the
    // host already shows these rows, so nothing is emitted.
    void rebase(int32_t top) { top_ = top; last_.win_top = static_cast<int16_t>(top); }

    bool alternate() const { return alt_; }
    uint64_t scrape_count() const { return scrapes_; }
    uint64_t change_count() const { return changes_; }

private:
    void reset_model(std::string& o, int w, int h, int32_t top);
    void emit_row(std::string& o, int y, const ConCell* now);
    void emit_text(std::string& o, const ConCell* row, int from, int to);
    void set_sgr(std::string& o, uint16_t attr);
    void cup(std::string& o, int y, int x);

    int w_ = 0, h_ = 0;
    int32_t top_ = 0;
    bool valid_ = false;
    bool alt_ = false;
    bool need_full_ = false;
    std::vector<ConCell> model_;
    std::vector<ConCell> scratch_;
    uint16_t sgr_ = kAttrDefault;
    int host_y_ = -1, host_x_ = -1;
    bool host_visible_ = true;
    std::u16string title_;
    ConInfo last_{};
    uint64_t scrapes_ = 0, changes_ = 0;
};

}  // namespace kt::pty::agent
