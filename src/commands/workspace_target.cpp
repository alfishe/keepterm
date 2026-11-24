#include "commands/workspace_target.h"

#include <algorithm>

namespace kt::commands {

namespace {

Status from_ws(const ws::WsError& e) {
    Errc code = Errc::invalid_argument;
    switch (e.code) {
        case ws::WsErrc::Busy: code = Errc::busy; break;
        case ws::WsErrc::NotFound: code = Errc::unavailable; break;
        case ws::WsErrc::Io:
        case ws::WsErrc::StateReadOnly: code = Errc::os_error; break;
        default: break;
    }
    return make_error(code, e.message);
}

template <class T>
Status done(const ws::WsResult<T>& r) {
    if (!r) return from_ws(r.error());
    return {};
}

Status none(std::string_view what) { return make_error(Errc::unavailable, std::string(what)); }

ws::Direction to_ws(Dir d) {
    switch (d) {
        case Dir::Left: return ws::Direction::Left;
        case Dir::Right: return ws::Direction::Right;
        case Dir::Up: return ws::Direction::Up;
        case Dir::Down: return ws::Direction::Down;
    }
    return ws::Direction::Right;
}

std::size_t index_of(const ws::ProjectWorkspace& w, ws::TabId id) {
    for (std::size_t i = 0; i < w.tabs.size(); ++i)
        if (w.tabs[i].id == id) return i;
    return w.tabs.size();
}

std::string basename(std::string_view path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.remove_suffix(1);
    const auto cut = path.find_last_of("/\\");
    return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

}  // namespace

WorkspaceTarget::WorkspaceTarget(ws::Workspace& w, ws::WindowId window, ICommandTarget* view)
    : ForwardingTarget(view), ws_(w), window_(window) {}

ws::ProjectId WorkspaceTarget::shown() const {
    const auto* w = win();
    return w ? w->shown : ws::kScratchProject;
}

TargetState WorkspaceTarget::state() const {
    TargetState s = ForwardingTarget::state();
    const ws::WindowState* w = win();
    if (!w) {
        s.has_window = false;
        return s;
    }
    s.has_window = true;
    s.has_project = !ws::is_fixed_workspace(w->shown) && ws_.project(w->shown) != nullptr;
    s.quick_terminal = w->shown == ws::kQuickProject;
    s.project_count = static_cast<std::uint32_t>(ws_.projects().size());
    s.sidebar_visible = w->sidebar_visible;
    s.fullscreen = w->fullscreen;
    const ws::ProjectWorkspace* pw = ws_.workspace_of(w->shown);
    const ws::Tab* t = tab();
    s.tab_count = pw ? static_cast<std::uint32_t>(pw->tabs.size()) : 0;
    s.tab_index = pw && t ? static_cast<std::uint32_t>(index_of(*pw, t->id) + 1) : 0;
    s.pane_count = t ? static_cast<std::uint32_t>(t->tree.size()) : 0;
    s.zoomed = t && t->zoomed.has_value();
    s.tab_pinned = t && t->pinned;
    const ws::Pane* p = t ? ws_.pane(t->focused) : nullptr;
    s.has_terminal = s.has_terminal && p != nullptr;
    s.pane_private = p && p->is_private;
    s.pane_read_only = p && p->read_only;
    s.pane_cwd_known = p && !p->last_cwd.empty();
    if (!p) s.has_session = false;
    return s;
}

// ---------------------------------------------------------------- tabs

Status WorkspaceTarget::new_tab(bool scratch) {
    ws::NewTabOptions o;
    o.window = window_;
    return done(ws_.new_tab(scratch ? ws::kScratchProject : shown(), {}, o));
}

Status WorkspaceTarget::close_tab(bool force) {
    const ws::Tab* t = tab();
    if (!t) return none("No tab");
    return done(ws_.close_tab(t->id, force));
}

Status WorkspaceTarget::select_tab_relative(int delta, bool across_projects) {
    const ws::Tab* t = tab();
    const ws::ProjectWorkspace* pw = ws_.workspace_of(shown());
    if (!t || !pw || pw->tabs.empty()) return none("No tab");
    const auto n = static_cast<std::ptrdiff_t>(pw->tabs.size());
    const auto i = static_cast<std::ptrdiff_t>(index_of(*pw, t->id));
    const std::ptrdiff_t j = i + delta;
    if (!across_projects || (j >= 0 && j < n)) {
        const std::ptrdiff_t k = ((j % n) + n) % n;
        return done(ws_.select_tab(window_, pw->tabs[static_cast<std::size_t>(k)].id));
    }
    // Across projects (sidebar order, scratch last): the next workspace with tabs.
    std::vector<ws::ProjectId> order;
    for (const auto& p : ws_.projects()) order.push_back(p.id);
    order.push_back(ws::kScratchProject);
    auto at = std::find(order.begin(), order.end(), shown());
    if (at == order.end()) return done(ws_.select_tab(window_, pw->tabs[static_cast<std::size_t>(((j % n) + n) % n)].id));
    const auto m = static_cast<std::ptrdiff_t>(order.size());
    std::ptrdiff_t pi = at - order.begin();
    for (std::ptrdiff_t step = 0; step < m; ++step) {
        pi = (((pi + (delta > 0 ? 1 : -1)) % m) + m) % m;
        const ws::ProjectWorkspace* other = ws_.workspace_of(order[static_cast<std::size_t>(pi)]);
        if (!other || other->tabs.empty()) continue;
        const ws::Tab& target = delta > 0 ? other->tabs.front() : other->tabs.back();
        auto tx = ws_.begin();
        if (auto r = ws_.show_project(window_, order[static_cast<std::size_t>(pi)]); !r) return from_ws(r.error());
        return done(ws_.select_tab(window_, target.id));
    }
    return {};
}

Status WorkspaceTarget::select_tab_index(std::uint32_t index) {
    const ws::ProjectWorkspace* pw = ws_.workspace_of(shown());
    if (!pw || index == 0 || index > pw->tabs.size()) return none("No such tab");
    return done(ws_.select_tab(window_, pw->tabs[index - 1].id));
}

Status WorkspaceTarget::move_tab(int delta) {
    const ws::Tab* t = tab();
    const ws::ProjectWorkspace* pw = ws_.workspace_of(shown());
    if (!t || !pw) return none("No tab");
    const auto i = static_cast<std::ptrdiff_t>(index_of(*pw, t->id));
    const auto to = std::clamp<std::ptrdiff_t>(i + delta, 0, static_cast<std::ptrdiff_t>(pw->tabs.size()) - 1);
    if (to == i) return {};
    return done(ws_.move_tab(t->id, static_cast<std::size_t>(to)));
}

Status WorkspaceTarget::move_tab_to_project(std::string_view project_id) {
    const ws::Tab* t = tab();
    ws::ProjectId p;
    if (!t) return none("No tab");
    if (!ws::parse_id(project_id, p)) return make_error(Errc::invalid_argument, "Not a project id");
    return done(ws_.move_tab_to_project(t->id, p));
}

Status WorkspaceTarget::separate_all_panes() {
    const ws::Tab* t = tab();
    if (!t) return none("No tab");
    return done(ws_.separate_panes(t->id));
}

Status WorkspaceTarget::set_tab_pinned(bool pinned) {
    const ws::Tab* t = tab();
    if (!t) return none("No tab");
    return done(ws_.set_tab_pinned(t->id, pinned));
}

// ---------------------------------------------------------------- panes

Status WorkspaceTarget::close_pane(bool force) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.close_pane(t->focused, force));
}

