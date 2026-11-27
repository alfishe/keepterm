// An ICommandTarget that forwards every operation to another one (the view, the window).
// Integrations derive from it and override what they own: WorkspaceTarget takes the model
// operations and leaves the terminal and window ones to the view.
#pragma once

#include "commands/target.h"

namespace kt::commands {

class ForwardingTarget : public ICommandTarget {
public:
    explicit ForwardingTarget(ICommandTarget* inner = nullptr) : inner_(inner) {}
    void set_inner(ICommandTarget* inner) { inner_ = inner; }
    ICommandTarget* inner() const { return inner_; }

    TargetState state() const override { return inner_ ? inner_->state() : TargetState{}; }
    void notify(std::string_view message) override { if (inner_) inner_->notify(message); }
    Status toggle_palette() override { return inner_ ? inner_->toggle_palette() : no("toggle palette"); }
    Status open_palette_scope(PaletteScope s) override { return inner_ ? inner_->open_palette_scope(s) : no("open palette scope"); }
    Status palette_nav(PaletteNav n) override { return inner_ ? inner_->palette_nav(n) : no("palette nav"); }
    Status new_tab(bool scratch) override { return inner_ ? inner_->new_tab(scratch) : no("new tab"); }
    Status close_tab(bool force) override { return inner_ ? inner_->close_tab(force) : no("close tab"); }
    Status begin_rename_tab() override { return inner_ ? inner_->begin_rename_tab() : no("begin rename tab"); }
    Status select_tab_relative(int delta, bool across_projects) override { return inner_ ? inner_->select_tab_relative(delta, across_projects) : no("select tab relative"); }
    Status select_recent_tab() override { return inner_ ? inner_->select_recent_tab() : no("select recent tab"); }
    Status select_tab_index(std::uint32_t index) override { return inner_ ? inner_->select_tab_index(index) : no("select tab index"); }
    Status move_tab(int delta) override { return inner_ ? inner_->move_tab(delta) : no("move tab"); }
    Status move_tab_to_project(std::string_view project_id) override { return inner_ ? inner_->move_tab_to_project(project_id) : no("move tab to project"); }
    Status separate_all_panes() override { return inner_ ? inner_->separate_all_panes() : no("separate all panes"); }
    Status set_tab_pinned(bool pinned) override { return inner_ ? inner_->set_tab_pinned(pinned) : no("set tab pinned"); }
    Status close_pane(bool force) override { return inner_ ? inner_->close_pane(force) : no("close pane"); }
    Status split_pane(SplitDir d) override { return inner_ ? inner_->split_pane(d) : no("split pane"); }
    Status separate_pane() override { return inner_ ? inner_->separate_pane() : no("separate pane"); }
    Status toggle_zoom() override { return inner_ ? inner_->toggle_zoom() : no("toggle zoom"); }
    Status equalize_panes() override { return inner_ ? inner_->equalize_panes() : no("equalize panes"); }
    Status focus_pane(Dir d) override { return inner_ ? inner_->focus_pane(d) : no("focus pane"); }
    Status focus_pane_cycle(int delta) override { return inner_ ? inner_->focus_pane_cycle(delta) : no("focus pane cycle"); }
    Status resize_pane(Dir d) override { return inner_ ? inner_->resize_pane(d) : no("resize pane"); }
    Status move_pane(Dir d) override { return inner_ ? inner_->move_pane(d) : no("move pane"); }
    Status copy_session_id() override { return inner_ ? inner_->copy_session_id() : no("copy session id"); }
    Status capture_pane() override { return inner_ ? inner_->capture_pane() : no("capture pane"); }
    Status set_pane_read_only(bool on) override { return inner_ ? inner_->set_pane_read_only(on) : no("set pane read only"); }
    Status set_pane_private(bool on) override { return inner_ ? inner_->set_pane_private(on) : no("set pane private"); }
    Status restart_session() override { return inner_ ? inner_->restart_session() : no("restart session"); }
    Status copy() override { return inner_ ? inner_->copy() : no("copy"); }
    Status paste() override { return inner_ ? inner_->paste() : no("paste"); }
    Status select_all() override { return inner_ ? inner_->select_all() : no("select all"); }
    Status open_find() override { return inner_ ? inner_->open_find() : no("open find"); }
    Status find_step(int delta) override { return inner_ ? inner_->find_step(delta) : no("find step"); }
    Status clear_screen_and_scrollback() override { return inner_ ? inner_->clear_screen_and_scrollback() : no("clear screen and scrollback"); }
    Status change_font_size(int delta) override { return inner_ ? inner_->change_font_size(delta) : no("change font size"); }
    Status scroll(ScrollTo to) override { return inner_ ? inner_->scroll(to) : no("scroll"); }
    Status jump_to_prompt(int delta) override { return inner_ ? inner_->jump_to_prompt(delta) : no("jump to prompt"); }
    Status toggle_copy_mode() override { return inner_ ? inner_->toggle_copy_mode() : no("toggle copy mode"); }
    Status show_send_next_key_pill() override { return inner_ ? inner_->show_send_next_key_pill() : Status{}; }
    Status send_text(std::string_view text) override { return inner_ ? inner_->send_text(text) : no("send text"); }
    Status send_key(const Sequence& key) override { return inner_ ? inner_->send_key(key) : no("send key"); }
    Status open_project_dialog() override { return inner_ ? inner_->open_project_dialog() : no("open project dialog"); }
    Status begin_rename_project() override { return inner_ ? inner_->begin_rename_project() : no("begin rename project"); }
    Status remove_project(bool force) override { return inner_ ? inner_->remove_project(force) : no("remove project"); }
    Status replace_project_path() override { return inner_ ? inner_->replace_project_path() : no("replace project path"); }
    Status apply_layout() override { return inner_ ? inner_->apply_layout() : no("apply layout"); }
    Status save_layout() override { return inner_ ? inner_->save_layout() : no("save layout"); }
    Status save_workspace() override { return inner_ ? inner_->save_workspace() : no("save workspace"); }
    Status save_workspace_as() override { return inner_ ? inner_->save_workspace_as() : no("workspace presets"); }
    Status open_workspace() override { return inner_ ? inner_->open_workspace() : no("workspace presets"); }
    Status manage_workspaces() override { return inner_ ? inner_->manage_workspaces() : no("workspace presets"); }
    Status open_workspace_named(std::string_view name) override { return inner_ ? inner_->open_workspace_named(name) : no("workspace presets"); }
    Status start_clean() override { return inner_ ? inner_->start_clean() : no("start clean"); }
    Status restore_last_workspace() override { return inner_ ? inner_->restore_last_workspace() : no("restore last workspace"); }
    Status export_workspace() override { return inner_ ? inner_->export_workspace() : no("export workspace"); }
    Status import_workspace() override { return inner_ ? inner_->import_workspace() : no("import workspace"); }
    Status show_sessions() override { return inner_ ? inner_->show_sessions() : no("sessions overview"); }
    Status show_session_history() override { return inner_ ? inner_->show_session_history() : no("session history"); }
    Status reveal_in_history(std::string_view id) override { return inner_ ? inner_->reveal_in_history(id) : no("session history"); }
    Status open_history_session(std::string_view id) override { return inner_ ? inner_->open_history_session(id) : no("session history"); }
    Status open_history_output(std::string_view id, std::string_view q, const std::optional<HistoryLine>& line) override {
        return inner_ ? inner_->open_history_output(id, q, line) : no("session history");
    }
    Status select_project_relative(int delta) override { return inner_ ? inner_->select_project_relative(delta) : no("select project relative"); }
    Status toggle_sidebar() override { return inner_ ? inner_->toggle_sidebar() : no("toggle sidebar"); }
    Status toggle_resources() override { return inner_ ? inner_->toggle_resources() : no("toggle resources"); }
    Status toggle_title_bar() override { return inner_ ? inner_->toggle_title_bar() : no("toggle title bar"); }
    Status toggle_fullscreen() override { return inner_ ? inner_->toggle_fullscreen() : no("toggle fullscreen"); }
    Status new_window() override { return inner_ ? inner_->new_window() : no("new window"); }
    Status close_window() override { return inner_ ? inner_->close_window() : no("close window"); }
    Status sidebar_nav(SidebarNav n) override { return inner_ ? inner_->sidebar_nav(n) : no("sidebar nav"); }
    Status open_settings() override { return inner_ ? inner_->open_settings() : no("open settings"); }
    Status reload_config() override { return inner_ ? inner_->reload_config() : no("reload config"); }
    Status open_config_file() override { return inner_ ? inner_->open_config_file() : no("open config file"); }
    Status toggle_quick_terminal() override { return inner_ ? inner_->toggle_quick_terminal() : no("toggle quick terminal"); }
    Status quit(bool end_sessions) override { return inner_ ? inner_->quit(end_sessions) : no("quit"); }
    Status activate_tab(std::string_view tab_id) override { return inner_ ? inner_->activate_tab(tab_id) : no("activate tab"); }
    Status move_tab_here(std::string_view tab_id) override { return inner_ ? inner_->move_tab_here(tab_id) : no("move tab here"); }
    Status open_project(std::string_view project_id, bool new_window) override { return inner_ ? inner_->open_project(project_id, new_window) : no("open project"); }
    Status open_folder(std::string_view path, FolderAction a) override { return inner_ ? inner_->open_folder(path, a) : no("open folder"); }
    Status attach_session(std::string_view session_id) override { return inner_ ? inner_->attach_session(session_id) : no("attach session"); }
    Status kill_session(std::string_view session_id) override { return inner_ ? inner_->kill_session(session_id) : no("kill session"); }
    Status open_file(std::string_view path, bool with_os_default) override { return inner_ ? inner_->open_file(path, with_os_default) : no("open file"); }

protected:
    ICommandTarget* inner_;
};

}  // namespace kt::commands
