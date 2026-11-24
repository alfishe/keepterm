// The Settings window's logic: builds the panes of settings_layout.h into a
// ui::SettingsView, writes each change to the user's config.toml through config::ConfigWriter,
// and updates the controls in place when the config reloads (an edit in Settings or in an
// editor).
//
// Every change applies immediately, as in Macterm: the writer saves the key, the host reloads
// the config (ConfigService::reload_now) and the reload reaches the live window through the
// usual subscriptions, then refresh() here. A value the writer refuses shows its error under
// the control and the file stays as it was.
//
// The host supplies the environment (what the window knows: the theme library, the fonts, the
// renderer, the session store); tests supply fakes.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agents/context_hook.h"
#include "app/settings_layout.h"
#include "base/error.h"
#include "commands/keymap.h"
#include "config/config.h"
#include "config/writer.h"
#include "store/usage.h"
#include "ui/settings_view.h"

namespace kt::theme {
class ThemeLibrary;
}

namespace kt::app {

struct SettingsEnv {
    std::filesystem::path config_file;                 // ConfigWriter::target_file
    std::string config_file_display;                   // "~/.config/keepterm/config.toml"; "" = the path
    commands::Os os = commands::current_os();
    std::function<config::Config()> config;            // the current snapshot
    std::function<void()> written;                     // after each write: reload the config
    const theme::ThemeLibrary* themes = nullptr;
    bool os_dark = true;                               // the OS appearance (theme.mode = system)
    std::function<std::vector<std::string>()> font_families;
    std::function<std::string()> renderer;             // the active backend ("Metal")
    std::function<Result<store::Usage>()> storage_usage;
    std::function<Result<void>(const std::string& session, bool pinned)> pin_session;
    std::function<Result<void>(const std::string& session)> forget_session;
    std::function<void()> open_config_file;
    // Agents → "Claude Code: exact context via status line" (agents/context_hook.h): the hook the
    // toggle installs and uninstalls, the same code as `keepterm --agent-hook`. Null: the row
    // says it is not available.
    std::function<std::unique_ptr<agents::IStatusLineHook>()> statusline_hook;
};

class SettingsController {
public:
    explicit SettingsController(SettingsEnv env);
    ~SettingsController();
    SettingsController(const SettingsController&) = delete;
    SettingsController& operator=(const SettingsController&) = delete;

    // A new view on `pane` ("appearance"; "" = the first). The controller keeps a pointer to it
    // until closed() is called.
    std::unique_ptr<ui::SettingsView> make_view(std::string_view pane = {});
    // The host shows the view (ui::show_settings) and calls closed() when the overlay goes.
    void closed();
    ui::SettingsView* view() const { return view_; }

    void show_pane(std::string_view id);
    const std::string& pane() const { return pane_; }
    void search(const std::string& query);

    // The config changed (a reload): controls take the new values in place.
    void refresh(const config::Config& c);
    const config::Config& config() const { return cfg_; }
    config::ConfigWriter& writer() { return writer_; }

    // Close requested by the view (Escape, the close button).
    std::function<void()> on_close;

    // Writes one key; the error (also shown under the row) or ok.
    Result<void> set_value(config::KeyId k, const config::Value& v);
    Result<void> reset_value(config::KeyId k);
    // Keymaps: binds `command` to the chord of a recorded key, unbinds, resets to the default.
    Result<void> bind(std::string_view command, const platform::KeyEvent& e);
    Result<void> unbind(std::string_view command);
    Result<void> reset_binding(std::string_view command);
    // Agents: resume on/off for an agent id (agents.restore_skip).
    Result<void> set_agent_restore(std::string_view agent, bool on);
    // Agents: installs (on) or uninstalls the Claude Code statusLine relay; the error is also
    // shown under the row.
    Result<agents::HookStatus> set_claude_statusline(bool on);
    // Appearance: picks a theme for the side being edited (light or dark).
    Result<void> choose_theme(const std::string& name, bool dark_side);

    struct Bound;

private:
    std::unique_ptr<ui::Widget> build_pane(const settings::PaneDef& p);
    std::unique_ptr<ui::Widget> build_search(const std::string& q);
    std::unique_ptr<ui::FormRow> build_row(const settings::RowDef& r);
    void add_groups(ui::Widget& page, const settings::PaneDef& p, const std::string& filter);
    void add_appearance(ui::Widget& page);
    void add_keymaps(ui::Widget& page);
    void add_agents(ui::Widget& page);
    void add_storage(ui::Widget& page);
    void add_advanced(ui::Widget& page);
    void update_rows();
    void update_banner();
    void update_keymaps();
    void update_themes();
    void update_agents();
    void update_statusline_row(const agents::HookStatus& s);
    void rebuild_later();
    void write_done(config::KeyId k, const Result<void>& r);
    void show_page(std::unique_ptr<ui::Widget> w, std::string title, bool keep_scroll);

    SettingsEnv env_;
    config::Config cfg_;
    config::ConfigWriter writer_;
    commands::Keymap keymap_;
    ui::SettingsView* view_ = nullptr;
    std::string pane_, query_;
    std::vector<std::unique_ptr<Bound>> bound_;   // rows of the current page
    struct Theme;
    std::unique_ptr<Theme> theme_;                // the Appearance theme picker
    struct KeyRow {
        std::string command;
        ui::FormRow* row = nullptr;
        ui::KeyRecorder* rec = nullptr;
    };
    std::vector<KeyRow> key_rows_;
    std::string key_filter_;
    ui::Widget* key_list_ = nullptr;
    struct AgentRow {
        std::string id;
        ui::Toggle* toggle = nullptr;
        ui::FormRow* row = nullptr;
    };
    std::vector<AgentRow> agent_rows_;
    ui::Toggle* hook_toggle_ = nullptr;   // the Claude statusLine row
    ui::FormRow* hook_row_ = nullptr;
    bool read_only_ = false;
    std::string read_only_reason_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// "#RRGGBB" of a theme color.
std::string hex_of(std::uint32_t rgb);

}  // namespace kt::app
