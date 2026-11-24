#include "workspace/workspace.h"

#include <algorithm>
#include <cassert>
#include <set>

#include "workspace/paths.h"
#include "workspace/state.h"

namespace kt::ws {

namespace {
constexpr std::size_t kMaxProjectName = 128;
constexpr std::size_t kMaxTabTitle = 256;

bool persisted_attention(Attention a) { return a == Attention::Done || a == Attention::Failed; }

// Change kinds that collapse to one record per entity at commit.
bool mergeable(ChangeKind k) {
    switch (k) {
        case ChangeKind::ProjectChanged:
        case ChangeKind::TabChanged:
        case ChangeKind::TreeChanged:
        case ChangeKind::FocusChanged:
        case ChangeKind::ZoomChanged:
        case ChangeKind::PaneChanged:
        case ChangeKind::WindowChanged:
        case ChangeKind::AttentionCountChanged:
        case ChangeKind::RecentFoldersChanged:
        case ChangeKind::QuickFrameChanged:
        case ChangeKind::ProjectsReordered:
        case ChangeKind::TabsReordered: return true;
        default: return false;
    }
}

bool same_entity(const Change& a, const Change& b) {
    return a.kind == b.kind && a.project == b.project && a.tab == b.tab && a.pane == b.pane && a.window == b.window;
}

std::vector<Change> dedupe(std::vector<Change> in) {
    std::vector<Change> out;
    out.reserve(in.size());
    for (Change& c : in) {
        if (mergeable(c.kind)) {
            auto it = std::find_if(out.begin(), out.end(), [&](const Change& o) { return same_entity(o, c); });
            if (it != out.end()) {
                it->fields |= c.fields;
                it->persistent = it->persistent || c.persistent;
                continue;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

template <class V, class Id>
std::size_t index_of(const V& v, Id id) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].id == id) return i;
    return v.size();
}
}  // namespace

// ------------------------------------------------------------------------------ Transaction

Transaction::Transaction(Workspace* ws) : ws_(ws) {
    if (ws_) ws_->tx_open();
}
Transaction::Transaction(Transaction&& o) noexcept : ws_(o.ws_) { o.ws_ = nullptr; }
Transaction& Transaction::operator=(Transaction&& o) noexcept {
    if (this != &o) {
        commit();
        ws_ = o.ws_;
        o.ws_ = nullptr;
    }
    return *this;
}
Transaction::~Transaction() { commit(); }
void Transaction::commit() {
    if (!ws_) return;
    Workspace* w = ws_;
    ws_ = nullptr;
    w->tx_close(true);
}
void Transaction::abort() {
    if (!ws_) return;
    Workspace* w = ws_;
    ws_ = nullptr;
    w->tx_close(false);
}

Subscription::Subscription(Subscription&& o) noexcept : ws_(o.ws_), id_(o.id_) { o.ws_ = nullptr; }
Subscription& Subscription::operator=(Subscription&& o) noexcept {
    if (this != &o) {
        reset();
        ws_ = o.ws_;
        id_ = o.id_;
        o.ws_ = nullptr;
    }
    return *this;
}
Subscription::~Subscription() { reset(); }
void Subscription::reset() {
    if (ws_) ws_->unsubscribe(id_);
    ws_ = nullptr;
}

// ------------------------------------------------------------------------------ core

Workspace::Workspace(IRandom& rng, IClock& clock, WorkspaceOptions opts)
    : rng_(rng), clock_(clock), opts_(std::move(opts)), owner_(std::this_thread::get_id()) {
    s_.workspaces.push_back(ProjectWorkspace{kScratchProject, {}, {}, {}});
    s_.quick.workspace.project = kQuickProject;
}

Workspace::~Workspace() = default;

void Workspace::check_thread() const {
    assert(std::this_thread::get_id() == owner_ && "the workspace model is owned by the UI thread");
}

void Workspace::tx_open() {
    check_thread();
    if (tx_depth_++ == 0) {
        batch_.clear();
        batch_aborted_ = false;
        tx_swapped_ = false;
    }
}

void Workspace::tx_close(bool deliver) {
    if (!deliver) batch_aborted_ = true;
    if (--tx_depth_ > 0) return;
    if (batch_aborted_) {
        batch_.clear();
        return;
    }
    update_attention_count();
    std::vector<Change> batch = dedupe(std::move(batch_));
    batch_.clear();
    last_commit_swap_ = tx_swapped_;
    if (batch.empty()) return;
    // A subscriber may call an operation (the binder answering synchronously); that batch is
    // delivered after the current one, in order.
    queued_batches_.push_back(std::move(batch));
    if (delivering_) return;
    delivering_ = true;
    while (!queued_batches_.empty()) {
        std::vector<Change> b = std::move(queued_batches_.front());
        queued_batches_.pop_front();
        last_commit_swap_ = std::any_of(b.begin(), b.end(), [](const Change& c) {
            return c.kind == ChangeKind::PaneChanged && (c.fields & field::kSession);
        });
        // Copy the handlers: a callback may unsubscribe.
        std::vector<std::pair<std::uint64_t, ChangeHandler>> subs;
        for (const Sub& sub : subs_) subs.emplace_back(sub.id, sub.fn);
        for (auto& [id, fn] : subs) {
            const bool still = std::any_of(subs_.begin(), subs_.end(), [&](const Sub& sub) { return sub.id == id; });
            if (still && fn) fn(std::span<const Change>(b));
        }
    }
    delivering_ = false;
}

void Workspace::emit(Change c) {
    if (c.kind == ChangeKind::PaneChanged && (c.fields & field::kSession)) tx_swapped_ = true;
    batch_.push_back(std::move(c));
}

Subscription Workspace::subscribe(ChangeHandler handler) {
    check_thread();
    const std::uint64_t id = next_sub_++;
    subs_.push_back({id, std::move(handler)});
    return Subscription(this, id);
}

void Workspace::unsubscribe(std::uint64_t id) {
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [&](const Sub& s) { return s.id == id; }), subs_.end());
}

// ------------------------------------------------------------------------------ lookups

ProjectWorkspace* Workspace::ws_mut(ProjectId p) {
    if (p == kQuickProject) return &s_.quick.workspace;
    for (auto& w : s_.workspaces)
        if (w.project == p) return &w;
    return nullptr;
}

const ProjectWorkspace* Workspace::workspace_of(ProjectId p) const {
    return const_cast<Workspace*>(this)->ws_mut(p);
}

Project* Workspace::project_mut(ProjectId p) {
    for (auto& pr : s_.projects)
        if (pr.id == p) return &pr;
    return nullptr;
}

const Project* Workspace::project(ProjectId p) const { return const_cast<Workspace*>(this)->project_mut(p); }

