// The main window's view of the workspace model: implements
// ui::IWorkspaceView over ws::Workspace for one window. Core code (no OS calls), so the
// interaction tests drive the real model with a seeded random source and no session hosts.
//
// The snapshot is rebuilt lazily after a model change (the adapter subscribes to the change
// bus) or after the host reports new pane facts (titles, processes, resources) with
// facts_changed(). Model ids become 64-bit keys: the two halves of the 128-bit id xor-ed.
#pragma once

#include <functional>
#include <span>
#include <map>
#include <string>

#include "ui/workspace_view.h"
#include "workspace/workspace.h"

namespace kt::app {

struct WorkspaceAdapterHooks {
    // What the model does not know: live title, foreground program, agent, resources, exit.
    std::function<void(const ws::Pane&, ui::PaneItem&)> pane_facts;
    // "New Project…": the host asks for a folder.
    std::function<void()> new_project;
    std::function<void(const std::string&)> copy_text;
    std::function<bool(const std::string&)> open_folder;
    // A busy pane: ask, then run `force` (Macterm's "Close tab with a running program?").
    std::function<void(const std::string& question, std::function<void()> force)> confirm;
    // The program a busy pane runs ("vim"), "" when idle: names it in the close question.
    std::function<std::string(const ws::Pane&)> running;
    // A failed operation (English message).
    std::function<void(const std::string&)> report;
};

class WorkspaceAdapter final : public ui::IWorkspaceView {
public:
    WorkspaceAdapter(ws::Workspace& w, ws::WindowId window, WorkspaceAdapterHooks hooks = {});
    ~WorkspaceAdapter() override;

    ws::Workspace& model() { return ws_; }
    ws::WindowId window() const { return window_; }
    void set_hooks(WorkspaceAdapterHooks h) { hooks_ = std::move(h); }
    // Pane facts changed (title, resources ...): the next snapshot() rebuilds.
    void facts_changed() { dirty_ = true; }

    static uint64_t key_of(const ws::Id128& id);
    template <class Id>
    static uint64_t key(const Id& id) { return key_of(id.value); }
    std::optional<ws::PaneId> pane_id(ui::PaneKey) const;
    std::optional<ws::TabId> tab_id(ui::TabKey) const;
    std::optional<ws::ProjectId> project_id(ui::ProjectKey) const;

    const ui::WorkspaceSnapshot& snapshot() const override;

    void show_project(ui::ProjectKey) override;
    void set_expanded(ui::ProjectKey, bool) override;
    void new_project() override;
    void rename_project(ui::ProjectKey, std::string name) override;
    void set_project_color(ui::ProjectKey, ui::ProjectColor) override;
    void move_project(ui::ProjectKey, size_t to) override;
    void remove_project(ui::ProjectKey) override;

    void new_tab(ui::ProjectKey) override;
    void select_tab(ui::TabKey) override;
    void close_tab(ui::TabKey) override;
    void rename_tab(ui::TabKey, std::string title) override;
    void move_tab(ui::TabKey, ui::ProjectKey to_project, size_t to) override;

    void split(ui::PaneKey, ui::SplitDir) override;
    void dock_pane(ui::PaneKey, ui::PaneKey target, ui::SplitDir side) override;
    void close_pane(ui::PaneKey) override;
    void focus_pane(ui::PaneKey) override;
    void toggle_zoom(ui::PaneKey) override;
    void layout(ui::TabKey, ui::Rect area, float divider, std::vector<ui::PaneRect>& panes,
                std::vector<ui::DividerRect>& dividers) const override;
    void set_divider_ratio(ui::TabKey, uint64_t ref, float ratio) override;

    void set_sidebar(float width, bool visible) override;
    bool open_folder(const std::string& path) override { return hooks_.open_folder && hooks_.open_folder(path); }
    void copy_text(const std::string& s) override {
        if (hooks_.copy_text) hooks_.copy_text(s);
    }

private:
    std::string busy_question(const char* what, std::span<const ws::PaneId> panes) const;
    void rebuild() const;
    template <class T>
    bool ok(const ws::WsResult<T>& r) {
        if (r) return true;
        if (hooks_.report) hooks_.report(r.error().message);
        return false;
    }
    std::vector<ws::ProjectId> expanded_list() const;

    ws::Workspace& ws_;
    ws::WindowId window_;
    WorkspaceAdapterHooks hooks_;
    ws::Subscription sub_;
    mutable bool dirty_ = true;
    mutable ui::WorkspaceSnapshot snap_;
    mutable std::map<uint64_t, ws::ProjectId> projects_;
    mutable std::map<uint64_t, ws::TabId> tabs_;
    mutable std::map<uint64_t, ws::PaneId> panes_;
};

}  // namespace kt::app
