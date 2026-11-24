// The commands' adapter to the workspace model: one per window.
//
// WorkspaceTarget runs the model operations of the catalog (tabs, panes, projects, palette rows)
// on ws::Workspace for its window, and forwards the rest (terminal, view, app) to the window's
// own target. state() is the window's state with the model's facts on top.
//
//     kt::commands::WorkspaceTarget target(ws, window_id, &window_view_target);
//     kt::commands::CommandContext ctx{target, &cfg, key_context, InvokeSource::Keymap};
//     registry.invoke("pane.split_right", ctx);
//
// Close and remove return Errc::busy when a pane runs a program (ws::WsErrc::Busy): the UI
// asks, then invokes again with ctx.force (the catalog passes it as `force`).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "commands/forwarding_target.h"
#include "commands/palette.h"
#include "workspace/usage.h"
#include "workspace/workspace.h"

namespace kt::commands {

class WorkspaceTarget : public ForwardingTarget {
public:
    WorkspaceTarget(ws::Workspace& w, ws::WindowId window, ICommandTarget* view = nullptr);

    void set_window(ws::WindowId w) { window_ = w; }
    ws::WindowId window() const { return window_; }

    TargetState state() const override;

    Status new_tab(bool scratch) override;
    Status close_tab(bool force) override;
    Status select_tab_relative(int delta, bool across_projects) override;
    Status select_tab_index(std::uint32_t index) override;
    Status move_tab(int delta) override;
    Status move_tab_to_project(std::string_view project_id) override;
    Status separate_all_panes() override;
    Status set_tab_pinned(bool pinned) override;

    Status close_pane(bool force) override;
    Status split_pane(SplitDir d) override;   // Auto: the view decides (it knows the pane's size)
    Status separate_pane() override;
    Status toggle_zoom() override;
    Status equalize_panes() override;
    Status focus_pane(Dir d) override;
    Status focus_pane_cycle(int delta) override;
    Status resize_pane(Dir d) override;
    Status move_pane(Dir d) override;
    Status set_pane_private(bool on) override;
    Status set_pane_read_only(bool on) override;

    Status remove_project(bool force) override;
    Status select_project_relative(int delta) override;
    Status close_window() override;

    Status activate_tab(std::string_view tab_id) override;
    Status move_tab_here(std::string_view tab_id) override;
    Status open_project(std::string_view project_id, bool new_window) override;
    Status open_folder(std::string_view path, FolderAction a) override;

private:
    const ws::WindowState* win() const { return ws_.window(window_); }
    const ws::Tab* tab() const { return ws_.shown_tab(window_); }
    ws::ProjectId shown() const;

    ws::Workspace& ws_;
    ws::WindowId window_;
};

// Palette usage (frecency) read from and recorded into the workspace's palette_usage.json.
class WorkspaceUsage final : public palette::IUsageStore {
public:
    explicit WorkspaceUsage(ws::UsageStore& s) : s_(s) {}
    std::span<const std::int64_t> timestamps(std::string_view key) const override;
    void record(std::string_view key, std::int64_t now_ms) override;   // the store's clock wins

private:
    ws::UsageStore& s_;
};

// The palette's data snapshot for a window. `tab_title` gives a tab's shown title (the view
// knows the live terminal title); unset: the user title or "Tab N".
using TabTitleFn = std::function<std::string(const ws::Tab&)>;
palette::PaletteData palette_data(const ws::Workspace& w, ws::WindowId window, const TabTitleFn& tab_title = {});

}  // namespace kt::commands
