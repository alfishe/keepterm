// The terminal state machine and screen model.
//
// A Terminal owns the parser, both screens, the scrollback, the style/link/grapheme tables and
// the frame publisher. feed() runs on the parsing thread only. Replies and events go to the
// TerminalSink, on the same thread.
//
// Current state of this class:
//   - resize() reflows the primary screen and the scrollback when the width changes;
//     a height-only change pushes rows above the cursor into the scrollback when it shrinks and
//     pulls them back when it grows. The alternate screen is clipped or padded.
//   - mode 2027 is stored and reported, but clustering is the "mode off" rule for now:
//     zero-width code points attach to the previous cell.
//   - snapshot/restore (KTSN, vt/snapshot.h), feed_integration and set_theme are here;
//     replay_vt, clipboard_reply, merge_scrollback and scrollback_view are not yet.
//   - OSC 4/10/11/12 set program overrides on top of set_theme (color_overrides); the frame
//     does not carry colors, the view reads effective_colors() after colors_changed().
#pragma once

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "vt/cell.h"
#include "vt/frame.h"
#include "vt/parser.h"
#include "vt/screen.h"
#include "vt/scrollback.h"
#include "vt/snapshot.h"
#include "vt/tables.h"
#include "vt/types.h"

namespace kt::vt {

// Callbacks run on the parsing thread. They must not block.
class TerminalSink {
public:
    virtual ~TerminalSink() = default;
    virtual void reply(std::span<const std::byte> bytes) = 0;  // DA, DSR, CPR, DECRQM, XTVERSION
    virtual void title_changed(std::u8string_view /*title*/) {}
    virtual void cwd_changed(const CwdReport& /*r*/) {}         // OSC 7
    virtual void bell() {}
    virtual void clipboard_write(ClipboardSel /*sel*/, std::span<const std::byte> /*data*/) {}
    virtual void clipboard_read_request(ClipboardSel /*sel*/) {}
    virtual void prompt_mark(PromptMark /*mark*/, uint64_t /*abs_line*/) {}  // OSC 133
    // OSC 133;C: the command line typed since the last B mark (or A when the shell sends no B),
    // read off the screen, rows joined, trimmed, at most 4 KiB. Empty when no mark was seen.
    virtual void command_started(std::u8string_view /*command_line*/) {}
    virtual void command_finished(std::optional<int> /*exit_code*/) {}
    virtual void notify(std::u8string_view /*title*/, std::u8string_view /*body*/) {}
    virtual void progress(ProgressState /*st*/, uint8_t /*percent*/) {}
    virtual void modes_changed(const InputModes& /*m*/) {}
    // A program changed or reset colors (OSC 4/10/11/12/104/110/111/112): effective_colors().
    virtual void colors_changed() {}
};

enum class ResetKind : uint8_t { Full, Soft };  // RIS, DECSTR

class Terminal final : private ParserHandler {
public:
    static Result<std::unique_ptr<Terminal>> create(GridSize, const TerminalOptions&, TerminalSink*);
    // Rebuilds a terminal from a KTSN snapshot. stream_offset() continues from the
    // snapshot's. scrollback_only and text_only snapshots give Errc::not_supported, a damaged one
    // Errc::corrupt. `opts.scrollback_lines` limits the restored scrollback.
    static Result<std::unique_ptr<Terminal>> restore(std::span<const std::byte> snapshot, const TerminalOptions&,
                                                     TerminalSink*, const RestoreOptions& = {});
    ~Terminal() override;
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Parsing thread only.
    void feed(std::span<const std::byte> bytes);  // any split; partial sequences are kept
    void feed(std::string_view bytes) { feed(std::as_bytes(std::span(bytes.data(), bytes.size()))); }
    void resize(GridSize);                        // reflows the primary screen (header comment)
    void reset(ResetKind);
    void clear_scrollback();
    void set_scrollback_limit(uint32_t lines);
    void set_scrollback_max_bytes(uint32_t bytes);
    // Bytes from the KEEPTERM_INTEGRATION side channel. A separate parser that
    // passes only OSC 7, OSC 9;9, OSC 133 and OSC 633 (each at most 4 KiB); everything else is dropped.
    // Never prints and does not change stream_offset(). cwd reports carry side_channel = true.
    void feed_integration(std::span<const std::byte> bytes);
    // Default colors for OSC 4/10/11/12 queries (SET_OPTION theme.* from the GUI). Kept in snapshots.
    void set_theme(const ThemeColors& theme);
    const ThemeColors& theme() const { return theme_; }
    // The colors programs set with OSC 4/10/11/12 (unset entries follow the theme); RIS and
    // OSC 104/110/111/112 clear them. Kept in snapshots.
    const ThemeColors& color_overrides() const { return color_overrides_; }
    // The theme with the program's overrides on top: what the renderer should draw with.
    ThemeColors effective_colors() const;
    // The cursor of a reset and of DECSCUSR 0 (terminal.cursor_style, terminal.cursor_blink).
    // Takes effect at once unless a program chose a style with DECSCUSR 1-6. Parsing thread.
    void set_default_cursor(CursorShape shape, bool blink);
    // Leaves the alternate screen and resets the modes that only make sense for a running
    // program: mouse tracking and format, bracketed paste, application keypad and cursor keys,
    // focus events, alternate scroll, win32 input, synchronized output; shows the cursor.
    // Crash recovery uses it after the journal replay.
    void reset_program_modes();

