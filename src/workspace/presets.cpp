#include "workspace/presets.h"

#include <algorithm>
#include <utility>

#include "workspace/workspace.h"

namespace kt::ws {

namespace {

using json::Value;

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxPanesPerTab = 64;

Value str(std::string_view s) { return Value::string(std::string(s)); }

Value shell_to_json(const ShellSpec& sh) {
    Value v = Value::object();
    v.set("program", str(sh.program));
    Value a = Value::array();
    for (const auto& x : sh.args) a.push(str(x));
    v.set("args", std::move(a));
    return v;
}

Value agent_to_json(const agents::AgentResume& r) {
    Value v = Value::object();
    v.set("agent", str(r.agent));
    v.set("session_id", str(r.session_id));
    v.set("directory", str(r.directory));
    Value a = Value::array();
    for (const auto& x : r.arguments) a.push(str(x));
    v.set("arguments", std::move(a));
    return v;
}

Value pane_to_json(const PresetPane& p) {
    Value v = Value::object();
    if (!p.cwd.empty()) v.set("cwd", str(p.cwd));
    if (p.shell) v.set("shell", shell_to_json(*p.shell));
    if (p.run) v.set("run", str(*p.run));
    if (p.agent) v.set("agent", agent_to_json(*p.agent));
    return v;
}

Value node_to_json(const PresetNode& n) {
    Value out = Value::object();
    if (n.pane) {
        out.set("pane", pane_to_json(*n.pane));
        return out;
    }
    Value s = Value::object();
    s.set("axis", str(n.axis == Axis::Horizontal ? "horizontal" : "vertical"));
    s.set("ratio", Value::number(n.ratio));
    s.set("first", n.first ? node_to_json(*n.first) : Value());
    s.set("second", n.second ? node_to_json(*n.second) : Value());
    out.set("split", std::move(s));
    return out;
}

WsError bad(std::string what) { return WsError{WsErrc::InvalidArgument, "not a workspace preset: " + std::move(what), {}}; }

const std::string* get_str(const Value& o, std::string_view k) {
    const Value* v = o.get(k);
    return v ? v->as_string() : nullptr;
}

bool strings(const Value* v, std::vector<std::string>& out, bool required) {
    if (!v || v->is_null()) return !required;
    const auto* a = v->as_array();
    if (!a) return false;
    for (const auto& x : *a) {
        const std::string* s = x.as_string();
        if (!s) return false;
        out.push_back(*s);
    }
    return true;
}

Expected<std::optional<ShellSpec>, WsError> shell_from(const Value* v) {
    if (!v || v->is_null()) return std::optional<ShellSpec>{};
    if (!v->is_object()) return Unexpected<WsError>(bad("\"shell\" is not an object"));
    const std::string* prog = get_str(*v, "program");
    if (!prog || prog->empty()) return Unexpected<WsError>(bad("\"shell\" needs a \"program\""));
    ShellSpec sh;
    sh.program = *prog;
    if (!strings(v->get("args"), sh.args, false)) return Unexpected<WsError>(bad("\"shell.args\" is not a list of strings"));
    return std::optional<ShellSpec>(std::move(sh));
}

// An invalid agent record drops the record, not the preset (like the state file).
std::optional<agents::AgentResume> agent_from(const Value* v) {
    if (!v || !v->is_object()) return std::nullopt;
    const std::string *a = get_str(*v, "agent"), *id = get_str(*v, "session_id"), *dir = get_str(*v, "directory");
    if (!a || !id || !dir || a->empty() || id->empty() || dir->empty()) return std::nullopt;
    agents::AgentResume r{*a, *id, *dir, {}};
    if (!strings(v->get("arguments"), r.arguments, false)) return std::nullopt;
    return r;
}

Expected<PresetPane, WsError> pane_from(const Value& v) {
    if (!v.is_object()) return Unexpected<WsError>(bad("a pane is not an object"));
    PresetPane p;
    if (const std::string* c = get_str(v, "cwd")) p.cwd = *c;
    auto sh = shell_from(v.get("shell"));
    if (!sh) return Unexpected<WsError>(sh.error());
    p.shell = std::move(*sh);
    if (const std::string* r = get_str(v, "run"); r && !r->empty()) p.run = *r;
    p.agent = agent_from(v.get("agent"));
    return p;
}

Expected<PresetNode, WsError> node_from(const Value& v, int depth) {
    if (depth > kMaxDepth) return Unexpected<WsError>(bad("the layout is nested too deep"));
    if (!v.is_object()) return Unexpected<WsError>(bad("a layout node is not an object"));
    PresetNode n;
    if (const Value* p = v.get("pane")) {
        auto pane = pane_from(*p);
        if (!pane) return Unexpected<WsError>(pane.error());
        n.pane = std::move(*pane);
        return n;
    }
    const Value* s = v.get("split");
    if (!s || !s->is_object()) return Unexpected<WsError>(bad("a layout node needs \"pane\" or \"split\""));
    const std::string* axis = get_str(*s, "axis");
    if (!axis || (*axis != "horizontal" && *axis != "vertical"))
        return Unexpected<WsError>(bad("\"axis\" is \"horizontal\" or \"vertical\""));
    n.axis = *axis == "horizontal" ? Axis::Horizontal : Axis::Vertical;
    if (const Value* r = s->get("ratio")) {
        const auto d = r->as_double();
        if (!d || !(*d > 0 && *d < 1)) return Unexpected<WsError>(bad("a split \"ratio\" is in (0, 1)"));
        n.ratio = *d;
    }
    const Value *f = s->get("first"), *sc = s->get("second");
    if (!f || !sc) return Unexpected<WsError>(bad("a split needs \"first\" and \"second\""));
    auto a = node_from(*f, depth + 1);
    if (!a) return Unexpected<WsError>(a.error());
    auto b = node_from(*sc, depth + 1);
    if (!b) return Unexpected<WsError>(b.error());
    n.first = std::make_unique<PresetNode>(std::move(*a));
    n.second = std::make_unique<PresetNode>(std::move(*b));
    return n;
}

std::size_t index_from(const Value* v, std::size_t count) {
    if (!v) return 0;
    const auto i = v->as_int();
    if (!i || *i < 0 || static_cast<std::size_t>(*i) >= count) return 0;
    return static_cast<std::size_t>(*i);
}

// ------------------------------------------------------------------------------ capture

using LiveCwd = std::function<std::string(PaneId)>;

PresetPane pane_of(const Pane& p, bool include_agents, const LiveCwd& live) {
    PresetPane out;
    out.cwd = p.last_cwd;
    if (live) {
        std::string c = live(p.id);
        if (!c.empty()) out.cwd = std::move(c);
    }
    out.shell = p.shell;
    if (include_agents) out.agent = p.agent_resume;
    return out;
}

PresetTab tab_of(const Workspace& ws, const Tab& t, bool include_agents, const LiveCwd& live) {
    PresetTab out;
    out.title = t.title_override;
    // The tree through its JSON form (the split tree has no public node walk).
    const Value tree = t.tree.to_json([&](PaneId id) {
        const Pane* p = ws.pane(id);
        return pane_to_json(p ? pane_of(*p, include_agents, live) : PresetPane{});
    });
    if (auto n = node_from(tree, 0)) out.layout = std::move(*n);
    else out.layout.pane = PresetPane{};
    const auto order = t.tree.panes();
    const auto it = std::find(order.begin(), order.end(), t.focused);
    out.focused = it == order.end() ? 0 : static_cast<std::size_t>(it - order.begin());
    return out;
}

PresetProject project_of(const Workspace& ws, const ProjectWorkspace& w, const Project* p, bool include_agents, const LiveCwd& live) {
    PresetProject out;
    if (p) {
        out.name = p->name;
        out.path = p->path;
        out.color = p->color;
        out.shell = p->shell;
    } else {
        out.scratch = true;
        out.name = ws.display_name(kScratchProject);
    }
    for (std::size_t i = 0; i < w.tabs.size(); ++i) {
        out.tabs.push_back(tab_of(ws, w.tabs[i], include_agents, live));
        if (w.active_tab == w.tabs[i].id) out.active = i;
    }
    return out;
}

// ------------------------------------------------------------------------------ apply

struct Builder {
    Workspace& ws;
    const PresetLoad& how;
    PresetLoadResult& res;