Tab* Workspace::tab_mut(TabId t, ProjectWorkspace** owner) {
    auto scan = [&](ProjectWorkspace& w) -> Tab* {
        for (auto& tab : w.tabs)
            if (tab.id == t) {
                if (owner) *owner = &w;
                return &tab;
            }
        return nullptr;
    };
    for (auto& w : s_.workspaces)
        if (Tab* r = scan(w)) return r;
    return scan(s_.quick.workspace);
}

const Tab* Workspace::tab(TabId t) const { return const_cast<Workspace*>(this)->tab_mut(t); }

Pane* Workspace::pane_mut(PaneId p) {
    auto it = s_.panes.find(p);
    return it == s_.panes.end() ? nullptr : &it->second;
}

const Pane* Workspace::pane(PaneId p) const { return const_cast<Workspace*>(this)->pane_mut(p); }

WindowState* Workspace::window_mut(WindowId w) {
    for (auto& win : s_.windows)
        if (win.id == w) return &win;
    return nullptr;
}

const WindowState* Workspace::window(WindowId w) const { return const_cast<Workspace*>(this)->window_mut(w); }

std::optional<WindowId> Workspace::key_window() const {
    for (const auto& w : s_.windows)
        if (w.key) return w.id;
    return std::nullopt;
}

const Tab* Workspace::shown_tab(WindowId wid) const {
    const WindowState* w = window(wid);
    if (!w) return nullptr;
    const ProjectWorkspace* pw = workspace_of(w->shown);
    if (!pw || pw->tabs.empty()) return nullptr;
    auto find = [&](std::optional<TabId> id) -> const Tab* {
        if (!id) return nullptr;
        for (const auto& t : pw->tabs)
            if (t.id == *id) return &t;
        return nullptr;
    };
    if (const Tab* t = find(w->active_tab)) return t;
    if (const Tab* t = find(pw->active_tab)) return t;
    return &pw->tabs.front();
}

std::optional<PaneLocation> Workspace::locate(PaneId p) const {
    auto scan = [&](const ProjectWorkspace& w) -> std::optional<PaneLocation> {
        for (const auto& t : w.tabs)
            if (t.tree.contains(p)) return PaneLocation{w.project, t.id};
        return std::nullopt;
    };
    for (const auto& w : s_.workspaces)
        if (auto r = scan(w)) return r;
    return scan(s_.quick.workspace);
}

std::optional<PaneLocation> Workspace::locate(const session::SessionId& sid) const {
    for (const auto& [id, p] : s_.panes)
        if (p.session == sid) return locate(id);
    return std::nullopt;
}

std::optional<PaneLocation> Workspace::locate_tab(TabId t) const {
    ProjectWorkspace* owner = nullptr;
    if (!const_cast<Workspace*>(this)->tab_mut(t, &owner)) return std::nullopt;
    return PaneLocation{owner->project, t};
}

std::string Workspace::display_name(ProjectId p) const {
    if (p == kScratchProject) return "Scratch";
    if (p == kQuickProject) return "Quick Terminal";
    const Project* pr = project(p);
    return pr ? pr->name : std::string();
}

std::vector<session::SessionId> Workspace::claimed_sessions() const {
    std::vector<session::SessionId> out;
    out.reserve(s_.panes.size());
    for (const auto& [id, p] : s_.panes) out.push_back(p.session);
    std::sort(out.begin(), out.end());
    return out;
}

bool Workspace::visible_and_focused(PaneId p) const {
    const auto key = key_window();
    if (!key) return false;
    const Tab* t = shown_tab(*key);
    return t && t->focused == p;
}

std::size_t Workspace::compute_attention() const {
    std::size_t n = 0;
    for (const auto& [id, p] : s_.panes) {
        if (p.attention != Attention::Done && p.attention != Attention::Failed && p.attention != Attention::Bell) continue;
        if (!visible_and_focused(id)) ++n;
    }
    return n;
}

std::size_t Workspace::attention_count() const { return compute_attention(); }

void Workspace::update_attention_count() {
    const std::size_t n = compute_attention();
    if (n != attention_cached_) {
        attention_cached_ = n;
        Change c;
        c.kind = ChangeKind::AttentionCountChanged;
        c.persistent = false;
        emit(c);
    }
}

std::vector<PaneId> Workspace::panes_in_attach_order() const {
    std::vector<PaneId> out;
    std::set<PaneId> seen;
    auto add_tab = [&](const Tab& t) {
        for (PaneId p : t.tree.panes())
            if (seen.insert(p).second) out.push_back(p);
    };
    std::vector<const WindowState*> wins;
    for (const auto& w : s_.windows) wins.push_back(&w);
    std::stable_sort(wins.begin(), wins.end(), [](const WindowState* a, const WindowState* b) { return a->key && !b->key; });
    for (const WindowState* w : wins)
        if (const Tab* t = shown_tab(w->id)) add_tab(*t);
    for (const auto& pr : s_.projects)
        if (const ProjectWorkspace* w = workspace_of(pr.id))
            for (const auto& t : w->tabs) add_tab(t);
    if (const ProjectWorkspace* w = workspace_of(kScratchProject))
        for (const auto& t : w->tabs) add_tab(t);
    for (const auto& t : s_.quick.workspace.tabs) add_tab(t);
    return out;
}

std::string Workspace::canonical(std::string_view path) const { return canonical_path(path); }

// ------------------------------------------------------------------------------ helpers

void Workspace::pane_changed(Pane& p, std::uint32_t fields, bool persistent) {
    ++p.rev;
    Change c;
    c.kind = ChangeKind::PaneChanged;
    c.pane = p.id;
    if (auto loc = locate(p.id)) {
        c.project = loc->project;
        c.tab = loc->tab;
    }
    c.fields = fields;
    c.persistent = persistent;
    emit(c);
}

void Workspace::window_changed(WindowState& w, std::uint32_t fields, bool persistent) {
    ++w.rev;
    Change c;
    c.kind = ChangeKind::WindowChanged;
    c.window = w.id;
    c.project = w.shown;
    c.fields = fields;
    c.persistent = persistent;
    emit(c);
}

void Workspace::touch_tab(ProjectId project, Tab& t, ChangeKind kind) {
    ++t.rev;
    Change c;
    c.kind = kind;
    c.project = project;
    c.tab = t.id;
    c.persistent = kind != ChangeKind::FocusChanged;
    emit(c);
}

void Workspace::record_focus(Tab& t, PaneId p) {
    auto& h = t.focus_history;
    h.erase(std::remove(h.begin(), h.end(), p), h.end());
    h.insert(h.begin(), p);
    if (h.size() > kFocusHistory) h.resize(kFocusHistory);
}

void Workspace::set_focus(ProjectId project, Tab& t, PaneId p, bool clear_zoom) {
    if (t.focused != p) {
        t.focused = p;
        touch_tab(project, t, ChangeKind::FocusChanged);
    }
    record_focus(t, p);
    if (clear_zoom && t.zoomed && *t.zoomed != p) {
        t.zoomed.reset();
        touch_tab(project, t, ChangeKind::ZoomChanged);
    }
    if (Pane* pp = pane_mut(p)) {
        if (pp->attention == Attention::Done || pp->attention == Attention::Failed || pp->attention == Attention::Bell) {
            const bool persistent = persisted_attention(pp->attention);
            pp->attention = Attention::Idle;
            pane_changed(*pp, field::kAttention, persistent);
        }
    }
}

