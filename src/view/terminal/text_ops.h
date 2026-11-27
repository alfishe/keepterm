// Text operations of the terminal view over the screen and the scrollback:
// selection, search, copy mode, URL detection.
//
// Everything works in absolute line coordinates (vt::Frame::first_abs_line + row), so a
// selection or a match stays valid while output scrolls. The owner gives access to the lines
// through a LineSource and keeps the terminal locked while a call reads it.
//
// Not here yet: PCRE2 regex search (literal, case-insensitive only), reflow remapping of a
// selection (a resize clears it), file path detection.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "view/terminal/terminal_view.h"
#include "vt/cell.h"

namespace kt::view {

struct SelPoint {
    uint64_t line = 0;   // absolute line
    uint16_t col = 0;
    friend constexpr auto operator<=>(const SelPoint&, const SelPoint&) = default;
};

// Read access to the lines of a terminal: scrollback lines, then the screen rows.
class LineSource {
public:
    virtual ~LineSource() = default;
    virtual uint64_t first_line() const = 0;   // oldest line available
    virtual uint64_t end_line() const = 0;     // one past the newest (the screen's last row)
    virtual uint16_t cols() const = 0;
    // Cells of a line (may be shorter than cols(): trailing blanks trimmed). Flags: vt::RowFlag.
    virtual std::span<const vt::Cell> cells(uint64_t line, uint16_t& flags) const = 0;
    // The code points of a grapheme cluster cell (Cell::cluster()).
    virtual std::u32string_view cluster(uint32_t index) const = 0;
    // The URI of an OSC 8 link index, "" when unknown.
    virtual std::string link_uri(uint16_t index) const = 0;
};

// A LineSource over a vt::Terminal (the caller holds the terminal's lock).
class TerminalLines final : public LineSource {
public:
    explicit TerminalLines(const vt::Terminal& t) : t_(t) {}
    uint64_t first_line() const override;
    uint64_t end_line() const override;
    uint16_t cols() const override;
    std::span<const vt::Cell> cells(uint64_t line, uint16_t& flags) const override;
    std::u32string_view cluster(uint32_t index) const override;
    std::string link_uri(uint16_t index) const override;

private:
    const vt::Terminal& t_;
};

// ---------------------------------------------------------------- selection

enum class SelMode : uint8_t { Char, Word, Line, Block };

struct Selection {
    SelPoint anchor, head;   // where the selection started and where it is now
    SelMode mode = SelMode::Char;
    bool active = false;

