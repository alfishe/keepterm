// One terminal pane of the main window: the Terminal (mirror of a session host, or the owner
// of a local PTY), its session link and its TerminalView, drawn into the window's draw list as
// a ui::ITerminalSurface.
//
// Threads as in the single-pane GUI: the link's I/O thread feeds the
// Terminal and publishes frames; the UI thread acquires them in paint(). The Terminal is
// touched by the UI thread only under term_mu_ (resize, scrollback reads, snapshot swaps).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/session_link.h"
#include "platform_gui/clipboard.h"
#include "platform_gui/event_loop.h"
#include "ui/workspace_view.h"
#include "view/terminal/atlas_textures.h"
#include "view/terminal/terminal_view.h"
#include "view/terminal/text_ops.h"
#include "vt/terminal.h"

namespace kt::app {

// What the panes of one window share (UI thread).
struct PaneEnv {
    platform::IEventLoop* loop = nullptr;
    platform::IClipboard* clipboard = nullptr;
    std::function<void()> request_frame;
    std::function<void()> consume_key;   // the platform window's consume_key()
    std::function<bool()> copy_on_select;   // terminal.copy_on_select, read at each mouse selection
    float scale = 1;
};

// Pane events, delivered on the UI thread.
struct PaneEvents {
    std::function<void()> title_changed;
    std::function<void(const std::string& path, const std::string& source)> cwd;
    std::function<void(int code)> exited;
    std::function<void(const std::string& why)> host_lost;
    std::function<void()> first_snapshot;
    std::function<void()> bell;
    std::function<void(std::optional<int> code)> command_finished;
    std::function<void(const std::string& title, const std::string& body)> notify;   // OSC 9 / 777
    // Cmd/Ctrl+click on a link; `shown`: the text of the link's cells.
    std::function<void(const std::string& url, const std::string& shown)> open_url;
    // Cmd/Ctrl+click on a file path: `resolve` maps the path as written to an existing file's
    // absolute path ("" = no such file; also used for the hover preview), `open_path` opens it.
    std::function<std::string(const std::string& path)> resolve_path;
    std::function<void(const std::string& abs, uint32_t line, uint32_t col)> open_path;
    std::function<void()> copy_mode_changed;
};

class PaneSession final : public ui::ITerminalSurface, public vt::TerminalSink, public SessionEvents {
public:
    PaneSession(PaneEnv& env, ui::PaneKey key, text::FontSet& font, view::AtlasTextures& tex, view::ViewOptions vo,
                vt::TerminalOptions to, PaneEvents events);
    ~PaneSession() override;
    PaneSession(const PaneSession&) = delete;
    PaneSession& operator=(const PaneSession&) = delete;

    ui::PaneKey key() const { return key_; }
    // Attaches to a session host (mirror) or starts a local PTY. UI thread.
    Result<void> attach(const std::string& session_id, const std::string& endpoint);
    Result<void> start_local(pty::SpawnSpec spec);
    bool linked() const;
    // Detaches (a session keeps running in its host) or ends the local program.
    void close();
    const std::string& session_id() const { return session_; }

    // Font, padding (DIP) and colors.
    void set_font_set(text::FontSet&, float pad_x_dip, float pad_y_dip);
    void set_theme(const view::Theme&);
    void set_scale(float s) { env_.scale = s; }
    std::string title() const;
    bool exited() const { return exited_; }
    std::optional<int> exit_code() const { return exit_code_; }
    vt::ThemeColors effective_colors() const;
    // terminal.cursor_style / terminal.cursor_blink: the cursor until the program sets one
    // (DECSCUSR). Kept and re-applied to every terminal a snapshot brings.
    void set_cursor_defaults(vt::CursorShape shape, bool blink);
    view::TerminalView& view() { return *tv_; }

    // pane.read_only: typing, paste, IME text and mouse reports are dropped; selection, copy,
    // scrolling and find still work. Terminal replies (DA, focus reports) still go out.
    void set_read_only(bool on) { read_only_ = on; }
    bool read_only() const { return read_only_; }

    // Commands of the terminal.
    void paste(std::u8string_view text);
    void scroll_page(int pages);   // +1 = toward older output
    void scroll_to_top();
    void scroll_to_bottom();
    // Scrolls the previous (delta < 0) or next prompt (OSC 133 A) to the top of the view,
    // counted from the view's top line; past the last prompt, to the live screen. False when the
    // terminal has no prompt marks (no shell integration) or shows the alternate screen.
    bool jump_to_prompt(int delta);
    void clear_scrollback();
    void send(std::string_view bytes);

    // Selection: drag, double-click word, triple-click line, Alt+drag block.
    bool has_selection() const { return !sel_.empty(); }
    std::string selection_text() const;
    void select_all();
    void clear_selection();
    // Find: literal, case-insensitive; returns the match count. `step` moves the current
    // match (+1 newer, -1 older) and scrolls it into view.
    size_t find(const std::string& query);
    void find_step(int delta);
    void end_find();
    std::optional<size_t> find_current() const { return cur_match_; }
    size_t find_count() const { return matches_.size(); }
    // Copy mode: vim keys over the scrollback; y / Enter copy, q / Esc leave.
    void toggle_copy_mode();
    bool in_copy_mode() const { return copy_.active(); }