bool Workspace::any_busy(std::span<const PaneId> panes) const {
    if (!busy_) return false;
    for (PaneId id : panes)
        if (const Pane* p = pane(id); p && busy_(*p)) return true;
    return false;
}

std::string Workspace::start_dir(ProjectId project, const NewPaneSpec& spec, const Pane* origin) const {
    const Project* pr = this->project(project);
    if (spec.cwd && !spec.cwd->empty()) {
        std::string c = to_slashes(*spec.cwd);
        const bool absolute = c.front() == '/' || c.front() == '~' ||
                              (c.size() >= 2 && c[1] == ':') || c.rfind("//", 0) == 0;
        if (!absolute && pr && !pr->path.empty()) {
            std::string base = pr->path;
            if (base.back() != '/') base.push_back('/');
            return base + c;
        }
        return c;
    }
    if (origin && !origin->last_cwd.empty()) return origin->last_cwd;
    if (pr && !pr->path.empty()) return pr->path;
    return opts_.home_dir;
}

Pane& Workspace::create_pane(ProjectId project, const NewPaneSpec& spec, const Pane* origin) {
    Pane p;
    p.id = new_id<PaneId>(rng_);
    while (s_.panes.count(p.id)) p.id = new_id<PaneId>(rng_);
    p.session = spec.adopt ? *spec.adopt : new_session_id(rng_);
    p.last_cwd = start_dir(project, spec, origin);
    p.last_cwd_source = CwdSource::LastKnown;
    p.shell = spec.shell;
    p.is_private = spec.is_private;
    p.pending_run = spec.run;
    p.never_started = !spec.adopt;
    auto [it, ok] = s_.panes.emplace(p.id, std::move(p));
    (void)ok;
    Change c;
    c.kind = ChangeKind::PaneAdded;
    c.pane = it->first;
    c.project = project;
    c.session = it->second.session;
    emit(c);
    return it->second;
}

void Workspace::destroy_pane(PaneId pane, ProjectId project, TabId tab, bool exited) {
    auto it = s_.panes.find(pane);
    if (it == s_.panes.end()) return;
    Change c;
    c.kind = ChangeKind::PaneRemoved;
    c.pane = pane;
    c.project = project;
    c.tab = tab;
    c.session = it->second.session;
    c.fields = exited ? field::kExited : (detaching_ ? field::kDetached : 0);
    s_.panes.erase(it);
    emit(c);
}

void Workspace::fix_active_after_removal(ProjectWorkspace& w, std::size_t removed_index, TabId removed) {
    std::optional<TabId> repl;
    if (removed_index < w.tabs.size()) repl = w.tabs[removed_index].id;
    else if (!w.tabs.empty()) repl = w.tabs.back().id;
    if (w.active_tab == removed) w.active_tab = repl;
    for (auto& win : s_.windows) {
        if (win.shown == w.project && win.active_tab == removed) {
            win.active_tab = repl;
            window_changed(win, field::kActiveTab);
        }
    }
}

void Workspace::remove_tab_at(ProjectWorkspace& w, std::size_t index, bool destroy_panes, bool exited) {
    const TabId id = w.tabs[index].id;
    const ProjectId project = w.project;
    if (destroy_panes)
        for (PaneId p : w.tabs[index].tree.panes()) destroy_pane(p, project, id, exited);
    w.tabs.erase(w.tabs.begin() + static_cast<std::ptrdiff_t>(index));
    Change c;
    c.kind = ChangeKind::TabRemoved;
    c.project = project;
    c.tab = id;
    emit(c);
    fix_active_after_removal(w, index, id);
}

void Workspace::select_in_windows(ProjectId project, TabId tab, std::optional<WindowId> only) {
    WindowState* target = nullptr;
    if (only) {
        target = window_mut(*only);
    } else if (auto key = key_window()) {
        WindowState* k = window_mut(*key);
        if (k && k->shown == project) target = k;
    }
    if (!target || target->quick != (project == kQuickProject)) return;
    std::uint32_t f = 0;
    if (target->shown != project) {
        target->shown = project;
        f |= field::kShown;
    }
    if (target->active_tab != tab) {
        target->active_tab = tab;
        f |= field::kActiveTab;
    }
    if (f) window_changed(*target, f);
}

// ------------------------------------------------------------------------------ projects

WsResult<ProjectId> Workspace::add_project(std::string name, std::string path, std::optional<std::size_t> at) {
    check_thread();
    name = trim(name);
    if (name.empty() || name.size() > kMaxProjectName) return ws_error(WsErrc::InvalidName, "a project name needs 1 to 128 bytes");
    if (trim(path).empty()) return ws_error(WsErrc::InvalidArgument, "a project needs a folder");
    Transaction tx(this);
    Project p;
    p.id = new_id<ProjectId>(rng_);
    while (project(p.id) || is_fixed_workspace(p.id)) p.id = new_id<ProjectId>(rng_);
    p.name = std::move(name);
    p.path = to_slashes(path);
    const ProjectId id = p.id;
    const std::size_t idx = at ? std::min(*at, s_.projects.size()) : s_.projects.size();
    s_.projects.insert(s_.projects.begin() + static_cast<std::ptrdiff_t>(idx), std::move(p));
    s_.workspaces.push_back(ProjectWorkspace{id, {}, {}, {}});
    Change c;
    c.kind = ChangeKind::ProjectAdded;
    c.project = id;
    emit(c);
    // A project without saved state gets one tab with one shell in its folder.
    NewTabOptions o;
    o.focus = false;
    (void)new_tab(id, NewPaneSpec{}, o);
    note_folder(this->project(id)->path);
    return id;
}

WsResult<ProjectId> Workspace::find_or_add_project(std::string path) {
    check_thread();
    const std::string canon = canonical_path(path);
    for (const auto& p : s_.projects)
        if (canonical_path(p.path) == canon) {
            Transaction tx(this);
            note_folder(p.path);
            return p.id;
        }
    return add_project(folder_name(path), path);
}

WsResult<void> Workspace::rename_project(ProjectId id, std::string name) {
    check_thread();
    if (is_fixed_workspace(id)) return ws_error(WsErrc::ScratchImmutable, "this workspace cannot be renamed");
    Project* p = project_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such project");
    name = trim(name);
    if (name.empty() || name.size() > kMaxProjectName) return ws_error(WsErrc::InvalidName, "a project name needs 1 to 128 bytes");
    if (p->name == name) return {};
    Transaction tx(this);
    p->name = std::move(name);
    ++p->rev;
    emit(Change{ChangeKind::ProjectChanged, id, {}, {}, {}, field::kName, true, {}});
    return {};
}