    // Serialization. Parsing thread only, between two feed() calls. Hands chunks of
    // 64 KiB to the sink as they are ready; the last call has last = true.
    Result<void> snapshot(const SnapshotOptions&, SnapshotChunkSink&) const;
    std::vector<std::byte> snapshot(const SnapshotOptions& = {}) const;
    // Publishes a frame if anything changed. feed() calls it at the end of every batch; the owner
    // calls it again to retry a skipped publish or to end a stalled synchronized update (2026).
    bool publish();
    // When the owner should call publish() again without new output: the end of the timeout of a
    // synchronized update (2026) that holds changes back, else `now + retry` when the last publish
    // was skipped (FramePublisher::pending); nullopt when nothing waits. feed() publishes on its
    // own, so this only covers output that stopped.
    std::optional<std::chrono::steady_clock::time_point> publish_due(std::chrono::steady_clock::time_point now,
                                                                     std::chrono::steady_clock::duration retry) const;

    uint64_t stream_offset() const { return stream_offset_; }
    const InputModes& input_modes() const { return modes_; }
    GridSize size() const { return size_; }
    std::u8string cursor_row_text(std::size_t max_bytes = 512) const;

    // The frame publisher. The first call attaches it; until then publish() is a no-op (host).
    FramePublisher& frames();

    // Read access on the parsing thread (tests, golden dumps, the host's CLI dumps).
    const Screen& screen() const { return *active_; }
    bool alt_active() const { return active_ != &primary_; }
    CursorState cursor() const;
    const StyleTable& styles() const { return styles_; }
    const LinkTable& links() const { return links_; }
    const GraphemePool& graphemes() const { return graphemes_; }
    // Collection passes run so far (tests: the passes stay O(1) per byte, collect_tables).
    uint64_t table_collections() const { return collections_; }
    const Scrollback& scrollback() const { return scrollback_; }
    uint64_t first_abs_line() const { return scrollback_.next_abs_line(); }
    std::u8string_view title() const { return title_; }
    std::u8string_view icon_name() const { return icon_name_; }
    std::u8string_view cwd() const { return cwd_; }
    // DEC private mode value as DECRQM reports it: 0 unknown, 1 set, 2 reset.
    int dec_mode(uint16_t mode) const;
    int ansi_mode(uint16_t mode) const;

private:
    enum class Charset : uint8_t { Ascii, DecGraphics, Uk };

    struct Pen {
        Style style;
        uint16_t index = 0;      // styles_ index of `style`
        uint16_t bce_index = 0;  // styles_ index of the erase style (background only)
        bool prot = false;       // DECSCA
    };

    struct SavedCursor {
        bool valid = false;
        uint16_t x = 0, y = 0;
        bool pending_wrap = false;
        Style style;
        bool prot = false;
        bool origin = false;
        std::array<Charset, 4> g{};
        uint8_t gl = 0;
    };

    Terminal(GridSize, const TerminalOptions&, TerminalSink*);

    // KTSN (vt/snapshot.cpp).
    friend struct SnapshotCodec;
    struct IntegrationHandler;