    // The covered range after word / line expansion, start <= end, both inclusive.
    void range(const LineSource&, SelPoint& start, SelPoint& end) const;
    bool empty() const { return !active || (mode == SelMode::Char && anchor == head); }
};

// The default word characters besides letters and digits.
inline constexpr std::u32string_view kWordChars = U"_-.~/:@%+#?&=";
bool is_word_char(char32_t c);

SelPoint word_start(const LineSource&, SelPoint p);
SelPoint word_end(const LineSource&, SelPoint p);
// The first and last line of the logical line through `line` (soft wraps joined).
uint64_t logical_start(const LineSource&, uint64_t line);
uint64_t logical_end(const LineSource&, uint64_t line);

// The selected text: trailing blanks trimmed per row, soft wraps joined, "\n" between lines.
std::string selection_text(const LineSource&, const Selection&);
// Highlights for the selection (one entry per line, clipped to the source's lines).
void selection_highlights(const LineSource&, const Selection&, uint32_t background, std::optional<uint32_t> foreground,
                          std::vector<Highlight>& out);

// ---------------------------------------------------------------- search

struct Match {
    SelPoint start;          // first cell
    SelPoint end;            // last cell (inclusive)
};

// Literal, case-insensitive (ASCII, Latin-1, Greek, Cyrillic) search over logical lines, oldest
// first. Stops after `limit` matches.
std::vector<Match> find_all(const LineSource&, std::string_view query_utf8, size_t limit = 10000);
// The same with a PCRE2 regular expression (case-insensitive, Unicode), within each logical line;
// empty matches are skipped. The pattern is untrusted (typed, pasted): every resource has a limit,
// so no pattern crashes, hangs or eats memory.
//   - compiling: its length, the nesting of parentheses and the compiled size; a pattern over
//     them, or not valid (including invalid UTF-8 and \C), is an error saying why;
//   - matching: per line, the backtracking steps, the depth and the heap; a line's length (one
//     pattern item can scan a whole line between two looks at the clock); and the whole search
//     a wall-clock budget, checked inside a match too (automatic callouts), so one line that
//     backtracks forever ends it.
// A limit hit while matching keeps what was found: `stopped` says why it is not all.
struct RegexLimits {
    size_t max_pattern_bytes = 1024;
    uint32_t max_nesting = 32;
    size_t max_compiled_bytes = 64 * 1024;
    uint32_t match_steps = 1'000'000;   // per line
    uint32_t depth = 10'000;            // backtracking frames, per line
    uint32_t heap_kib = 8 * 1024;       // per line
    size_t max_line_bytes = 256 * 1024;  // a longer line is searched in its first ones
    std::chrono::milliseconds budget{150};
};
struct RegexFound {
    std::vector<Match> matches;
    std::string stopped;   // "" when the search covered every line
};
Result<RegexFound> find_all_regex(const LineSource&, std::string_view pattern_utf8, size_t limit = 10000,
                                  const RegexLimits& limits = {});
void match_highlights(const LineSource&, const std::vector<Match>&, std::optional<size_t> current, uint32_t background,
                      uint32_t current_background, std::vector<Highlight>& out);

// ---------------------------------------------------------------- links

// The URL under a cell: the OSC 8 link of the cell first, else a detected URL (http, https,
// ftp, file, ssh, git, mailto:, www.) in the logical line. "" when there is none.
std::string url_at(const LineSource&, SelPoint p, Match* where = nullptr);
// Why a click on a link should ask first, "" when it opens at once. `shown` is the text of the
// link's cells (an OSC 8 link's label; a detected URL shows itself). Asks for: a local file or a
// program (file:), another program (ssh:, git:), a user name before the host
// ("https://bank.com@evil.example"), a host with non-ASCII or punycode labels, and a label that
// names another host than the target.
std::string link_risk(std::string_view url, std::string_view shown);
// A file path in the text under a cell (Cmd/Ctrl+click): the run of path characters around the
// cell, with a trailing ":line[:col]" or "(line[,col])" taken apart and trailing punctuation
// dropped. Only the shape is checked (a '/', '\\', '~' or a "name.ext"); the caller resolves it
// against the pane's folder and keeps it only when the file exists.
struct PathRef {
    std::string path;   // as written, UTF-8
    uint32_t line = 0, col = 0;   // 0 = not given
    bool operator==(const PathRef&) const = default;
};
std::optional<PathRef> path_at(const LineSource&, SelPoint p, Match* where = nullptr);
std::optional<PathRef> parse_path_token(std::u32string_view token);   // for tests

// The URLs a text contains (for tests and "show links").
std::vector<std::string> detect_urls(std::u32string_view text);

// ---------------------------------------------------------------- copy mode

// Copy mode: a keyboard cursor over the lines, vim keys. The owner feeds keys
// and reads the cursor, the selection and the requested viewport.
class CopyMode {
public:
    enum class Action : uint8_t { None, Moved, Copy, Exit, Search, SearchBack, Next, Previous };

    void begin(const LineSource&, SelPoint cursor, uint16_t rows, uint64_t top_line);
    bool active() const { return active_; }
    void end() { active_ = false; sel_ = {}; }
    SelPoint cursor() const { return cur_; }
    const Selection& selection() const { return sel_; }
    // Keys as text ("j", "G", "\x04" for Ctrl-d ...) or named keys through `key`.
    Action key(const LineSource&, const KeyInput&);
    // The top line of the viewport that keeps the cursor visible.
    uint64_t top_line() const { return top_; }
    void set_viewport(uint64_t top, uint16_t rows) { top_ = top; rows_ = rows; }
    void move_to(const LineSource&, SelPoint p);

private:
    Action motion(const LineSource&, char32_t c, uint32_t count, bool ctrl);
    void clamp(const LineSource&);
    void follow();
    bool active_ = false;
    SelPoint cur_;
    Selection sel_;
    uint64_t top_ = 0;
    uint16_t rows_ = 24;
    uint32_t count_ = 0;
    char32_t pending_ = 0;   // 'g' of "gg"
};

}  // namespace kt::view