WsResult<void> Workspace::set_project_color(ProjectId id, ColorTag color) {
    check_thread();
    if (is_fixed_workspace(id)) return ws_error(WsErrc::ScratchImmutable, "this workspace has no color");
    Project* p = project_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such project");
    if (p->color == color) return {};
    Transaction tx(this);
    p->color = color;
    ++p->rev;
    emit(Change{ChangeKind::ProjectChanged, id, {}, {}, {}, field::kColor, true, {}});
    return {};
}

WsResult<void> Workspace::set_project_shell(ProjectId id, std::optional<ShellSpec> shell) {
    check_thread();
    if (is_fixed_workspace(id)) return ws_error(WsErrc::ScratchImmutable, "this workspace has no shell setting");
    Project* p = project_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such project");
    if (shell && trim(shell->program).empty()) shell.reset();
    if (p->shell == shell) return {};
    Transaction tx(this);
    p->shell = std::move(shell);
    ++p->rev;
    emit(Change{ChangeKind::ProjectChanged, id, {}, {}, {}, field::kShell, true, {}});
    return {};
}

WsResult<void> Workspace::set_project_path(ProjectId id, std::string path) {
    check_thread();
    if (is_fixed_workspace(id)) return ws_error(WsErrc::ScratchImmutable, "this workspace has no folder");
    Project* p = project_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such project");
    if (trim(path).empty()) return ws_error(WsErrc::InvalidArgument, "a project needs a folder");
    path = to_slashes(path);
    if (p->path == path) return {};
    Transaction tx(this);
    p->path = std::move(path);
    ++p->rev;
    emit(Change{ChangeKind::ProjectChanged, id, {}, {}, {}, field::kPath, true, {}});
    note_folder(p->path);
    return {};
}

WsResult<void> Workspace::move_project(ProjectId id, std::size_t to) {
    check_thread();
    if (is_fixed_workspace(id)) return ws_error(WsErrc::ScratchImmutable, "this workspace is not in the project list");
    const std::size_t from = index_of(s_.projects, id);
    if (from == s_.projects.size()) return ws_error(WsErrc::NotFound, "no such project");
    to = std::min(to, s_.projects.size() - 1);
    if (from == to) return {};
    Transaction tx(this);
    Project p = std::move(s_.projects[from]);
    s_.projects.erase(s_.projects.begin() + static_cast<std::ptrdiff_t>(from));
    s_.projects.insert(s_.projects.begin() + static_cast<std::ptrdiff_t>(to), std::move(p));
    emit(Change{ChangeKind::ProjectsReordered, {}, {}, {}, {}, 0, true, {}});
    return {};
}

WsResult<void> Workspace::remove_project(ProjectId id, bool force) {
    check_thread();
    if (is_fixed_workspace(id)) return ws_error(WsErrc::ScratchImmutable, "this workspace cannot be removed");
    const std::size_t idx = index_of(s_.projects, id);
    if (idx == s_.projects.size()) return ws_error(WsErrc::NotFound, "no such project");
    ProjectWorkspace* w = ws_mut(id);
    std::vector<PaneId> all;
    if (w)
        for (const auto& t : w->tabs)
            for (PaneId p : t.tree.panes()) all.push_back(p);
    if (!force && any_busy(all)) return ws_error(WsErrc::Busy, "a pane of the project runs a program");
    Transaction tx(this);
    if (w)
        while (!w->tabs.empty()) remove_tab_at(*w, w->tabs.size() - 1, true, false);
    // Windows that showed it move to the next project in sidebar order, else the previous, else scratch.
    ProjectId next = kScratchProject;
    if (idx + 1 < s_.projects.size()) next = s_.projects[idx + 1].id;
    else if (idx > 0) next = s_.projects[idx - 1].id;
    s_.projects.erase(s_.projects.begin() + static_cast<std::ptrdiff_t>(idx));
    s_.workspaces.erase(std::remove_if(s_.workspaces.begin(), s_.workspaces.end(),
                                       [&](const ProjectWorkspace& x) { return x.project == id; }),
                        s_.workspaces.end());
    emit(Change{ChangeKind::ProjectRemoved, id, {}, {}, {}, 0, true, {}});
    for (auto& win : s_.windows) {
        std::uint32_t f = 0;
        if (win.shown == id) {
            win.shown = next;
            const ProjectWorkspace* nw = workspace_of(next);
            win.active_tab = nw ? nw->active_tab : std::nullopt;
            f |= field::kShown | field::kActiveTab;
        }
        auto e = std::remove(win.expanded.begin(), win.expanded.end(), id);
        if (e != win.expanded.end()) {
            win.expanded.erase(e, win.expanded.end());
            f |= field::kSidebar;
        }
        if (f) window_changed(win, f);
    }
    return {};
}

// ------------------------------------------------------------------------------ tabs

WsResult<TabId> Workspace::new_tab(ProjectId project, NewPaneSpec spec, NewTabOptions opts) {
    check_thread();
    ProjectWorkspace* w = ws_mut(project);
    if (!w) return ws_error(WsErrc::NotFound, "no such project");
    if (opts.window && !window(*opts.window)) return ws_error(WsErrc::NotFound, "no such window");
    // The focused pane of the tab the user is looking at gives the default folder.
    const Tab* current = nullptr;
    if (opts.window) {
        const WindowState* win = window(*opts.window);
        if (win->shown == project) current = shown_tab(*opts.window);
    }
    if (!current && w->active_tab) current = tab(*w->active_tab);
    if (!current && !w->tabs.empty()) current = &w->tabs.front();
    const Pane* origin = current ? pane(current->focused) : nullptr;

    Transaction tx(this);
    Pane& p = create_pane(project, spec, origin);
    Tab t;
    t.id = new_id<TabId>(rng_);
    while (tab(t.id)) t.id = new_id<TabId>(rng_);
    t.tree = SplitTree(p.id);
    t.focused = p.id;
    t.focus_history = {p.id};
    const TabId id = t.id;
    std::size_t idx = w->tabs.size();
    if (opts.at) {
        idx = std::min(*opts.at, w->tabs.size());
    } else if (current) {
        idx = index_of(w->tabs, current->id) + 1;
        idx = std::min(idx, w->tabs.size());
    }
    const bool was_empty = w->tabs.empty();
    w->tabs.insert(w->tabs.begin() + static_cast<std::ptrdiff_t>(idx), std::move(t));
    emit(Change{ChangeKind::TabAdded, project, id, {}, {}, 0, true, {}});
    if (opts.focus) {
        w->active_tab = id;
        select_in_windows(project, id, opts.window);
    } else if (was_empty || !w->active_tab) {
        w->active_tab = id;   // adopt into an empty workspace; no window selection changes
    }
    return id;
}

WsResult<void> Workspace::close_tab(TabId id, bool force) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    const auto panes = t->tree.panes();
    if (!force && any_busy(panes)) return ws_error(WsErrc::Busy, "a pane of the tab runs a program");
    Transaction tx(this);
    remove_tab_at(*w, index_of(w->tabs, id), true, false);
    return {};
}