    // ParserHandler.
    void print_ascii(std::string_view run) override;
    void print(char32_t cp) override;
    void execute(uint8_t control) override;
    void esc_dispatch(const Sequence&) override;
    void csi_dispatch(const Sequence&) override;
    void osc_dispatch(std::string_view payload) override;
    void dcs_hook(const Sequence&) override;
    void dcs_put(uint8_t) override;
    void dcs_unhook() override;

    // Printing.
    void put_char(char32_t cp, int width);
    void attach_combining(char32_t cp);
    void wrap_line();
    char32_t map_charset(char32_t cp);

    // Cursor and margins.
    uint16_t line_left() const { return x_ >= left_ ? left_ : 0; }
    uint16_t line_right() const { return x_ <= right_ ? right_ : static_cast<uint16_t>(cols() - 1u); }
    bool lr_margins() const;
    bool in_region() const { return y_ >= top_ && y_ <= bottom_ && x_ >= left_ && x_ <= right_; }
    void move_to(int x, int y);  // absolute, clamped to the screen
    void cup(int row, int col);  // 0-based, origin mode aware
    void cursor_up(int n);
    void cursor_down(int n);
    void cursor_left(int n);
    void cursor_right(int n);
    void carriage_return();
    void index();
    void reverse_index();
    void backspace();
    void reverse_wrap_left(int n);  // BS and CUB with mode 45 or 1045
    void back_index();              // DECBI
    void forward_index();           // DECFI
    void save_dec_modes(const Params& p);     // XTSAVE (CSI ? Pm s)
    void restore_dec_modes(const Params& p);  // XTRESTORE (CSI ? Pm r)
    void tab_forward(int n);
    void tab_backward(int n);
    void save_cursor();
    void restore_cursor();
    void reset_margins();
    void resize_height(GridSize old, GridSize size);
    void reflow_primary(GridSize old, GridSize size);
    void reset_tabs();

    // Scrolling and editing.
    void scroll_up(uint16_t n);  // the scroll region; full-screen primary scrolls feed the scrollback
    void scroll_down(uint16_t n);
    void erase_display(int mode, bool selective);
    void erase_line(int mode, bool selective);
    void insert_lines(uint16_t n);
    void delete_lines(uint16_t n);
    void insert_chars(uint16_t n);
    void delete_chars(uint16_t n);
    void erase_chars(uint16_t n);
    void repeat_last(uint16_t n);
    void insert_columns(uint16_t n);  // DECIC
    void delete_columns(uint16_t n);  // DECDC
    void scroll_columns(uint16_t n, bool left);  // SL, SR
    void decaln();
    Cell blank() const { return Cell::blank(pen_.bce_index); }

    // Styles.
    void sgr(const Params& p);
    void update_pen();
    void collect_tables();
    // False while a collection pass would find the tables as full as the last one left them
    // (collect_tables comment): until collect_wait_ bytes have been fed since it.
    bool collect_due() const;

    // Modes.
    void set_dec_mode(uint16_t mode, bool on);
    void set_ansi_mode(uint16_t mode, bool on);
    void switch_screen(bool alt, bool clear_alt);
    void notify_modes();
    void soft_reset();
    void full_reset();

    // Replies and OSC.
    void reply(std::string_view s);
    void report_mode(const Sequence& s);
    void osc_hyperlink(std::string_view rest);
    void osc_clipboard(std::string_view rest);
    void osc_prompt(std::string_view rest);
    void osc_color(unsigned cmd, std::string_view rest);
    void osc_color_reset(unsigned cmd, std::string_view rest);
    void osc_vscode(std::string_view rest);
    void window_op(const Params& p);
    void rect_checksum(const Params& p);  // DECRQCRA
    void request_status(std::string_view what);   // DECRQSS
    void request_termcap(std::string_view hex);    // XTGETTCAP
    std::string sgr_string() const;
    void osc_notify(std::string_view rest);

    uint16_t cols() const { return size_.cols; }
    uint16_t rows() const { return size_.rows; }
    uint64_t cursor_abs_line() const { return scrollback_.next_abs_line() + y_; }
    std::u8string input_text() const;  // from the last A/B mark to the cursor (command_started)