Status WorkspaceTarget::split_pane(SplitDir d) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    if (d == SplitDir::Auto) {
        if (inner_) {
            auto r = inner_->split_pane(d);
            if (r || r.error().code != Errc::not_supported) return r;
        }
        d = SplitDir::Right;
    }
    return done(ws_.split_pane(t->focused, d == SplitDir::Down ? ws::Direction::Down : ws::Direction::Right));
}

Status WorkspaceTarget::separate_pane() {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.break_pane_to_tab(t->focused));
}

Status WorkspaceTarget::toggle_zoom() {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.toggle_zoom(t->focused));
}

Status WorkspaceTarget::equalize_panes() {
    const ws::Tab* t = tab();
    if (!t) return none("No tab");
    return done(ws_.equalize(t->id));
}

Status WorkspaceTarget::focus_pane(Dir d) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.focus_direction(t->focused, to_ws(d)));
}

Status WorkspaceTarget::focus_pane_cycle(int delta) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(delta >= 0 ? ws_.focus_next(t->focused) : ws_.focus_previous(t->focused));
}

Status WorkspaceTarget::resize_pane(Dir d) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.resize_pane(t->focused, to_ws(d)));
}

Status WorkspaceTarget::move_pane(Dir d) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.move_pane(t->focused, to_ws(d)));
}

Status WorkspaceTarget::set_pane_private(bool on) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.set_pane_private(t->focused, on));
}

Status WorkspaceTarget::set_pane_read_only(bool on) {
    const ws::Tab* t = tab();
    if (!t) return none("No pane");
    return done(ws_.set_pane_read_only(t->focused, on));
}

// ---------------------------------------------------------------- projects and windows

Status WorkspaceTarget::remove_project(bool force) {
    const ws::ProjectId p = shown();
    if (ws::is_fixed_workspace(p)) return none("No project is open");
    return done(ws_.remove_project(p, force));
}

Status WorkspaceTarget::select_project_relative(int delta) {
    const auto projects = ws_.projects();
    if (projects.empty()) return none("No projects");
    const auto n = static_cast<std::ptrdiff_t>(projects.size());
    std::ptrdiff_t i = -1;
    for (std::ptrdiff_t k = 0; k < n; ++k)
        if (projects[static_cast<std::size_t>(k)].id == shown()) i = k;
    std::ptrdiff_t j = i < 0 ? (delta > 0 ? 0 : n - 1) : (((i + delta) % n) + n) % n;
    return done(ws_.show_project(window_, projects[static_cast<std::size_t>(j)].id));
}