WsResult<void> Workspace::detach_project(ProjectId id) {
    detaching_ = true;
    auto r = remove_project(id, true);
    detaching_ = false;
    return r;
}

WsResult<void> Workspace::detach_tab(TabId id) {
    detaching_ = true;
    auto r = close_tab(id, true);
    detaching_ = false;
    return r;
}

WsResult<void> Workspace::rename_tab(TabId id, std::optional<std::string> title) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    // An empty title clears the override. (No optional::reset() here: GCC 15 -O2 reports a
    // false maybe-uninitialized on it.)
    const bool clear = !title || title->empty();
    if (!clear && title->size() > kMaxTabTitle) return ws_error(WsErrc::InvalidName, "a tab title has at most 256 bytes");
    if (clear ? !t->title_override : t->title_override == title) return {};
    Transaction tx(this);
    if (clear) t->title_override = std::nullopt;
    else t->title_override = std::move(*title);
    ++t->rev;
    emit(Change{ChangeKind::TabChanged, w->project, id, {}, {}, field::kTitle, true, {}});
    return {};
}

WsResult<void> Workspace::set_tab_pinned(TabId id, bool pinned) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    if (t->pinned == pinned) return {};
    Transaction tx(this);
    t->pinned = pinned;
    ++t->rev;
    emit(Change{ChangeKind::TabChanged, w->project, id, {}, {}, field::kPinned, true, {}});
    return {};
}

WsResult<void> Workspace::move_tab(TabId id, std::size_t to) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    if (!tab_mut(id, &w)) return ws_error(WsErrc::NotFound, "no such tab");
    const std::size_t from = index_of(w->tabs, id);
    to = std::min(to, w->tabs.size() - 1);
    if (from == to) return {};
    Transaction tx(this);
    Tab t = std::move(w->tabs[from]);
    w->tabs.erase(w->tabs.begin() + static_cast<std::ptrdiff_t>(from));
    w->tabs.insert(w->tabs.begin() + static_cast<std::ptrdiff_t>(to), std::move(t));
    emit(Change{ChangeKind::TabsReordered, w->project, {}, {}, {}, 0, true, {}});
    return {};
}

WsResult<void> Workspace::move_tab_to_project(TabId id, ProjectId to, std::optional<std::size_t> at,
                                              std::optional<WindowId> activate_in) {
    check_thread();
    ProjectWorkspace* src = nullptr;
    if (!tab_mut(id, &src)) return ws_error(WsErrc::NotFound, "no such tab");
    if (src->project == to) return ws_error(WsErrc::SameProject, "the tab is already in that project");
    if (!ws_mut(to)) return ws_error(WsErrc::NotFound, "no such project");
    Transaction tx(this);
    const ProjectId from = src->project;
    const std::size_t idx = index_of(src->tabs, id);
    Tab t = std::move(src->tabs[idx]);
    src->tabs.erase(src->tabs.begin() + static_cast<std::ptrdiff_t>(idx));
    emit(Change{ChangeKind::TabRemoved, from, id, {}, {}, 0, true, {}});
    fix_active_after_removal(*src, idx, id);
    ProjectWorkspace* dst = ws_mut(to);   // after the erase: no reallocation of workspaces happened
    const std::size_t pos = at ? std::min(*at, dst->tabs.size()) : dst->tabs.size();
    ++t.rev;
    dst->tabs.insert(dst->tabs.begin() + static_cast<std::ptrdiff_t>(pos), std::move(t));
    emit(Change{ChangeKind::TabAdded, to, id, {}, {}, 0, true, {}});
    emit(Change{ChangeKind::TabMoved, to, id, {}, {}, 0, true, {}});
    if (!dst->active_tab) dst->active_tab = id;
    if (activate_in) {
        if (WindowState* win = window_mut(*activate_in); win && win->shown == to) {
            dst->active_tab = id;
            if (win->active_tab != id) {
                win->active_tab = id;
                window_changed(*win, field::kActiveTab);
            }
        }
    }
    return {};
}

WsResult<void> Workspace::select_tab(WindowId wid, TabId id) {
    check_thread();
    WindowState* win = window_mut(wid);
    if (!win) return ws_error(WsErrc::NotFound, "no such window");
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    if ((w->project == kQuickProject) != win->quick)
        return ws_error(WsErrc::InvalidArgument, "quick terminal tabs show in the quick terminal");
    Transaction tx(this);
    w->active_tab = id;
    select_in_windows(w->project, id, wid);
    // The tab's focused pane is now in view: clear its attention.
    set_focus(w->project, *t, t->focused, false);
    return {};
}

WsResult<void> Workspace::set_active_tab(TabId id) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    if (w->active_tab == id) return {};
    Transaction tx(this);
    w->active_tab = id;
    touch_tab(w->project, *t, ChangeKind::TabChanged);
    return {};
}

WsResult<std::vector<TabId>> Workspace::separate_panes(TabId id) {
    check_thread();
    Tab* t = tab_mut(id);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    std::vector<PaneId> others;
    for (PaneId p : t->tree.panes())
        if (p != t->focused) others.push_back(p);
    Transaction tx(this);
    std::vector<TabId> out;
    for (PaneId p : others) {
        auto r = break_pane_to_tab(p);
        if (r) out.push_back(*r);
    }
    return out;
}

// ------------------------------------------------------------------------------ panes

WsResult<PaneId> Workspace::split_pane(PaneId id, Direction dir, NewPaneSpec spec, bool focus) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Tab* t = tab_mut(loc->tab);
    if (t->tree.depth() >= SplitTree::kMaxDepth) return ws_error(WsErrc::InvalidArgument, "the tab cannot be split further");
    Transaction tx(this);
    const Pane* origin = pane(id);
    Pane& np = create_pane(loc->project, spec, origin);
    const PaneId nid = np.id;
    (void)t->tree.split(id, dir, nid);
    if (opts_.auto_tile) t->tree.equalize();
    touch_tab(loc->project, *t, ChangeKind::TreeChanged);
    if (focus) set_focus(loc->project, *t, nid, true);
    return nid;
}

WsResult<void> Workspace::close_pane(PaneId id, bool force) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    if (!force && any_busy(std::span<const PaneId>(&id, 1))) return ws_error(WsErrc::Busy, "the pane runs a program");
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(loc->tab, &w);
    if (t->tree.size() == 1) {
        if (!opts_.close_last_pane_closes_tab) return ws_error(WsErrc::LastPane, "the last pane of a tab");
        Transaction tx(this);
        remove_tab_at(*w, index_of(w->tabs, loc->tab), true, false);
        return {};
    }
    Transaction tx(this);
    auto focus = t->tree.close(id, t->focus_history);
    if (!focus) return ws_error(focus.error().code, focus.error().message);
    auto& h = t->focus_history;
    h.erase(std::remove(h.begin(), h.end(), id), h.end());
    if (t->zoomed == id) {
        t->zoomed.reset();
        touch_tab(loc->project, *t, ChangeKind::ZoomChanged);
    }
    if (opts_.auto_tile) t->tree.equalize();
    touch_tab(loc->project, *t, ChangeKind::TreeChanged);
    if (t->focused == id) set_focus(loc->project, *t, *focus, false);
    destroy_pane(id, loc->project, loc->tab, false);
    return {};
}