    NewPaneSpec spec_of(const PresetPane& p) const {
        NewPaneSpec s;
        if (!p.cwd.empty()) s.cwd = p.cwd;
        s.shell = p.shell;
        s.run = p.run;
        if (how.include_agents && p.agent && !s.run && how.resume_line) s.run = how.resume_line(*p.agent, p.shell);
        return s;
    }

    static const PresetPane& leftmost(const PresetNode& n) {
        const PresetNode* x = &n;
        while (!x->pane && x->first) x = x->first.get();
        static const PresetPane empty;
        return x->pane ? *x->pane : empty;
    }

    void finish_pane(PaneId id, const PresetPane& p) {
        ++res.panes;
        if (how.include_agents && p.agent) {
            ws.set_agent_resume(id, p.agent);
            ++res.agents;
        }
    }

    // `at` holds the region of `n`; its spec was the leftmost leaf of `n`.
    WsResult<void> build(const PresetNode& n, PaneId at, std::vector<PaneId>& order, int depth) {
        if (depth > kMaxDepth) return ws_error(WsErrc::InvalidArgument, "the layout is nested too deep");
        if (n.pane) {
            finish_pane(at, *n.pane);
            order.push_back(at);
            return {};
        }
        if (!n.first || !n.second) return ws_error(WsErrc::InvalidArgument, "a split needs two sides");
        const Direction dir = n.axis == Axis::Horizontal ? Direction::Right : Direction::Down;
        auto second = ws.split_pane(at, dir, spec_of(leftmost(*n.second)), false);
        if (!second) return Unexpected<WsError>(second.error());
        (void)ws.set_split_ratio(at, n.axis, n.ratio);
        if (auto r = build(*n.first, at, order, depth + 1); !r) return r;
        return build(*n.second, *second, order, depth + 1);
    }