Status WorkspaceTarget::close_window() { return done(ws_.close_window(window_)); }

// ---------------------------------------------------------------- palette rows

Status WorkspaceTarget::activate_tab(std::string_view tab_id) {
    ws::TabId id;
    if (!ws::parse_id(tab_id, id)) return make_error(Errc::invalid_argument, "Not a tab id");
    const auto loc = ws_.locate_tab(id);
    if (!loc) return none("The tab is gone");
    auto tx = ws_.begin();
    if (loc->project != shown())
        if (auto r = ws_.show_project(window_, loc->project); !r) return from_ws(r.error());
    return done(ws_.select_tab(window_, id));
}

Status WorkspaceTarget::move_tab_here(std::string_view tab_id) {
    ws::TabId id;
    if (!ws::parse_id(tab_id, id)) return make_error(Errc::invalid_argument, "Not a tab id");
    return done(ws_.move_tab_to_project(id, shown(), std::nullopt, window_));
}

Status WorkspaceTarget::open_project(std::string_view project_id, bool new_window) {
    ws::ProjectId id;
    if (!ws::parse_id(project_id, id)) return make_error(Errc::invalid_argument, "Not a project id");
    if (!ws_.project(id)) return none("The project is gone");
    if (new_window) {
        (void)ws_.open_window(id);   // the UI creates the window from the change bus
        return {};
    }
    return done(ws_.show_project(window_, id));
}

Status WorkspaceTarget::open_folder(std::string_view path, FolderAction a) {
    switch (a) {
        case FolderAction::OpenAsProject: {
            auto p = ws_.find_or_add_project(std::string(path));
            if (!p) return from_ws(p.error());
            ws_.note_folder(std::string(path));
            return done(ws_.show_project(window_, *p));
        }
        case FolderAction::AddProject: {
            auto p = ws_.add_project(basename(path), std::string(path));
            if (!p) return from_ws(p.error());
            ws_.note_folder(std::string(path));
            return done(ws_.show_project(window_, *p));
        }
        case FolderAction::OpenAsTab: {
            ws::NewPaneSpec spec;
            spec.cwd = std::string(path);
            ws::NewTabOptions o;
            o.window = window_;
            return done(ws_.new_tab(shown(), spec, o));
        }
    }
    return {};
}

// ---------------------------------------------------------------- usage

std::span<const std::int64_t> WorkspaceUsage::timestamps(std::string_view key) const {
    const ws::UsageEntry* e = s_.get(key);
    if (!e) return {};
    return e->t;
}

void WorkspaceUsage::record(std::string_view key, std::int64_t now_ms) {
    (void)now_ms;
    s_.record(key);
}

// ---------------------------------------------------------------- palette data

palette::PaletteData palette_data(const ws::Workspace& w, ws::WindowId window, const TabTitleFn& tab_title) {
    palette::PaletteData d;
    const ws::WindowState* win = w.window(window);
    const ws::Tab* current = w.shown_tab(window);
    if (win) d.current_project_id = win->shown.to_string();
    for (const auto& p : w.projects()) {
        palette::ProjectInfo info;
        info.id = p.id.to_string();
        info.name = p.name;
        info.path = p.path;
        d.projects.push_back(std::move(info));
    }
    std::vector<ws::ProjectId> order;
    for (const auto& p : w.projects()) order.push_back(p.id);
    order.push_back(ws::kScratchProject);
    for (const auto& pid : order) {
        const ws::ProjectWorkspace* pw = w.workspace_of(pid);
        if (!pw) continue;
        const std::string pname = w.display_name(pid);
        for (std::size_t i = 0; i < pw->tabs.size(); ++i) {
            const ws::Tab& t = pw->tabs[i];
            palette::TabInfo info;
            info.id = t.id.to_string();
            info.project_id = pid.to_string();
            info.project_name = pname;
            if (tab_title) info.title = tab_title(t);
            if (info.title.empty()) info.title = t.title_override ? *t.title_override : "Tab " + std::to_string(i + 1);
            info.pinned = t.pinned;
            info.scratch = pid == ws::kScratchProject;
            info.current = current && current->id == t.id;
            if (const ws::Pane* p = w.pane(t.focused)) {
                if (p->attention != ws::Attention::Idle) info.status = std::string(ws::to_string(p->attention));
            }
            d.tabs.push_back(std::move(info));
        }
    }
    for (const auto& f : w.recent_folders()) d.folders.push_back({f.path, f.used_unix_ms});
    // Project recency: the newest use of its folder.
    for (auto& p : d.projects)
        for (const auto& f : w.recent_folders())
            if (f.path == p.path) p.last_used_ms = std::max(p.last_used_ms, f.used_unix_ms);
    return d;
}

}  // namespace kt::commands