WsResult<void> Workspace::pane_exited(PaneId id) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(loc->tab, &w);
    Transaction tx(this);
    if (t->tree.size() == 1) {
        remove_tab_at(*w, index_of(w->tabs, loc->tab), true, true);
        return {};
    }
    auto focus = t->tree.close(id, t->focus_history);
    auto& h = t->focus_history;
    h.erase(std::remove(h.begin(), h.end(), id), h.end());
    if (t->zoomed == id) {
        t->zoomed.reset();
        touch_tab(loc->project, *t, ChangeKind::ZoomChanged);
    }
    if (opts_.auto_tile) t->tree.equalize();
    touch_tab(loc->project, *t, ChangeKind::TreeChanged);
    if (t->focused == id && focus) set_focus(loc->project, *t, *focus, false);
    destroy_pane(id, loc->project, loc->tab, true);
    return {};
}

WsResult<void> Workspace::focus_pane(PaneId id) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Transaction tx(this);
    set_focus(loc->project, *tab_mut(loc->tab), id, true);
    return {};
}

WsResult<void> Workspace::focus_direction(PaneId from, Direction dir) {
    check_thread();
    auto loc = locate(from);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Tab* t = tab_mut(loc->tab);
    if (t->zoomed) return {};   // only the zoomed pane is on screen
    auto n = t->tree.neighbor(from, dir, t->focus_history);
    if (!n) return {};   // no wrap-around: focus stays
    Transaction tx(this);
    set_focus(loc->project, *t, *n, true);
    return {};
}

WsResult<void> Workspace::focus_next(PaneId from) {
    check_thread();
    auto loc = locate(from);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Tab* t = tab_mut(loc->tab);
    auto order = t->tree.panes();
    if (order.size() < 2) return {};
    const auto it = std::find(order.begin(), order.end(), from);
    const std::size_t i = static_cast<std::size_t>(it - order.begin());
    Transaction tx(this);
    set_focus(loc->project, *t, order[(i + 1) % order.size()], true);
    return {};
}

WsResult<void> Workspace::focus_previous(PaneId from) {
    check_thread();
    auto loc = locate(from);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Tab* t = tab_mut(loc->tab);
    auto order = t->tree.panes();
    if (order.size() < 2) return {};
    const auto it = std::find(order.begin(), order.end(), from);
    const std::size_t i = static_cast<std::size_t>(it - order.begin());
    Transaction tx(this);
    set_focus(loc->project, *t, order[(i + order.size() - 1) % order.size()], true);
    return {};
}

WsResult<void> Workspace::resize_pane(PaneId id, Direction dir, double delta) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    if (!(delta >= 0 && delta < 1)) return ws_error(WsErrc::InvalidRatio, "a resize step is in [0, 1)");
    Tab* t = tab_mut(loc->tab);
    SplitTree before = t->tree;
    if (!t->tree.resize(id, dir, delta) || t->tree.same_shape(before)) return {};
    Transaction tx(this);
    touch_tab(loc->project, *t, ChangeKind::TreeChanged);
    return {};
}

WsResult<void> Workspace::set_split_ratio(PaneId id, Axis axis, double ratio) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    if (!(ratio > 0 && ratio < 1)) return ws_error(WsErrc::InvalidRatio, "a split ratio is in (0, 1)");
    Tab* t = tab_mut(loc->tab);
    SplitTree before = t->tree;
    if (!t->tree.set_ratio(id, axis, ratio)) return ws_error(WsErrc::InvalidArgument, "no split on that axis holds the pane");
    if (t->tree.same_shape(before)) return {};
    Transaction tx(this);
    touch_tab(loc->project, *t, ChangeKind::TreeChanged);
    return {};
}

WsResult<void> Workspace::set_branch_ratio(TabId id, NodeRef branch, double ratio, double min_ratio, double max_ratio) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    SplitTree before = t->tree;
    if (!t->tree.set_branch_ratio(branch, ratio, min_ratio, max_ratio))
        return ws_error(WsErrc::InvalidArgument, "no such divider");
    if (t->tree.same_shape(before)) return {};
    Transaction tx(this);
    touch_tab(w->project, *t, ChangeKind::TreeChanged);
    return {};
}

WsResult<void> Workspace::equalize(TabId id) {
    check_thread();
    ProjectWorkspace* w = nullptr;
    Tab* t = tab_mut(id, &w);
    if (!t) return ws_error(WsErrc::NotFound, "no such tab");
    SplitTree before = t->tree;
    t->tree.equalize();
    if (t->tree.same_shape(before)) return {};
    Transaction tx(this);
    touch_tab(w->project, *t, ChangeKind::TreeChanged);
    return {};
}

WsResult<void> Workspace::toggle_zoom(PaneId id) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Tab* t = tab_mut(loc->tab);
    Transaction tx(this);
    if (t->zoomed == id) {
        t->zoomed.reset();
    } else {
        if (t->tree.size() < 2) return {};
        t->zoomed = id;
        set_focus(loc->project, *t, id, false);
    }
    touch_tab(loc->project, *t, ChangeKind::ZoomChanged);
    return {};
}

WsResult<void> Workspace::move_pane(PaneId id, Direction dir) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    Tab* t = tab_mut(loc->tab);
    if (!t->tree.move(id, dir, t->focus_history)) return {};
    Transaction tx(this);
    touch_tab(loc->project, *t, ChangeKind::TreeChanged);
    set_focus(loc->project, *t, id, false);
    return {};
}

WsResult<void> Workspace::move_pane_to_tab(PaneId id, TabId target, PaneId next_to, Direction side) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    ProjectWorkspace* dw = nullptr;
    Tab* dst = tab_mut(target, &dw);
    if (!dst || !dst->tree.contains(next_to)) return ws_error(WsErrc::NotFound, "no such target pane");
    if (id == next_to) return ws_error(WsErrc::InvalidArgument, "a pane cannot dock to itself");
    const ProjectId dproject = dw->project;
    {
        SplitTree trial = dst->tree;
        if (loc->tab == target) (void)trial.detach(id);
        if (!trial.insert(next_to, side, id)) return ws_error(WsErrc::InvalidArgument, "the target tab cannot be split further");
    }
    ProjectWorkspace* sw = nullptr;
    Tab* src = tab_mut(loc->tab, &sw);
    Transaction tx(this);
    if (src->tree.size() == 1) {
        // The source tab goes away with its only pane moving out; the session stays.
        remove_tab_at(*sw, index_of(sw->tabs, loc->tab), false, false);
    } else {
        auto focus = src->tree.detach(id, src->focus_history);
        auto& h = src->focus_history;
        h.erase(std::remove(h.begin(), h.end(), id), h.end());
        if (src->zoomed == id) {
            src->zoomed.reset();
            touch_tab(loc->project, *src, ChangeKind::ZoomChanged);
        }
        touch_tab(loc->project, *src, ChangeKind::TreeChanged);
        if (src->focused == id && focus) set_focus(loc->project, *src, *focus, false);
    }
    dst = tab_mut(target);   // the erase above may have moved tabs
    (void)dst->tree.insert(next_to, side, id);   // checked on a copy above
    touch_tab(dproject, *dst, ChangeKind::TreeChanged);
    set_focus(dproject, *dst, id, true);
    return {};
}

