// The output viewer: a window over the stored output of one session of the
// history, read-only. A find bar on top (the text, the match count, previous and next), a
// line saying what the window shows, and a terminal with the lines (PaneSession::show_lines),
// each command's line marked as a prompt, so Jump to Previous / Next Prompt goes from command
// to command. Selection, copy, Find, scrolling and the zoom keys work as in a pane.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/history_controller.h"
#include "app/pane_session.h"
#include "app/ui_window_host.h"
#include "session/history_output.h"
#include "text/font.h"
#include "ui/workspace_widgets.h"

namespace kt::ui {
class TextField;
class Label;
class Button;
}  // namespace kt::ui

namespace kt::app {

struct OutputViewerSpec {
    UiWindowSpec window;            // title, size, renderer, fonts, theme; the viewer sets keymap and scale_changed
    text::FontSpec font;            // the terminal font of the main window, at its size
    text::FontFiles files;
    float pad_x = 0, pad_y = 0;     // terminal padding (DIP)
    view::ViewOptions view;
    vt::TerminalOptions term;
    std::function<void(PaneSession&)> style;   // terminal colors and cursor, as the panes have them
    platform::IClipboard* clipboard = nullptr;
    // Export… (the find bar's button): the session's output in a format, the save chooser over
    // `view` (the viewer's window); null: no button.
    std::function<void(session::ExportFormat, void* view, ExportDone done)> export_output;
    // The command id the main window's keymap gives a key ("" for none).
    std::function<std::string(const ui::KeyEvent&)> command_of;
};

class OutputViewer final : public ui::ITerminalHost {
public:
    static Result<std::unique_ptr<OutputViewer>> open(platform::IPlatform&, OutputViewerSpec);
    ~OutputViewer() override;
    OutputViewer(const OutputViewer&) = delete;
    OutputViewer& operator=(const OutputViewer&) = delete;

    // Shows `o` (the session's output, or a new window of it around another line) and finds
    // `query` there, at the target line. The session's name is the window's title.
    void show(const session::HistoryOutput& o, const std::string& query);
    void activate();
    void close();
    bool is_open() const { return host_ && host_->open(); }
    UiWindowHost* host() { return host_.get(); }
    PaneSession* pane() { return pane_.get(); }

    // ui::ITerminalHost
    ui::ITerminalSurface* terminal(ui::PaneKey) override { return pane_.get(); }

    // The viewer's keys (the window's keymap): true when it took the key. Public for the tests.
    bool key(const ui::KeyEvent&);

private:
    OutputViewer(platform::IPlatform&, OutputViewerSpec);
    Result<void> init();
    bool load_font(float ui_scale);
    void find(const std::string& text, bool at_target);
    void step(int delta);
    void update_count();
    void ask_export();

    platform::IPlatform& p_;
    OutputViewerSpec spec_;
    PaneEnv env_;
    std::unique_ptr<UiWindowHost> host_;
    text::FontSet* font_ = nullptr;
    std::unique_ptr<PaneSession> pane_;
    ui::TextField* find_ = nullptr;
    ui::Label* count_ = nullptr;
    ui::Label* note_ = nullptr;
    ui::Widget* term_widget_ = nullptr;
    std::optional<uint64_t> target_line_;   // the absolute line the viewer opened at
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace kt::app