    WsResult<TabId> tab(ProjectId project, const PresetTab& t) {
        if (t.layout.pane_count() > kMaxPanesPerTab) return ws_error(WsErrc::InvalidArgument, "a tab has too many panes");
        NewTabOptions o;
        o.focus = false;
        // At the end: "after the active tab" would reverse the preset's order.
        if (const ProjectWorkspace* w = ws.workspace_of(project)) o.at = w->tabs.size();
        auto id = ws.new_tab(project, spec_of(leftmost(t.layout)), o);
        if (!id) return id;
        const Tab* tb = ws.tab(*id);
        if (!tb) return ws_error(WsErrc::NotFound, "the new tab is gone");
        std::vector<PaneId> order;
        if (auto r = build(t.layout, tb->focused, order, 0); !r) return Unexpected<WsError>(r.error());
        if (t.title && !t.title->empty()) (void)ws.rename_tab(*id, t.title);
        if (t.focused < order.size()) (void)ws.focus_pane(order[t.focused]);
        ++res.tabs;
        return id;
    }

    // The tabs of `p` into `project`; the tabs that were there before go away (a new project's
    // default tab, or the old tabs of a layout target) when `replace_existing`.
    // `count`: the old panes go into res.detached / res.closed (not for a new project's default tab).
    WsResult<void> fill(ProjectId project, const PresetProject& p, bool replace_existing, bool keep_sessions,
                        bool count = true) {
        std::vector<TabId> old;
        if (const ProjectWorkspace* w = ws.workspace_of(project))
            for (const auto& t : w->tabs) old.push_back(t.id);
        std::vector<TabId> made;
        for (const PresetTab& t : p.tabs) {
            auto id = tab(project, t);
            if (!id) return Unexpected<WsError>(id.error());
            made.push_back(*id);
        }
        if (replace_existing && !made.empty()) {
            for (TabId t : old) {
                const Tab* tb = ws.tab(t);
                const std::size_t n = tb ? tb->tree.panes().size() : 0;
                if (keep_sessions) {
                    if (ws.detach_tab(t) && count) res.detached += n;
                } else if (ws.close_tab(t, true) && count) {
                    res.closed += n;
                }
            }
        }
        if (!made.empty()) {
            const TabId active = made[std::min(p.active, made.size() - 1)];
            (void)ws.set_active_tab(active);
            for (const auto& win : ws.windows()) {
                if (win.shown == project) (void)ws.select_tab(win.id, active);
            }
        }
        return {};
    }
};

// A project name not used yet: "api", "api 2", "api 3" ...
std::string free_name(const Workspace& ws, const std::string& name) {
    auto used = [&](const std::string& n) {
        for (const auto& p : ws.projects())
            if (p.name == n) return true;
        return false;
    };
    if (!used(name)) return name;
    for (int i = 2; i < 1000; ++i) {
        std::string n = name + " " + std::to_string(i);
        if (n.size() <= 128 && !used(n)) return n;
    }
    return name;
}

}  // namespace

// ------------------------------------------------------------------------------ PresetPane / Node

bool PresetPane::operator==(const PresetPane& o) const {
    return cwd == o.cwd && shell == o.shell && run == o.run && agent == o.agent;
}

PresetNode::PresetNode(const PresetNode& o) : pane(o.pane), axis(o.axis), ratio(o.ratio) {
    if (o.first) first = std::make_unique<PresetNode>(*o.first);
    if (o.second) second = std::make_unique<PresetNode>(*o.second);
}

PresetNode& PresetNode::operator=(const PresetNode& o) {
    if (this != &o) {
        PresetNode copy(o);
        *this = std::move(copy);
    }
    return *this;
}

bool PresetNode::operator==(const PresetNode& o) const {
    if (pane || o.pane) return pane == o.pane;
    auto same = [](const std::unique_ptr<PresetNode>& a, const std::unique_ptr<PresetNode>& b) {
        return (!a && !b) || (a && b && *a == *b);
    };
    // Ratios round-trip through the JSON text: compare to a millionth.
    const double d = ratio - o.ratio;
    return axis == o.axis && d < 1e-6 && d > -1e-6 && same(first, o.first) && same(second, o.second);
}

std::size_t PresetNode::pane_count() const {
    if (pane) return 1;
    return (first ? first->pane_count() : 0) + (second ? second->pane_count() : 0);
}

void PresetNode::leaves(std::vector<const PresetPane*>& out) const {
    if (pane) {
        out.push_back(&*pane);
        return;
    }
    if (first) first->leaves(out);
    if (second) second->leaves(out);
}

std::size_t Preset::pane_count() const {
    std::size_t n = 0;
    for (const auto& p : projects)
        for (const auto& t : p.tabs) n += t.layout.pane_count();
    return n;
}

bool Preset::has_agents() const {
    for (const auto& p : projects)
        for (const auto& t : p.tabs) {
            std::vector<const PresetPane*> l;
            t.layout.leaves(l);
            for (const auto* x : l)
                if (x->agent) return true;
        }
    return false;
}

// ------------------------------------------------------------------------------ capture

Preset capture_preset(const Workspace& ws, std::string name, CaptureOptions opts) {
    Preset out;
    out.name = std::move(name);
    for (const Project& p : ws.projects()) {
        if (const ProjectWorkspace* w = ws.workspace_of(p.id)) out.projects.push_back(project_of(ws, *w, &p, opts.include_agents, opts.live_cwd));
    }
    if (opts.include_scratch) {
        if (const ProjectWorkspace* w = ws.workspace_of(kScratchProject); w && !w->tabs.empty())
            out.projects.push_back(project_of(ws, *w, nullptr, opts.include_agents, opts.live_cwd));
    }
    return out;
}

std::optional<PresetProject> capture_project(const Workspace& ws, ProjectId project, bool include_agents, const LiveCwd& live_cwd) {
    const ProjectWorkspace* w = ws.workspace_of(project);
    if (!w || project == kQuickProject) return std::nullopt;
    return project_of(ws, *w, ws.project(project), include_agents, live_cwd);
}

// ------------------------------------------------------------------------------ JSON

json::Value project_to_json(const PresetProject& p) {
    Value v = Value::object();
    v.set("name", str(p.name));
    if (p.scratch) v.set("scratch", Value::boolean(true));
    else v.set("path", str(p.path));
    if (p.color != ColorTag::None) v.set("color", str(to_string(p.color)));
    if (p.shell) v.set("shell", shell_to_json(*p.shell));
    v.set("active", Value::integer(static_cast<std::int64_t>(p.active)));
    Value tabs = Value::array();
    for (const PresetTab& t : p.tabs) {
        Value tv = Value::object();
        if (t.title) tv.set("title", str(*t.title));
        tv.set("focused", Value::integer(static_cast<std::int64_t>(t.focused)));
        tv.set("layout", node_to_json(t.layout));
        tabs.push(std::move(tv));
    }
    v.set("tabs", std::move(tabs));
    return v;
}

WsResult<PresetProject> project_from_json(const Value& v) {
    if (!v.is_object()) return Unexpected<WsError>(bad("a project is not an object"));
    PresetProject p;
    if (const Value* s = v.get("scratch")) p.scratch = s->as_bool().value_or(false);
    if (const std::string* n = get_str(v, "name")) p.name = *n;
    if (const std::string* path = get_str(v, "path")) p.path = *path;
    if (!p.scratch && p.path.empty()) return Unexpected<WsError>(bad("a project needs a \"path\""));
    if (!p.scratch && p.name.empty()) {
        // The folder name, like "Open folder".
        const auto cut = p.path.find_last_of("/\\", p.path.size() > 1 ? p.path.size() - 2 : std::string::npos);
        p.name = cut == std::string::npos ? p.path : p.path.substr(cut + 1);
        while (!p.name.empty() && (p.name.back() == '/' || p.name.back() == '\\')) p.name.pop_back();
        if (p.name.empty()) p.name = p.path;
    }
    if (const std::string* c = get_str(v, "color")) p.color = color_from_string(*c).value_or(ColorTag::None);
    auto sh = shell_from(v.get("shell"));
    if (!sh) return Unexpected<WsError>(sh.error());
    p.shell = std::move(*sh);
    const Value* tabs = v.get("tabs");
    if (tabs && !tabs->is_null()) {
        const auto* a = tabs->as_array();
        if (!a) return Unexpected<WsError>(bad("\"tabs\" is not a list"));
        for (const Value& tv : *a) {
            if (!tv.is_object()) return Unexpected<WsError>(bad("a tab is not an object"));
            PresetTab t;
            if (const std::string* title = get_str(tv, "title"); title && !title->empty()) t.title = *title;
            const Value* layout = tv.get("layout");
            if (layout) {
                auto n = node_from(*layout, 0);
                if (!n) return Unexpected<WsError>(n.error());
                t.layout = std::move(*n);
            } else {
                t.layout.pane = PresetPane{};   // a tab without a layout: one shell in the project folder
            }
            if (t.layout.pane_count() > kMaxPanesPerTab) return Unexpected<WsError>(bad("a tab has too many panes"));
            t.focused = index_from(tv.get("focused"), t.layout.pane_count());
            p.tabs.push_back(std::move(t));
        }
    }
    p.active = index_from(v.get("active"), p.tabs.size());
    return p;
}

json::Value preset_to_json(const Preset& p) {
    Value v = Value::object();
    v.set("format", str(kPresetFormat));
    v.set("version", Value::integer(kPresetVersion));
    v.set("name", str(p.name));
    Value projects = Value::array();
    for (const auto& pr : p.projects) projects.push(project_to_json(pr));
    v.set("projects", std::move(projects));
    return v;
}

std::string preset_to_text(const Preset& p) { return preset_to_json(p).dump(true) + "\n"; }

WsResult<Preset> preset_from_json(const Value& v) {
    if (!v.is_object()) return Unexpected<WsError>(bad("the file is not a JSON object"));
    if (const std::string* f = get_str(v, "format"); f && *f != kPresetFormat) return Unexpected<WsError>(bad("format is \"" + *f + "\""));
    if (const Value* ver = v.get("version")) {
        const auto n = ver->as_int();
        if (!n || *n < 1) return Unexpected<WsError>(bad("bad \"version\""));
        if (*n > kPresetVersion) return Unexpected<WsError>(WsError{WsErrc::InvalidArgument, "the preset was written by a newer keepterm", {}});
    }
    Preset p;
    if (const std::string* n = get_str(v, "name")) p.name = *n;
    const Value* projects = v.get("projects");
    const auto* a = projects ? projects->as_array() : nullptr;
    if (!a) return Unexpected<WsError>(bad("\"projects\" is missing or not a list"));
    for (const Value& pv : *a) {
        auto pr = project_from_json(pv);
        if (!pr) return Unexpected<WsError>(pr.error());
        p.projects.push_back(std::move(*pr));
    }
    return p;
}

WsResult<Preset> preset_from_text(std::string_view text) {
    Value v;
    json::ParseError err;
    if (!json::parse(text, v, &err))
        return Unexpected<WsError>(bad("JSON error at byte " + std::to_string(err.offset) + ": " + err.message));
    return preset_from_json(v);
}

// ------------------------------------------------------------------------------ apply

Preset preset_from_state(const StateSnapshot* state, std::string name, IRandom& rng, IClock& clock, std::string home) {
    WorkspaceOptions wo;
    wo.home_dir = std::move(home);
    Workspace tmp(rng, clock, wo);
    if (state) tmp.load(*state);
    else tmp.reset_to_default();
    return capture_preset(tmp, std::move(name));
}

WsResult<PresetLoadResult> apply_preset(Workspace& ws, const Preset& preset, const PresetLoad& how) {
    PresetLoadResult res;
    Builder b{ws, how, res};
    const bool replace = how.mode == PresetLoad::Mode::Replace;
    std::vector<ProjectId> old;
    for (const auto& p : ws.projects()) old.push_back(p.id);
    std::vector<TabId> old_scratch;
    if (const ProjectWorkspace* w = ws.workspace_of(kScratchProject))
        for (const auto& t : w->tabs) old_scratch.push_back(t.id);

    Transaction tx = ws.begin();
    bool scratch_filled = false;
    for (const PresetProject& p : preset.projects) {
        if (p.scratch) {
            // Replace: the scratch tabs of the preset take the place of the current ones.
            if (auto r = b.fill(kScratchProject, p, false, how.keep_sessions); !r) return Unexpected<WsError>(r.error());
            scratch_filled = scratch_filled || !p.tabs.empty();
            continue;
        }
        auto id = ws.add_project(replace ? p.name : free_name(ws, p.name), p.path);
        if (!id) return Unexpected<WsError>(id.error());
        res.projects.push_back(*id);
        if (p.color != ColorTag::None) (void)ws.set_project_color(*id, p.color);
        if (p.shell) (void)ws.set_project_shell(*id, p.shell);
        // add_project opened a default tab: the preset's tabs replace it. Its session never
        // started, so it is dropped without a kill.
        if (auto r = b.fill(*id, p, true, true, false); !r) return Unexpected<WsError>(r.error());
    }
    if (replace) {
        // Show the first new project where an old one was shown, then drop the old tree.
        const ProjectId first = !res.projects.empty() ? res.projects.front() : kScratchProject;
        for (const auto& win : ws.windows()) {
            if (win.shown == kQuickProject) continue;
            (void)ws.show_project(win.id, first);
            if (const ProjectWorkspace* w = ws.workspace_of(first); w && w->active_tab) (void)ws.select_tab(win.id, *w->active_tab);
        }
        auto count = [&](const ProjectWorkspace* w) {
            std::size_t n = 0;
            if (w)
                for (const auto& t : w->tabs) n += t.tree.panes().size();
            return n;
        };
        for (ProjectId id : old) {
            const std::size_t n = count(ws.workspace_of(id));
            if (how.keep_sessions) {
                if (ws.detach_project(id)) res.detached += n;
            } else if (ws.remove_project(id, true)) {
                res.closed += n;
            }
        }
        if (scratch_filled) {
            for (TabId t : old_scratch) {
                const Tab* tb = ws.tab(t);
                const std::size_t n = tb ? tb->tree.panes().size() : 0;
                if (how.keep_sessions) {
                    if (ws.detach_tab(t)) res.detached += n;
                } else if (ws.close_tab(t, true)) {
                    res.closed += n;
                }
            }
        }
    }
    return res;
}

bool valid_preset_name(std::string_view n) {
    if (n.empty() || n.size() > 64 || n == "." || n == "..") return false;
    if (n.front() == ' ' || n.back() == ' ' || n.front() == '.' || n.back() == '.') return false;
    for (const char c : n) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return false;
        if (std::string_view("/\\<>:\"|?*").find(c) != std::string_view::npos) return false;
    }
    return true;
}

}  // namespace kt::ws