WsResult<TabId> Workspace::break_pane_to_tab(PaneId id) {
    check_thread();
    auto loc = locate(id);
    if (!loc) return ws_error(WsErrc::NotFound, "no such pane");
    ProjectWorkspace* w = nullptr;
    Tab* src = tab_mut(loc->tab, &w);
    if (src->tree.size() == 1) return ws_error(WsErrc::LastPane, "the pane is already alone in its tab");
    Transaction tx(this);
    auto focus = src->tree.detach(id, src->focus_history);
    auto& h = src->focus_history;
    h.erase(std::remove(h.begin(), h.end(), id), h.end());
    if (src->zoomed == id) {
        src->zoomed.reset();
        touch_tab(loc->project, *src, ChangeKind::ZoomChanged);
    }
    touch_tab(loc->project, *src, ChangeKind::TreeChanged);
    if (src->focused == id && focus) set_focus(loc->project, *src, *focus, false);
    Tab t;
    t.id = new_id<TabId>(rng_);
    while (tab(t.id)) t.id = new_id<TabId>(rng_);
    t.tree = SplitTree(id);
    t.focused = id;
    t.focus_history = {id};
    const TabId nid = t.id;
    const std::size_t idx = index_of(w->tabs, loc->tab) + 1;
    w->tabs.insert(w->tabs.begin() + static_cast<std::ptrdiff_t>(idx), std::move(t));
    emit(Change{ChangeKind::TabAdded, loc->project, nid, {}, {}, 0, true, {}});
    // The new tab takes the place of the source tab wherever that one was active.
    if (w->active_tab == loc->tab) w->active_tab = nid;
    for (auto& win : s_.windows)
        if (win.shown == loc->project && win.active_tab == loc->tab) {
            win.active_tab = nid;
            window_changed(win, field::kActiveTab);
        }
    return nid;
}

// ------------------------------------------------------------------------------ windows

WindowId Workspace::open_window(ProjectId shown) {
    check_thread();
    if (!workspace_of(shown) || shown == kQuickProject) shown = kScratchProject;
    Transaction tx(this);
    WindowState w;
    w.id = new_id<WindowId>(rng_);
    while (window(w.id)) w.id = new_id<WindowId>(rng_);
    w.shown = shown;
    w.active_tab = workspace_of(shown)->active_tab;
    const WindowId id = w.id;
    s_.windows.push_back(std::move(w));
    emit(Change{ChangeKind::WindowOpened, shown, {}, {}, id, 0, true, {}});
    return id;
}

WindowId Workspace::open_quick_window() {
    check_thread();
    for (const auto& w : s_.windows)
        if (w.quick) return w.id;
    Transaction tx(this);
    WindowState w;
    w.id = new_id<WindowId>(rng_);
    while (window(w.id)) w.id = new_id<WindowId>(rng_);
    w.shown = kQuickProject;
    w.quick = true;
    w.sidebar_visible = false;
    w.active_tab = s_.quick.workspace.active_tab;
    const WindowId id = w.id;
    s_.windows.push_back(std::move(w));
    emit(Change{ChangeKind::WindowOpened, kQuickProject, {}, {}, id, 0, true, {}});
    return id;
}

WsResult<void> Workspace::close_window(WindowId id) {
    check_thread();
    const std::size_t idx = index_of(s_.windows, id);
    if (idx == s_.windows.size()) return ws_error(WsErrc::NotFound, "no such window");
    Transaction tx(this);
    const ProjectId shown = s_.windows[idx].shown;
    s_.windows.erase(s_.windows.begin() + static_cast<std::ptrdiff_t>(idx));
    emit(Change{ChangeKind::WindowClosed, shown, {}, {}, id, 0, true, {}});
    return {};
}

WsResult<void> Workspace::show_project(WindowId id, ProjectId project) {
    check_thread();
    WindowState* w = window_mut(id);
    if (!w) return ws_error(WsErrc::NotFound, "no such window");
    const ProjectWorkspace* pw = workspace_of(project);
    if (!pw) return ws_error(WsErrc::NotFound, "no such project");
    if (project == kQuickProject || w->quick) return ws_error(WsErrc::InvalidArgument, "the quick terminal has its own window");
    Transaction tx(this);
    if (const Project* pr = this->project(project)) note_folder(pr->path);
    if (w->shown == project) return {};
    w->shown = project;
    w->active_tab = pw->active_tab;
    window_changed(*w, field::kShown | field::kActiveTab);
    return {};
}

WsResult<void> Workspace::update_window_frame(WindowId id, RectDip frame, std::string monitor_key, double scale,
                                              bool maximized, bool fullscreen) {
    check_thread();
    WindowState* w = window_mut(id);
    if (!w) return ws_error(WsErrc::NotFound, "no such window");
    if (!(scale > 0)) scale = 1.0;
    if (w->frame == frame && w->monitor_key == monitor_key && w->monitor_scale == scale && w->maximized == maximized &&
        w->fullscreen == fullscreen)
        return {};
    Transaction tx(this);
    w->frame = frame;
    w->monitor_key = std::move(monitor_key);
    w->monitor_scale = scale;
    w->maximized = maximized;
    w->fullscreen = fullscreen;
    window_changed(*w, field::kFrame);
    return {};
}

WsResult<void> Workspace::update_sidebar(WindowId id, double width, bool visible, std::vector<ProjectId> expanded) {
    check_thread();
    WindowState* w = window_mut(id);
    if (!w) return ws_error(WsErrc::NotFound, "no such window");
    std::sort(expanded.begin(), expanded.end());
    expanded.erase(std::unique(expanded.begin(), expanded.end()), expanded.end());
    if (w->sidebar_width == width && w->sidebar_visible == visible && w->expanded == expanded) return {};
    Transaction tx(this);
    w->sidebar_width = width;
    w->sidebar_visible = visible;
    w->expanded = std::move(expanded);
    window_changed(*w, field::kSidebar);
    return {};
}

