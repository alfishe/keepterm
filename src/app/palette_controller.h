// The command palette of a main window: the palette engine of src/commands
// behind the PaletteView of src/ui. Opens over the window, searches as the user types, runs
// the chosen row through the window's command target, keeps scopes (Files, Tabs, ...).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "commands/palette.h"
#include "commands/target.h"
#include "ui/palette_view.h"
#include "workspace/workspace.h"

namespace kt::app {

struct PaletteEnv {
    ui::Window* window = nullptr;
    commands::ICommandTarget* target = nullptr;     // runs the rows (the window's target)
    const commands::Keymap* keymap = nullptr;
    const config::Config* config = nullptr;
    ws::Workspace* model = nullptr;
    ws::WindowId window_id;
    commands::palette::IUsageStore* usage = nullptr;   // null: an in-memory store
    std::function<commands::KeyContext()> key_context;   // the window's context before opening
    std::function<std::string(const ws::Tab&)> tab_title;
    std::function<void()> after_run;                // a row ran: re-sync the view, refocus
    std::function<std::int64_t()> now_ms;
    std::function<void(commands::palette::PaletteData&)> fill;   // adds what the model lacks (sessions)
};

class PaletteController {
public:
    explicit PaletteController(PaletteEnv env);
    ~PaletteController();

    bool is_open() const { return view_ != nullptr; }
    void open(std::optional<commands::PaletteScope> scope = std::nullopt);
    void close();
    void toggle();
    // palette.* navigation commands from the keymap (Ctrl+N, Ctrl+P ...).
    bool nav(commands::PaletteNav n);
    void fill_state(commands::TargetState& s) const;
    // palette.* settings or sources changed.
    void reconfigure();
    ui::PaletteView* view() const { return view_; }
    // Searches the typed text again (an asynchronous source answered).
    void requery() { refresh(); }

private:
    void refresh();
    void accept(size_t row, bool alt);
    void enter(size_t row);
    bool pop();
    commands::palette::SourceContext context() const;
    std::string scope_title() const;

    PaletteEnv env_;
    commands::palette::MemoryUsageStore memory_usage_;
    std::unique_ptr<commands::palette::IFileSystem> fs_;
    std::unique_ptr<commands::palette::Engine> engine_;
    commands::palette::PaletteData data_;
    std::vector<const commands::palette::Row*> rows_;   // into the engine's last result
    ui::PaletteView* view_ = nullptr;
    ui::WidgetId overlay_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace kt::app