    TerminalOptions opts_;
    TerminalSink* sink_;
    GridSize size_;
    Parser parser_;

    Screen primary_;
    std::unique_ptr<Screen> alt_;  // allocated on first use
    Screen* active_;
    Scrollback scrollback_;
    StyleTable styles_;
    LinkTable links_;
    GraphemePool graphemes_;

    // Where the command line starts: the cursor at the last OSC 133 B (or A) mark.
    uint64_t input_abs_ = UINT64_MAX;
    uint16_t input_col_ = 0;
    bool input_from_b_ = false;

    // Cursor.
    uint16_t x_ = 0, y_ = 0;
    bool pending_wrap_ = false;
    CursorShape shape_ = CursorShape::Block;
    CursorShape default_shape_ = CursorShape::Block;
    bool default_blink_ = false;
    bool shape_default_ = true;  // no DECSCUSR 1-6 since the last reset
    Pen pen_;
    uint16_t link_ = 0;  // current OSC 8 link
    std::array<Charset, 4> g_{};
    uint8_t gl_ = 0;
    uint8_t single_shift_ = 0;  // 2 or 3 for the next character, else 0
    std::array<SavedCursor, 2> saved_{};  // [0] primary, [1] alternate
    char32_t last_cp_ = 0;  // for REP

    // Margins (inclusive) and tabs.
    uint16_t top_ = 0, bottom_ = 0, left_ = 0, right_ = 0;
    std::vector<uint8_t> tabs_;

    // Modes. DEC private modes live in a fixed slot table (also the serialization order).
    static constexpr std::array<uint16_t, 32> kDecModes = {
        1, 3, 5, 6, 7, 9, 12, 25, 40, 45, 47, 66, 67, 69, 1000, 1002, 1003, 1004, 1005, 1006,
        1007, 1015, 1016, 1036, 1047, 1048, 1049, 2004, 2026, 2027, 2031, 9001};
    static constexpr int dec_slot(uint16_t mode) {
        for (std::size_t i = 0; i < kDecModes.size(); ++i) {
            if (kDecModes[i] == mode) return static_cast<int>(i);
        }
        return -1;
    }
    bool dec(uint16_t mode) const {
        const int s = dec_slot(mode);
        return s >= 0 && dec_[static_cast<std::size_t>(s)];
    }
    std::bitset<kDecModes.size()> dec_;
    bool insert_mode_ = false;  // IRM (ANSI 4)
    bool lnm_ = false;          // LNM (ANSI 20)
    InputModes modes_;
    bool sync_ = false;  // 2026
    std::chrono::steady_clock::time_point sync_since_{};

    std::u8string title_, icon_name_, cwd_;
    uint64_t stream_offset_ = 0;
    uint64_t collect_offset_ = 0;   // stream_offset_ at the last collection pass
    uint64_t collect_wait_ = 0;     // bytes to feed before the next one may run
    uint64_t collections_ = 0;
    ThemeColors theme_;
    std::unique_ptr<Parser> integ_parser_;  // feed_integration, created on first use
    bool integ_ = false;                    // osc_dispatch runs for the side channel

    // Frames.
    std::unique_ptr<FramePublisher> publisher_;
    CursorState last_cursor_{};
    InputModes last_modes_{};
    uint64_t last_styles_version_ = 0;
    bool force_publish_ = false;

    // Rarely used state, kept after the members that feed() touches.
    ThemeColors color_overrides_;
    // XTWINOPS 22/23: (title, icon name) pairs; xterm keeps 10.
    static constexpr std::size_t kTitleStackMax = 10;
    std::vector<std::pair<std::u8string, std::u8string>> title_stack_;
    // DCS being collected: 0 none, 1 DECRQSS, 2 XTGETTCAP.
    uint8_t dcs_kind_ = 0;
    std::string dcs_buf_;
    // Mode 1045, extended reverse wraparound (xterm 383). Outside the slot table: the table is
    // full (32 bits in the snapshot); MODES keeps it in a trailing byte.
    bool rev_wrap_ext_ = false;
    // XTSAVE: DEC modes saved by number (each saved mode at most once; xterm keeps one level).
    std::vector<std::pair<uint16_t, bool>> saved_dec_modes_;
};

}  // namespace kt::vt