WsResult<void> Workspace::set_key_window(WindowId id) {
    check_thread();
    if (!window(id)) return ws_error(WsErrc::NotFound, "no such window");
    Transaction tx(this);
    for (auto& w : s_.windows) {
        const bool k = w.id == id;
        if (w.key != k) {
            w.key = k;
            window_changed(w, field::kKey);
        }
    }
    if (const Tab* t = shown_tab(id)) {
        if (auto loc = locate_tab(t->id)) set_focus(loc->project, *tab_mut(t->id), t->focused, false);
    }
    return {};
}

// ------------------------------------------------------------------------------ pane facts

void Workspace::set_pane_cwd(PaneId id, std::string cwd, CwdSource source) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || cwd.empty()) return;
    cwd = to_slashes(cwd);
    // last_known only echoes the start folder: it never replaces a value.
    if (source == CwdSource::LastKnown && !p->last_cwd.empty()) return;
    if (p->last_cwd == cwd && p->last_cwd_source == source) return;
    Transaction tx(this);
    p->last_cwd = cwd;
    p->last_cwd_source = source;
    pane_changed(*p, field::kCwd, true);
    if (!p->is_private &&
        (source == CwdSource::Integration || source == CwdSource::OsQuery || source == CwdSource::ProcessPeb))
        note_folder(cwd);
}

void Workspace::set_pane_attention(PaneId id, Attention a) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || p->attention == a) return;
    Transaction tx(this);
    const bool persistent = persisted_attention(p->attention) || persisted_attention(a);
    p->attention = a;
    pane_changed(*p, field::kAttention, persistent);
}

WsResult<void> Workspace::set_pane_private(PaneId id, bool on) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such pane");
    if (p->is_private == on) return {};
    Transaction tx(this);
    p->is_private = on;
    pane_changed(*p, field::kPrivate, true);
    return {};
}

WsResult<void> Workspace::set_pane_read_only(PaneId id, bool on) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such pane");
    if (p->read_only == on) return {};
    Transaction tx(this);
    p->read_only = on;
    pane_changed(*p, field::kReadOnly, true);
    return {};
}

WsResult<void> Workspace::set_pane_shell(PaneId id, std::optional<ShellSpec> shell) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p) return ws_error(WsErrc::NotFound, "no such pane");
    if (shell && trim(shell->program).empty()) shell.reset();
    if (p->shell == shell) return {};
    Transaction tx(this);
    p->shell = std::move(shell);
    pane_changed(*p, field::kPaneShell, true);
    return {};
}

void Workspace::set_agent_resume(PaneId id, std::optional<agents::AgentResume> rec) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || p->agent_resume == rec) return;
    Transaction tx(this);
    p->agent_resume = std::move(rec);
    pane_changed(*p, field::kAgent, true);
}

void Workspace::clear_agent_resume_pending(PaneId id) { binder_set_agent_pending(id, false); }

void Workspace::clear_pending_run(PaneId id) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || !p->pending_run) return;
    Transaction tx(this);
    p->pending_run.reset();
    pane_changed(*p, field::kPendingRun, false);
}

void Workspace::binder_set_session(PaneId id, session::SessionId sid) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || p->session == sid) return;
    Transaction tx(this);
    p->session = sid;
    pane_changed(*p, field::kSession, true);
}

void Workspace::binder_set_bind(PaneId id, BindState st, std::string endpoint, std::string error) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || (p->bind == st && p->endpoint == endpoint && p->bind_error == error)) return;
    Transaction tx(this);
    p->bind = st;
    if (st == BindState::Ready) p->never_started = false;
    p->endpoint = std::move(endpoint);
    p->bind_error = std::move(error);
    pane_changed(*p, field::kBind, false);
}

void Workspace::binder_set_restore(PaneId id, RestoreStep r) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || p->restore == r) return;
    Transaction tx(this);
    p->restore = r;
    pane_changed(*p, field::kRestore, false);
}

void Workspace::binder_set_agent_pending(PaneId id, bool on) {
    check_thread();
    Pane* p = pane_mut(id);
    if (!p || p->agent_resume_pending == on) return;
    Transaction tx(this);
    p->agent_resume_pending = on;
    pane_changed(*p, field::kAgentPending, false);
}

// ------------------------------------------------------------------------------ quick terminal

void Workspace::update_quick_frame(std::string monitor_key, RectFraction f) {
    check_thread();
    auto it = s_.quick.frames.find(monitor_key);
    if (it != s_.quick.frames.end() && it->second == f) return;
    Transaction tx(this);
    s_.quick.frames[std::move(monitor_key)] = f;
    emit(Change{ChangeKind::QuickFrameChanged, kQuickProject, {}, {}, {}, 0, true, {}});
}

// ------------------------------------------------------------------------------ recent folders

void Workspace::note_folder(std::string path) {
    check_thread();
    if (trim(path).empty()) return;
    path = to_slashes(path);
    const std::string canon = canonical_path(path);
    auto& r = s_.recent_folders;
    const std::int64_t now = clock_.unix_ms();
    if (!r.empty() && canonical_path(r.front().path) == canon && r.front().path == path && r.front().used_unix_ms == now)
        return;
    Transaction tx(this);
    r.erase(std::remove_if(r.begin(), r.end(), [&](const RecentFolder& f) { return canonical_path(f.path) == canon; }),
            r.end());
    r.insert(r.begin(), RecentFolder{std::move(path), now});
    if (r.size() > kMaxRecentFolders) r.resize(kMaxRecentFolders);
    emit(Change{ChangeKind::RecentFoldersChanged, {}, {}, {}, {}, 0, true, {}});
}

void Workspace::forget_folder(std::string_view path) {
    check_thread();
    const std::string canon = canonical_path(path);
    auto& r = s_.recent_folders;
    auto e = std::remove_if(r.begin(), r.end(), [&](const RecentFolder& f) { return canonical_path(f.path) == canon; });
    if (e == r.end()) return;
    Transaction tx(this);
    r.erase(e, r.end());
    emit(Change{ChangeKind::RecentFoldersChanged, {}, {}, {}, {}, 0, true, {}});
}

// ------------------------------------------------------------------------------ load

void Workspace::load(StateSnapshot state) {
    check_thread();
    Transaction tx(this);
    s_ = std::move(state);
    if (std::none_of(s_.workspaces.begin(), s_.workspaces.end(),
                     [](const ProjectWorkspace& w) { return w.project == kScratchProject; }))
        s_.workspaces.push_back(ProjectWorkspace{kScratchProject, {}, {}, {}});
    s_.quick.workspace.project = kQuickProject;
    auto init = [](ProjectWorkspace& w) {
        for (auto& t : w.tabs)
            if (t.focus_history.empty()) t.focus_history = {t.focused};
    };
    for (auto& w : s_.workspaces) init(w);
    init(s_.quick.workspace);
    attention_cached_ = compute_attention();
    emit(Change{ChangeKind::Reset, {}, {}, {}, {}, 0, false, {}});
}

void Workspace::reset_to_default() { load(default_state(rng_, opts_.home_dir)); }

}  // namespace kt::ws