    // ui::ITerminalSurface (UI thread).
    void set_bounds(render::IRect px) override;
    void paint(render::DrawList& out, bool full, std::vector<render::IRect>& damage) override;
    bool dirty() const override { return dirty_.load(); }
    // animations.smooth_scrolling where the window can afford it (MainWindow decides).
    void set_smooth_scroll(bool on, bool snap) {
        tv_->set_smooth_scroll(on, snap);
        changed();
    }
    void set_focused(bool) override;
    void key(const ui::KeyEvent&) override;
    bool wants_mouse() const override;
    std::string hover_link() const override { return hover_url_; }
    ui::ScrollMetrics scroll_metrics() const override;
    void scroll_to_offset(uint32_t rows_up) override;
    // The text of the screen (`scrollback`: plus the scrollback), the last `max_lines` lines
    // above the empty rows at the bottom (0 = no limit), trailing blanks trimmed, soft wraps joined. For `keepterm pane dump`.
    std::string dump_text(bool scrollback, std::size_t max_lines) const;
    // `keepterm pane key`: a key as if typed, through the keyboard's encoder. False when the key
    // produces no bytes in the current modes.
    bool send_key(const view::KeyInput& k);
    struct GridInfo {
        uint16_t cols = 0, rows = 0, cursor_col = 0, cursor_row = 0;
        uint64_t scrollback_lines = 0;
        uint32_t scroll_offset = 0;   // rows the view is scrolled up from the live screen
        bool alt_screen = false;
    };
    GridInfo grid_info() const;   // for `keepterm pane inspect`
    // Terminals replaced by a snapshot and not freed yet (only while a paint draws from one).
    std::size_t retired_terminals() const;
    void text(std::string_view) override;
    void pointer(const ui::PointerEvent&, ui::Vec2 local_dip) override;
    bool wheel(const ui::WheelEvent&, ui::Vec2 local_dip) override;
    ui::Rect caret_rect_dip() const override;
    void preedit(std::string_view text, int caret) override;

    // vt::TerminalSink (I/O thread).
    void reply(std::span<const std::byte> bytes) override;
    void title_changed(std::u8string_view title) override;
    void bell() override;
    void clipboard_write(vt::ClipboardSel, std::span<const std::byte> data) override;
    void modes_changed(const vt::InputModes& m) override;
    void command_finished(std::optional<int> exit_code) override;
    void notify(std::u8string_view title, std::u8string_view body) override;

    // SessionEvents (I/O thread).
    void on_output(std::span<const std::byte> bytes) override;
    void on_exit(int code) override;
    void on_snapshot(std::vector<std::byte> ktsn, bool resync) override;
    void on_resized(pty::GridSize size) override;
    void on_cwd(const std::string& path, const std::string& source) override;
    void on_host_lost(const std::string& why) override;

private:
    void post(std::function<void()> fn);
    void wake();
    void schedule_publish_retry();
    void relayout(bool force_link);
    vt::InputModes modes();
    static pty::GridSize pty_size(const vt::GridSize& g);
    view::SelPoint point_at(ui::Vec2 window_dip) const;   // caller holds term_mu_
    uint64_t top_line() const;                            // caller holds term_mu_
    void update_highlights();                             // caller holds term_mu_
    void reveal(uint64_t line);                           // caller holds term_mu_
    void copy_text(const std::string& s);
    void changed();

    PaneEnv& env_;
    ui::PaneKey key_;
    PaneEvents events_;
    vt::TerminalOptions term_opts_;
    std::unique_ptr<view::TerminalView> tv_;
    render::IRect bounds_{0, 0, 0, 0};
    bool focused_ = false;
    bool full_next_ = true;
    std::atomic<bool> dirty_{true};

    mutable std::mutex term_mu_;
    std::unique_ptr<vt::Terminal> term_;
    // Terminals replaced by a snapshot while paint() drew from them (it reads the frames outside
    // term_mu_). Under term_mu_; freed as soon as no paint uses them, also for panes that are not
    // drawn (background tabs), which resync again and again under a flood.
    std::vector<std::unique_ptr<vt::Terminal>> graveyard_;
    const vt::Terminal* painting_ = nullptr;   // under term_mu_: the terminal paint() draws from
    // The terminal's title, kept apart from term_mu_: the window reads every pane's title on each
    // workspace sync, and term_mu_ is held by the I/O thread for whole output batches.
    mutable std::mutex title_mu_;
    std::string title_;
    std::atomic<bool> title_posted_{false};   // a title event is queued for the UI thread
    void set_title(std::string t);            // any thread; posts one event per change burst
    vt::GridSize grid_{};
    mutable std::mutex modes_mu_;
    vt::InputModes modes_;

    mutable std::mutex link_mu_;
    std::unique_ptr<ISessionLink> link_;
    std::atomic<bool> mirror_{false};
    std::string session_;
    bool read_only_ = false;
    std::optional<view::PathRef> hover_path_;   // waits for resolve_path after the lock
    bool exited_ = false;
    std::optional<int> exit_code_;
    bool first_snapshot_seen_ = false;

    view::Selection sel_;
    bool selecting_ = false;
    std::string hover_url_;                     // the link under the pointer (UI thread)
    std::optional<view::SelPoint> hover_at_;
    std::vector<view::Match> matches_;
    std::optional<size_t> cur_match_;
    view::CopyMode copy_;

    std::atomic<bool> wake_posted_{false}, retry_posted_{false};
    platform::TimerId retry_timer_ = 0;
    // Cursor blink (Macterm: a filled block that blinks while the pane has focus). Input and
    // focus restart the phase with the cursor shown.
    void restart_blink();
    void stop_blink();
    void arm_blink();
    platform::TimerId blink_timer_ = 0;
    vt::CursorShape cursor_shape_ = vt::CursorShape::Block;
    bool cursor_blink_ = true;
    bool blink_hidden_ = false;
    platform::Clock::time_point retry_due_{};
    // Callbacks posted to the loop check this before touching the pane.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace kt::app
