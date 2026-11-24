#include "app/workspace_adapter.h"

#include <algorithm>

namespace kt::app {

namespace {

ui::ProjectColor to_ui(ws::ColorTag c) { return static_cast<ui::ProjectColor>(static_cast<uint8_t>(c)); }
ws::ColorTag to_ws(ui::ProjectColor c) { return static_cast<ws::ColorTag>(static_cast<uint8_t>(c)); }

ui::Attention to_ui(ws::Attention a) {
    switch (a) {
        case ws::Attention::Running: return ui::Attention::running;
        case ws::Attention::Done: return ui::Attention::done;
        case ws::Attention::Failed: return ui::Attention::failed;
        case ws::Attention::Bell: return ui::Attention::bell;
        case ws::Attention::Idle: break;
    }
    return ui::Attention::idle;
}

ws::Direction to_ws(ui::SplitDir d) {
    switch (d) {
        case ui::SplitDir::right: return ws::Direction::Right;
        case ui::SplitDir::down: return ws::Direction::Down;
        case ui::SplitDir::left: return ws::Direction::Left;
        case ui::SplitDir::up: return ws::Direction::Up;
    }
    return ws::Direction::Right;
}

std::string basename_of(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

WorkspaceAdapter::WorkspaceAdapter(ws::Workspace& w, ws::WindowId window, WorkspaceAdapterHooks hooks)
    : ws_(w), window_(window), hooks_(std::move(hooks)) {
    sub_ = ws_.subscribe([this](std::span<const ws::Change>) { dirty_ = true; });
}

WorkspaceAdapter::~WorkspaceAdapter() = default;

uint64_t WorkspaceAdapter::key_of(const ws::Id128& id) {
    uint64_t a = 0, b = 0;
    for (size_t i = 0; i < 8; ++i) {
        a = (a << 8) | id.bytes[i];
        b = (b << 8) | id.bytes[i + 8];
    }
    const uint64_t k = a ^ b;
    return k ? k : 1;
}

std::optional<ws::PaneId> WorkspaceAdapter::pane_id(ui::PaneKey k) const {
    snapshot();
    auto it = panes_.find(k);
    if (it == panes_.end()) return std::nullopt;
    return it->second;
}

std::optional<ws::TabId> WorkspaceAdapter::tab_id(ui::TabKey k) const {
    snapshot();
    auto it = tabs_.find(k);
    if (it == tabs_.end()) return std::nullopt;
    return it->second;
}

std::optional<ws::ProjectId> WorkspaceAdapter::project_id(ui::ProjectKey k) const {
    snapshot();
    auto it = projects_.find(k);
    if (it == projects_.end()) return std::nullopt;
    return it->second;
}

const ui::WorkspaceSnapshot& WorkspaceAdapter::snapshot() const {
    if (dirty_) rebuild();
    return snap_;
}

void WorkspaceAdapter::rebuild() const {
    dirty_ = false;
    const uint64_t rev = snap_.rev + 1;
    snap_ = ui::WorkspaceSnapshot{};
    snap_.rev = rev;
    projects_.clear();
    tabs_.clear();
    panes_.clear();
    const ws::WindowState* win = ws_.window(window_);
    const ws::ProjectId shown = win ? win->shown : ws::kScratchProject;
    const ws::Tab* shown_tab = ws_.shown_tab(window_);
    if (win) {
        snap_.sidebar_width = static_cast<float>(win->sidebar_width);
        snap_.sidebar_visible = win->sidebar_visible;
    }
    snap_.home = ws_.options().home_dir;
    auto add = [&](ws::ProjectId pid, const ws::Project* p) {
        const ws::ProjectWorkspace* w = ws_.workspace_of(pid);
        ui::ProjectItem item;
        item.id = key(pid);
        projects_[item.id] = pid;
        item.name = ws_.display_name(pid);
        item.scratch = ws::is_fixed_workspace(pid);
        if (p) {
            item.path = p->path;
            item.color = to_ui(p->color);
        }
        item.expanded = item.scratch || !win ||
                        std::find(win->expanded.begin(), win->expanded.end(), pid) != win->expanded.end();
        std::optional<ws::TabId> active;
        if (pid == shown && shown_tab) active = shown_tab->id;
        else if (w && w->active_tab) active = w->active_tab;
        if (w) {
            for (const ws::Tab& t : w->tabs) {
                ui::TabItem ti;
                ti.id = key(t.id);
                tabs_[ti.id] = t.id;
                ti.panes.reserve(t.tree.size());
                for (ws::PaneId pane : t.tree.panes()) {
                    const ui::PaneKey pk = key(pane);
                    ti.panes.push_back(pk);
                    panes_[pk] = pane;
                    ui::PaneItem pi;
                    pi.id = pk;
                    if (const ws::Pane* mp = ws_.pane(pane)) {
                        pi.cwd = mp->last_cwd;
                        pi.attention = to_ui(mp->attention);
                        pi.exited = mp->bind == ws::BindState::Ended;
                        if (hooks_.pane_facts) hooks_.pane_facts(*mp, pi);
                        if (mp->bind == ws::BindState::Failed && pi.title.empty()) pi.title = "Session failed";
                    }
                    snap_.panes[pk] = std::move(pi);
                }
                ti.focused = key(t.focused);
                if (t.zoomed) ti.zoomed = key(*t.zoomed);
                ti.pinned = t.pinned;
                if (t.title_override) {
                    ti.title = *t.title_override;
                } else if (const ui::PaneItem* fp = snap_.pane(ti.focused)) {
                    ti.title = !fp->title.empty() ? fp->title : !fp->cwd.empty() ? basename_of(fp->cwd) : std::string();
                }
                item.tabs.push_back(std::move(ti));
            }
            if (!active && !w->tabs.empty()) active = w->tabs.front().id;
        }
        if (active) item.active_tab = key(*active);
        snap_.projects.push_back(std::move(item));
    };
    for (const ws::Project& p : ws_.projects()) add(p.id, &p);
    // Scratch last, when it has tabs or the window shows it.
    if (const ws::ProjectWorkspace* sw = ws_.workspace_of(ws::kScratchProject); (sw && !sw->tabs.empty()) || shown == ws::kScratchProject)
        add(ws::kScratchProject, nullptr);
    // The quick terminal's workspace: listed only in its own panel, but every window mirrors
    // its panes (the owner forwards their session events, the CLI reaches them).
    if (shown == ws::kQuickProject) {
        add(ws::kQuickProject, nullptr);
    } else if (const ws::ProjectWorkspace* qw = ws_.workspace_of(ws::kQuickProject)) {
        for (const ws::Tab& t : qw->tabs) {
            tabs_[key(t.id)] = t.id;
            for (ws::PaneId pane : t.tree.panes()) panes_[key(pane)] = pane;
        }
    }
    snap_.shown = key(shown);
}

std::vector<ws::ProjectId> WorkspaceAdapter::expanded_list() const {
    const ws::WindowState* w = ws_.window(window_);
    return w ? w->expanded : std::vector<ws::ProjectId>{};
}

void WorkspaceAdapter::show_project(ui::ProjectKey k) {
    const auto id = project_id(k);
    if (!id) return;
    if (!ok(ws_.show_project(window_, *id))) return;
    // An empty project opens a tab at once (Macterm: selecting a project shows a terminal).
    if (const ws::ProjectWorkspace* w = ws_.workspace_of(*id); w && w->tabs.empty()) new_tab(k);
}

void WorkspaceAdapter::set_expanded(ui::ProjectKey k, bool on) {
    const auto id = project_id(k);
    const ws::WindowState* w = ws_.window(window_);
    if (!id || !w || ws::is_fixed_workspace(*id)) return;
    std::vector<ws::ProjectId> ex = w->expanded;
    auto it = std::find(ex.begin(), ex.end(), *id);
    if (on && it == ex.end()) ex.push_back(*id);
    if (!on && it != ex.end()) ex.erase(it);
    std::sort(ex.begin(), ex.end());
    ok(ws_.update_sidebar(window_, w->sidebar_width, w->sidebar_visible, std::move(ex)));
}

void WorkspaceAdapter::new_project() {
    if (hooks_.new_project) hooks_.new_project();
}

void WorkspaceAdapter::rename_project(ui::ProjectKey k, std::string name) {
    if (const auto id = project_id(k)) ok(ws_.rename_project(*id, std::move(name)));
}

void WorkspaceAdapter::set_project_color(ui::ProjectKey k, ui::ProjectColor c) {
    if (const auto id = project_id(k)) ok(ws_.set_project_color(*id, to_ws(c)));
}

void WorkspaceAdapter::move_project(ui::ProjectKey k, size_t to) {
    if (const auto id = project_id(k)) ok(ws_.move_project(*id, std::min(to, ws_.projects().size() - 1)));
}

void WorkspaceAdapter::remove_project(ui::ProjectKey k) {
    const auto id = project_id(k);
    if (!id) return;
    // The UI asked already ("Remove project?"); running programs end with it.
    ok(ws_.remove_project(*id, true));
}

void WorkspaceAdapter::new_tab(ui::ProjectKey k) {
    const auto id = project_id(k);
    if (!id) return;
    const ws::WindowState* w = ws_.window(window_);
    if (w && w->shown != *id && !ok(ws_.show_project(window_, *id))) return;
    ws::NewTabOptions o;
    o.window = window_;
    ok(ws_.new_tab(*id, {}, o));
}

void WorkspaceAdapter::select_tab(ui::TabKey k) {
    const auto id = tab_id(k);
    if (!id) return;
    const auto loc = ws_.locate_tab(*id);
    const ws::WindowState* w = ws_.window(window_);
    if (!loc || !w) return;
    ws::Transaction tx = ws_.begin();
    if (w->shown != loc->project && !ok(ws_.show_project(window_, loc->project))) return;
    ok(ws_.select_tab(window_, *id));
}

std::string WorkspaceAdapter::busy_question(const char* what, std::span<const ws::PaneId> panes) const {
    std::vector<std::string> names;
    if (hooks_.running) {
        for (ws::PaneId id : panes) {
            const ws::Pane* p = ws_.pane(id);
            if (!p) continue;
            std::string n = hooks_.running(*p);
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(std::move(n));
        }
    }
    std::string q = std::string("Close this ") + what + "? ";
    if (names.empty()) return q + "A program is still running in it.";
    std::string list;
    for (size_t i = 0; i < names.size() && i < 3; ++i) list += (i ? ", " : "") + names[i];
    if (names.size() > 3) list += ", ...";
    return q + list + (names.size() == 1 ? " is" : " are") + " still running in it and will be terminated.";
}

void WorkspaceAdapter::close_tab(ui::TabKey k) {
    const auto id = tab_id(k);
    if (!id) return;
    auto r = ws_.close_tab(*id, false);
    if (!r && r.error().code == ws::WsErrc::Busy && hooks_.confirm) {
        const ws::Tab* t = ws_.tab(*id);
        const auto panes = t ? t->tree.panes() : std::vector<ws::PaneId>{};
        hooks_.confirm(busy_question("tab", panes), [this, t = *id] { ok(ws_.close_tab(t, true)); });
        return;
    }
    ok(r);
}

void WorkspaceAdapter::rename_tab(ui::TabKey k, std::string title) {
    const auto id = tab_id(k);
    if (!id) return;
    ok(ws_.rename_tab(*id, title.empty() ? std::nullopt : std::optional<std::string>(std::move(title))));
}

void WorkspaceAdapter::move_tab(ui::TabKey k, ui::ProjectKey to_project, size_t to) {
    const auto id = tab_id(k);
    const auto dest = project_id(to_project);
    if (!id || !dest) return;
    const auto loc = ws_.locate_tab(*id);
    if (!loc) return;
    if (loc->project == *dest) {
        const ws::ProjectWorkspace* w = ws_.workspace_of(*dest);
        if (w && !w->tabs.empty()) ok(ws_.move_tab(*id, std::min(to, w->tabs.size() - 1)));
        return;
    }
    ok(ws_.move_tab_to_project(*id, *dest, to));
}

void WorkspaceAdapter::split(ui::PaneKey k, ui::SplitDir d) {
    if (const auto id = pane_id(k)) ok(ws_.split_pane(*id, to_ws(d)));
}

void WorkspaceAdapter::dock_pane(ui::PaneKey k, ui::PaneKey target, ui::SplitDir side) {
    const auto id = pane_id(k), to = pane_id(target);
    if (!id || !to || *id == *to) return;
    const auto loc = ws_.locate(*to);
    if (!loc) return;
    ok(ws_.move_pane_to_tab(*id, loc->tab, *to, to_ws(side)));
}

void WorkspaceAdapter::close_pane(ui::PaneKey k) {
    const auto id = pane_id(k);
    if (!id) return;
    auto r = ws_.close_pane(*id, false);
    if (!r && r.error().code == ws::WsErrc::Busy && hooks_.confirm) {
        hooks_.confirm(busy_question("pane", std::span<const ws::PaneId>(&*id, 1)), [this, p = *id] { ok(ws_.close_pane(p, true)); });
        return;
    }
    ok(r);
}

void WorkspaceAdapter::focus_pane(ui::PaneKey k) {
    if (const auto id = pane_id(k)) {
        const ws::Tab* t = nullptr;
        if (const auto loc = ws_.locate(*id)) t = ws_.tab(loc->tab);
        if (t && t->focused == *id) return;
        ok(ws_.focus_pane(*id));
    }
}

void WorkspaceAdapter::toggle_zoom(ui::PaneKey k) {
    if (const auto id = pane_id(k)) ok(ws_.toggle_zoom(*id));
}

void WorkspaceAdapter::layout(ui::TabKey k, ui::Rect area, float divider, std::vector<ui::PaneRect>& panes,
                              std::vector<ui::DividerRect>& dividers) const {
    const auto id = tab_id(k);
    const ws::Tab* t = id ? ws_.tab(*id) : nullptr;
    if (!t) return;
    const ws::RectDip a{area.x, area.y, area.w, area.h};
    auto rect = [](const ws::RectDip& r) {
        return ui::Rect{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
    };
    for (const ws::PaneFrame& f : t->tree.layout(a, divider)) panes.push_back({key(f.pane), rect(f.rect)});
    for (const ws::DividerFrame& d : t->tree.dividers(a, divider)) {
        ui::DividerRect r;
        r.ref = d.branch.index;
        r.vertical = d.axis == ws::Axis::Horizontal;
        r.r = rect(d.rect);
        r.parent = rect(d.area);
        dividers.push_back(r);
    }
}

void WorkspaceAdapter::set_divider_ratio(ui::TabKey k, uint64_t ref, float ratio) {
    const auto id = tab_id(k);
    if (!id) return;
    (void)ws_.set_branch_ratio(*id, ws::NodeRef{static_cast<uint32_t>(ref)}, ratio, ws::kMinRatio, ws::kMaxRatio);
}

void WorkspaceAdapter::set_sidebar(float width, bool visible) {
    ok(ws_.update_sidebar(window_, width, visible, expanded_list()));
}

}  // namespace kt::app
