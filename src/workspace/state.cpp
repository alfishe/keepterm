#include "workspace/state.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>

#include "workspace/paths.h"

namespace kt::ws {

namespace {

using json::Value;

// ------------------------------------------------------------------------------ time text

// Days since 1970-01-01 -> civil date (H. Hinnant's algorithm), and back.
void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Civil {
    int y;
    unsigned mo, d, h, mi, s;
};

Civil civil(std::int64_t unix_ms) {
    std::int64_t secs = unix_ms >= 0 ? unix_ms / 1000 : (unix_ms - 999) / 1000;
    std::int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    const auto sod = static_cast<unsigned>(secs - days * 86400);
    Civil c{};
    civil_from_days(days, c.y, c.mo, c.d);
    c.h = sod / 3600;
    c.mi = sod / 60 % 60;
    c.s = sod % 60;
    return c;
}

}  // namespace

std::string iso8601(std::int64_t unix_ms) {
    const Civil c = civil(unix_ms);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02u:%02u:%02uZ", c.y, c.mo, c.d, c.h, c.mi, c.s);
    return buf;
}

std::optional<std::int64_t> parse_iso8601(std::string_view s) {
    // "YYYY-MM-DDTHH:MM:SS" followed by optional fraction digits and "Z".
    if (s.size() < 20 || s.back() != 'Z') return std::nullopt;
    auto num = [&](std::size_t at, std::size_t n, unsigned& out) {
        out = 0;
        for (std::size_t k = at; k < at + n; ++k) {
            if (s[k] < '0' || s[k] > '9') return false;
            out = out * 10 + static_cast<unsigned>(s[k] - '0');
        }
        return true;
    };
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!num(0, 4, y) || s[4] != '-' || !num(5, 2, mo) || s[7] != '-' || !num(8, 2, d) || s[10] != 'T' ||
        !num(11, 2, h) || s[13] != ':' || !num(14, 2, mi) || s[16] != ':' || !num(17, 2, sec))
        return std::nullopt;
    for (std::size_t k = 19; k + 1 < s.size(); ++k)
        if (!(k == 19 ? s[k] == '.' : (s[k] >= '0' && s[k] <= '9'))) return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return std::nullopt;
    return (days_from_civil(static_cast<int>(y), mo, d) * 86400 + h * 3600 + mi * 60 + sec) * 1000;
}

std::string file_stamp(std::int64_t unix_ms) {
    const Civil c = civil(unix_ms);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d%02u%02u-%02u%02u%02u", c.y, c.mo, c.d, c.h, c.mi, c.s);
    return buf;
}

namespace {

// ------------------------------------------------------------------------------ writer

Value str(std::string_view s) { return Value::string(std::string(s)); }

Value shell_json(const ShellSpec& sh) {
    Value v = Value::object();
    v.set("program", str(sh.program));
    Value a = Value::array();
    for (const auto& x : sh.args) a.push(str(x));
    v.set("args", std::move(a));
    return v;
}

Value agent_json(const agents::AgentResume& r) {
    Value v = Value::object();
    v.set("agent", str(r.agent));
    v.set("session_id", str(r.session_id));
    v.set("directory", str(r.directory));
    Value a = Value::array();
    for (const auto& x : r.arguments) a.push(str(x));
    v.set("arguments", std::move(a));
    return v;
}

Value with_extra(const Value& extra) { return extra.is_object() ? extra : Value::object(); }

Value pane_json(const Pane& p) {
    Value v = with_extra(p.extra);
    v.set("id", str(p.id.to_string()));
    v.set("session", str(p.session.str()));
    if (!p.last_cwd.empty()) v.set("cwd", str(p.last_cwd));
    if (p.last_cwd_source != CwdSource::LastKnown) v.set("cwd_source", str(to_string(p.last_cwd_source)));
    if (p.attention == Attention::Done || p.attention == Attention::Failed) v.set("attention", str(to_string(p.attention)));
    if (p.is_private) v.set("private", Value::boolean(true));
    if (p.read_only) v.set("read_only", Value::boolean(true));
    if (p.shell) v.set("shell", shell_json(*p.shell));
    if (p.agent_resume) v.set("agent", agent_json(*p.agent_resume));
    return v;
}

Value tab_json(const Tab& t, const StateSnapshot& s) {
    Value v = with_extra(t.extra);
    v.set("id", str(t.id.to_string()));
    if (t.title_override) v.set("title", str(*t.title_override));
    v.set("focused", str(t.focused.to_string()));
    if (t.zoomed) v.set("zoomed", str(t.zoomed->to_string()));
    if (t.pinned) v.set("pinned", Value::boolean(true));
    v.set("tree", t.tree.to_json([&](PaneId id) {
        auto it = s.panes.find(id);
        if (it != s.panes.end()) return pane_json(it->second);
        Value missing = Value::object();
        missing.set("id", str(id.to_string()));
        return missing;
    }));
    return v;
}

Value workspace_json(const ProjectWorkspace& w, const StateSnapshot& s) {
    Value v = with_extra(w.extra);
    v.set("project", str(w.project.to_string()));
    if (w.active_tab) v.set("active_tab", str(w.active_tab->to_string()));
    Value tabs = Value::array();
    for (const auto& t : w.tabs) tabs.push(tab_json(t, s));
    v.set("tabs", std::move(tabs));
    return v;
}

Value rect_json(double x, double y, double w, double h) {
    Value v = Value::object();
    v.set("x", Value::number(x));
    v.set("y", Value::number(y));
    v.set("w", Value::number(w));
    v.set("h", Value::number(h));
    return v;
}

Value project_json(const Project& p) {
    Value v = with_extra(p.extra);
    v.set("id", str(p.id.to_string()));
    v.set("name", str(p.name));
    v.set("path", str(p.path));
    if (p.color != ColorTag::None) v.set("color", str(to_string(p.color)));
    if (p.shell) v.set("shell", shell_json(*p.shell));
    return v;
}

Value window_json(const WindowState& w) {
    Value v = with_extra(w.extra);
    v.set("id", str(w.id.to_string()));
    v.set("shown", str(w.shown.to_string()));
    if (w.active_tab) v.set("active_tab", str(w.active_tab->to_string()));
    v.set("frame", rect_json(w.frame.x, w.frame.y, w.frame.w, w.frame.h));
    if (!w.monitor_key.empty()) v.set("monitor", str(w.monitor_key));
    v.set("scale", Value::number(w.monitor_scale));
    v.set("maximized", Value::boolean(w.maximized));
    v.set("fullscreen", Value::boolean(w.fullscreen));
    Value sb = Value::object();
    sb.set("width", Value::number(w.sidebar_width));
    sb.set("visible", Value::boolean(w.sidebar_visible));
    Value ex = Value::array();
    std::vector<ProjectId> sorted = w.expanded;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& p : sorted) ex.push(str(p.to_string()));
    sb.set("expanded", std::move(ex));
    v.set("sidebar", std::move(sb));
    v.set("key", Value::boolean(w.key));
    return v;
}

}  // namespace

std::string serialize_state(const StateSnapshot& s, const WriterInfo& wi, std::int64_t saved_unix_ms) {
    Value doc = with_extra(s.extra);
    doc.set("schema", Value::integer(kSchemaVersion));
    Value writer = Value::object();
    writer.set("app", str(wi.app));
    if (!wi.version.empty()) writer.set("version", str(wi.version));
    if (!wi.build.empty()) writer.set("build", str(wi.build));
    doc.set("writer", std::move(writer));
    if (saved_unix_ms >= 0) doc.set("saved_at", str(iso8601(saved_unix_ms)));
    else doc.erase("saved_at");
    Value projects = Value::array();
    for (const auto& p : s.projects) projects.push(project_json(p));
    doc.set("projects", std::move(projects));
    std::vector<const ProjectWorkspace*> ws;
    for (const auto& w : s.workspaces) ws.push_back(&w);
    std::sort(ws.begin(), ws.end(), [](const auto* a, const auto* b) { return a->project < b->project; });
    Value wsj = Value::array();
    for (const auto* w : ws) wsj.push(workspace_json(*w, s));
    doc.set("workspaces", std::move(wsj));
    Value quick = with_extra(s.quick.extra);
    quick.set("workspace", workspace_json(s.quick.workspace, s));
    Value frames = Value::object();
    for (const auto& [k, f] : s.quick.frames) frames.set(k, rect_json(f.x, f.y, f.w, f.h));
    quick.set("frames", std::move(frames));
    doc.set("quick_terminal", std::move(quick));
    Value recent = Value::array();
    for (const auto& r : s.recent_folders) {
        Value e = Value::object();
        e.set("path", str(r.path));
        e.set("used_at", str(iso8601(r.used_unix_ms)));
        recent.push(std::move(e));
    }
    doc.set("recent_folders", std::move(recent));
    Value wins = Value::array();
    for (const auto& w : s.windows)
        if (!w.quick) wins.push(window_json(w));
    doc.set("windows", std::move(wins));
    return doc.dump(true);
}

// ------------------------------------------------------------------------------ migration

std::span<const Migration> migrations() {
    // Schema 1 is the first. A step here turns version N into N+1.
    static const std::vector<Migration> steps;
    return steps;
}

bool migrate(json::Value& doc, int from, int to, std::span<const Migration> steps) {
    for (int v = from; v < to; ++v) {
        auto it = std::find_if(steps.begin(), steps.end(), [&](const Migration& m) { return m.from == v; });
        if (it == steps.end() || !it->apply || !it->apply(doc)) return false;
        doc.set("schema", Value::integer(v + 1));
    }
    return true;
}

// ------------------------------------------------------------------------------ reader

namespace {

struct Reader {
    IRandom& rng;
    RepairLog& log;
    StateSnapshot& s;
    std::set<Id128> used_ids;   // every entity id seen so far (duplicates get new ids)
    bool structural = false;
    std::string why;

    void corrupt(std::string w) {
        if (!structural) why = std::move(w);
        structural = true;
    }

    template <class Id>
    Id fresh() {
        Id id = new_id<Id>(rng);
        while (used_ids.count(id.value)) id = new_id<Id>(rng);
        used_ids.insert(id.value);
        return id;
    }

    // An id field. Missing or bad: nullopt. A duplicate gets a fresh id (logged).
    template <class Id>
    std::optional<Id> read_id(const Value& obj, std::string_view key, std::string_view what) {
        const Value* v = obj.get(key);
        Id id{};
        if (!v || !v->as_string() || !parse_id(*v->as_string(), id)) return std::nullopt;
        if (!used_ids.insert(id.value).second) {
            log.notes.push_back("duplicate " + std::string(what) + " id " + id.to_string() + " got a new id");
            return fresh<Id>();
        }
        return id;
    }

    template <class Id>
    std::optional<Id> ref_id(const Value& obj, std::string_view key) {
        const Value* v = obj.get(key);
        Id id{};
        if (!v || !v->as_string() || !parse_id(*v->as_string(), id)) return std::nullopt;
        return id;
    }

    static Value extra_of(const Value& obj, std::initializer_list<std::string_view> known) {
        Value e = Value::object();
        for (const auto& [k, v] : obj.members())
            if (std::find(known.begin(), known.end(), k) == known.end()) e.set(k, v);
        return e;
    }

    static std::string string_or(const Value& obj, std::string_view key, std::string fallback = {}) {
        const Value* v = obj.get(key);
        return v && v->as_string() ? *v->as_string() : fallback;
    }

    static std::optional<ShellSpec> shell(const Value* v) {
        if (!v || !v->is_object()) return std::nullopt;
        ShellSpec sh;
        sh.program = string_or(*v, "program");
        if (trim(sh.program).empty()) return std::nullopt;
        if (const Value* a = v->get("args"); a && a->as_array())
            for (const auto& x : *a->as_array())
                if (x.as_string()) sh.args.push_back(*x.as_string());
        return sh;
    }

    static std::optional<agents::AgentResume> agent(const Value* v) {
        if (!v || !v->is_object()) return std::nullopt;
        agents::AgentResume r;
        r.agent = string_or(*v, "agent");
        r.session_id = string_or(*v, "session_id");
        r.directory = string_or(*v, "directory");
        if (r.agent.empty() || r.session_id.empty() || r.directory.empty()) return std::nullopt;
        if (const Value* a = v->get("arguments")) {
            if (!a->as_array()) return std::nullopt;
            for (const auto& x : *a->as_array()) {
                if (!x.as_string()) return std::nullopt;
                r.arguments.push_back(*x.as_string());
            }
        }
        return r;
    }

    std::optional<PaneId> pane(const Value& v) {
        if (!v.is_object()) {
            corrupt("a pane is not an object");
            return std::nullopt;
        }
        auto id = read_id<PaneId>(v, "id", "pane");
        if (!id) {
            corrupt("a pane without an id");
            return std::nullopt;
        }
        Pane p;
        p.id = *id;
        const std::string sid = string_or(v, "session");
        if (auto parsed = session::SessionId::parse(sid)) {
            p.session = *parsed;
        } else {
            p.session = new_session_id(rng);
            log.notes.push_back("pane " + id->to_string() + " had no valid session id; a new one was assigned");
        }
        p.last_cwd = to_slashes(string_or(v, "cwd"));
        p.last_cwd_source = cwd_source_from_string(string_or(v, "cwd_source", "last_known")).value_or(CwdSource::LastKnown);
        const auto att = attention_from_string(string_or(v, "attention", "idle")).value_or(Attention::Idle);
        p.attention = (att == Attention::Done || att == Attention::Failed) ? att : Attention::Idle;
        if (const Value* pr = v.get("private")) p.is_private = pr->as_bool().value_or(false);
        if (const Value* ro = v.get("read_only")) p.read_only = ro->as_bool().value_or(false);
        p.shell = shell(v.get("shell"));
        p.agent_resume = agent(v.get("agent"));
        p.extra = extra_of(v, {"id", "session", "cwd", "cwd_source", "attention", "private", "read_only", "shell", "agent"});
        s.panes[p.id] = std::move(p);
        return id;
    }

    std::optional<Tab> tab(const Value& v) {
        if (!v.is_object()) {
            corrupt("a tab is not an object");
            return std::nullopt;
        }
        Tab t;
        if (auto id = read_id<TabId>(v, "id", "tab")) {
            t.id = *id;
        } else {
            t.id = fresh<TabId>();
            log.notes.push_back("a tab without an id got a new id");
        }
        const Value* tree = v.get("tree");
        if (!tree) {
            corrupt("a tab without a tree");
            return std::nullopt;
        }
        int repaired = 0;
        auto tr = SplitTree::from_json(*tree, [&](const Value& pv) { return pane(pv); }, &repaired);
        if (!tr || tr->empty()) {
            corrupt(tr ? "an empty split tree" : "a split node without both children");
            return std::nullopt;
        }
        if (repaired) log.notes.push_back("tab " + t.id.to_string() + ": split ratio outside (0, 1) clamped");
        t.tree = std::move(*tr);
        if (const Value* title = v.get("title"); title && title->as_string() && !title->as_string()->empty())
            t.title_override = *title->as_string();
        if (auto f = ref_id<PaneId>(v, "focused")) t.focused = *f;
        if (auto z = ref_id<PaneId>(v, "zoomed")) t.zoomed = *z;
        if (const Value* pin = v.get("pinned")) t.pinned = pin->as_bool().value_or(false);
        t.extra = extra_of(v, {"id", "title", "focused", "zoomed", "pinned", "tree"});
        return t;
    }

    std::optional<ProjectWorkspace> workspace(const Value& v) {
        if (!v.is_object()) {
            corrupt("a workspace is not an object");
            return std::nullopt;
        }
        ProjectWorkspace w;
        auto pid = ref_id<ProjectId>(v, "project");
        if (!pid) {
            log.notes.push_back("a workspace without a project moved to scratch");
            w.project = kScratchProject;
        } else {
            w.project = *pid;
        }
        if (const Value* tabs = v.get("tabs")) {
            if (!tabs->as_array()) {
                corrupt("tabs is not an array");
                return std::nullopt;
            }
            for (const auto& tv : *tabs->as_array()) {
                auto t = tab(tv);
                if (structural) return std::nullopt;
                if (t) w.tabs.push_back(std::move(*t));
            }
        }
        if (auto a = ref_id<TabId>(v, "active_tab")) w.active_tab = *a;
        w.extra = extra_of(v, {"project", "active_tab", "tabs"});
        return w;
    }

    static RectDip rect(const Value* v) {
        RectDip r;
        if (!v || !v->is_object()) return r;
        auto num = [&](std::string_view k) {
            const Value* x = v->get(k);
            return x ? x->as_double().value_or(0.0) : 0.0;
        };
        r.x = num("x");
        r.y = num("y");
        r.w = num("w");
        r.h = num("h");
        return r;
    }

    std::optional<WindowState> window(const Value& v) {
        if (!v.is_object()) return std::nullopt;
        WindowState w;
        if (auto id = read_id<WindowId>(v, "id", "window")) w.id = *id;
        else w.id = fresh<WindowId>();
        w.shown = ref_id<ProjectId>(v, "shown").value_or(kScratchProject);
        w.active_tab = ref_id<TabId>(v, "active_tab");
        w.frame = rect(v.get("frame"));
        w.monitor_key = string_or(v, "monitor");
        if (const Value* sc = v.get("scale")) w.monitor_scale = sc->as_double().value_or(1.0);
        if (!(w.monitor_scale > 0)) w.monitor_scale = 1.0;
        if (const Value* m = v.get("maximized")) w.maximized = m->as_bool().value_or(false);
        if (const Value* f = v.get("fullscreen")) w.fullscreen = f->as_bool().value_or(false);
        if (const Value* sb = v.get("sidebar"); sb && sb->is_object()) {
            if (const Value* x = sb->get("width")) w.sidebar_width = x->as_double().value_or(240);
            if (const Value* x = sb->get("visible")) w.sidebar_visible = x->as_bool().value_or(true);
            if (const Value* x = sb->get("expanded"); x && x->as_array())
                for (const auto& e : *x->as_array()) {
                    ProjectId p{};
                    if (e.as_string() && parse_id(*e.as_string(), p)) w.expanded.push_back(p);
                }
        }
        if (const Value* k = v.get("key")) w.key = k->as_bool().value_or(false);
        w.extra = extra_of(v, {"id", "shown", "active_tab", "frame", "monitor", "scale", "maximized", "fullscreen",
                               "sidebar", "key"});
        return w;
    }

    std::optional<Project> project(const Value& v) {
        if (!v.is_object()) return std::nullopt;
        Project p;
        auto id = read_id<ProjectId>(v, "id", "project");
        if (!id || is_fixed_workspace(*id)) {
            p.id = fresh<ProjectId>();
            log.notes.push_back("a project without a valid id got a new id");
        } else {
            p.id = *id;
        }
        p.path = to_slashes(string_or(v, "path"));
        p.name = trim(string_or(v, "name"));
        if (p.name.empty()) p.name = p.path.empty() ? "Project" : folder_name(p.path);
        if (p.name.size() > 128) p.name.resize(128);
        p.color = color_from_string(string_or(v, "color", "none")).value_or(ColorTag::None);
        p.shell = shell(v.get("shell"));
        p.extra = extra_of(v, {"id", "name", "path", "color", "shell"});
        return p;
    }
};

}  // namespace

void repair_state(StateSnapshot& s, IRandom& rng, RepairLog& log) {
    // Scratch exists exactly once; workspaces of unknown projects move their tabs to scratch.
    std::set<ProjectId> known;
    for (const auto& p : s.projects) known.insert(p.id);
    std::vector<ProjectWorkspace> merged;
    ProjectWorkspace scratch{kScratchProject, {}, {}, {}};
    bool have_scratch = false;
    std::set<ProjectId> placed;
    for (auto& w : s.workspaces) {
        if (w.project == kScratchProject || !known.count(w.project) || placed.count(w.project)) {
            if (w.project != kScratchProject)
                log.notes.push_back("tabs of an unknown or repeated project " + w.project.to_string() + " moved to scratch");
            if (w.project == kScratchProject && !have_scratch) {
                scratch.active_tab = w.active_tab;
                scratch.extra = w.extra;
            }
            have_scratch = have_scratch || w.project == kScratchProject;
            for (auto& t : w.tabs) scratch.tabs.push_back(std::move(t));
            continue;
        }
        placed.insert(w.project);
        merged.push_back(std::move(w));
    }
    for (const auto& p : s.projects)
        if (!placed.count(p.id)) merged.push_back(ProjectWorkspace{p.id, {}, {}, {}});
    merged.push_back(std::move(scratch));
    s.workspaces = std::move(merged);

    // Panes: one tree position each; duplicate session ids get fresh ids (first keeps it).
    std::set<session::SessionId> sessions;
    std::set<PaneId> in_trees;
    auto fix_ws = [&](ProjectWorkspace& w) {
        for (auto& t : w.tabs) {
            const auto panes = t.tree.panes();
            for (PaneId p : panes) {
                in_trees.insert(p);
                auto it = s.panes.find(p);
                if (it == s.panes.end()) continue;
                if (!sessions.insert(it->second.session).second) {
                    log.notes.push_back("pane " + p.to_string() + " shared a session id; it got a new one");
                    it->second.session = new_session_id(rng);
                    while (!sessions.insert(it->second.session).second) it->second.session = new_session_id(rng);
                }
            }
            if (!t.tree.contains(t.focused)) {
                if (!t.focused.is_nil()) log.notes.push_back("tab " + t.id.to_string() + ": focused pane fixed");
                t.focused = panes.front();
            }
            if (t.zoomed && !t.tree.contains(*t.zoomed)) {
                log.notes.push_back("tab " + t.id.to_string() + ": zoomed pane cleared");
                t.zoomed.reset();
            }
            if (t.zoomed && t.tree.size() < 2) t.zoomed.reset();
        }
        if (w.active_tab && std::none_of(w.tabs.begin(), w.tabs.end(), [&](const Tab& t) { return t.id == *w.active_tab; })) {
            log.notes.push_back("a dangling active tab was fixed");
            w.active_tab.reset();
        }
        if (!w.active_tab && !w.tabs.empty()) w.active_tab = w.tabs.front().id;
    };
    for (auto& w : s.workspaces) fix_ws(w);
    s.quick.workspace.project = kQuickProject;
    fix_ws(s.quick.workspace);
    for (auto it = s.panes.begin(); it != s.panes.end();) {
        if (!in_trees.count(it->first)) it = s.panes.erase(it);
        else ++it;
    }

    // Windows.
    std::set<WindowId> wins;
    bool key_seen = false;
    for (auto& w : s.windows) {
        if (!wins.insert(w.id).second) {
            w.id = new_id<WindowId>(rng);
            wins.insert(w.id);
        }
        if (w.shown != kScratchProject && !known.count(w.shown)) {
            log.notes.push_back("a window showing an unknown project now shows scratch");
            w.shown = kScratchProject;
            w.active_tab.reset();
        }
        const ProjectWorkspace* pw = nullptr;
        for (const auto& x : s.workspaces)
            if (x.project == w.shown) pw = &x;
        if (w.active_tab && pw &&
            std::none_of(pw->tabs.begin(), pw->tabs.end(), [&](const Tab& t) { return t.id == *w.active_tab; }))
            w.active_tab.reset();
        if (!w.active_tab && pw) w.active_tab = pw->active_tab;
        std::vector<ProjectId> ex;
        for (const auto& p : w.expanded)
            if (known.count(p)) ex.push_back(p);
        std::sort(ex.begin(), ex.end());
        ex.erase(std::unique(ex.begin(), ex.end()), ex.end());
        w.expanded = std::move(ex);
        if (w.key && key_seen) w.key = false;
        key_seen = key_seen || w.key;
    }
    if (!key_seen && !s.windows.empty()) s.windows.front().key = true;

    if (s.recent_folders.size() > kMaxRecentFolders) s.recent_folders.resize(kMaxRecentFolders);
}

WsResult<ParseOutcome> parse_state(std::string_view text, IRandom& rng) {
    Value doc;
    json::ParseError perr;
    if (!json::parse(text, doc, &perr))
        return ws_error(WsErrc::StateCorrupt, "the state file is not valid JSON",
                        perr.message + " at byte " + std::to_string(perr.offset));
    if (!doc.is_object()) return ws_error(WsErrc::StateCorrupt, "the state file is not a JSON object");
    const Value* schema = doc.get("schema");
    if (!schema || !schema->as_int() || schema->kind() != Value::Kind::Int)
        return ws_error(WsErrc::StateCorrupt, "the state file has no schema version");
    ParseOutcome out;
    out.file_schema = static_cast<int>(*schema->as_int());
    if (out.file_schema < 1) return ws_error(WsErrc::StateCorrupt, "bad schema version");
    if (out.file_schema > kSchemaVersion) {
        out.newer_schema = true;
    } else if (out.file_schema < kSchemaVersion) {
        if (!migrate(doc, out.file_schema, kSchemaVersion, migrations()))
            return ws_error(WsErrc::StateCorrupt, "no migration from schema " + std::to_string(out.file_schema));
    }

    StateSnapshot& s = out.state;
    Reader rd{rng, out.repairs, s, {}, false, {}};
    rd.used_ids.insert(kScratchProject.value);
    rd.used_ids.insert(kQuickProject.value);
    auto array_of = [&](std::string_view key) -> const std::vector<Value>* {
        const Value* v = doc.get(key);
        if (!v || v->is_null()) return nullptr;
        if (!v->as_array()) rd.corrupt(std::string(key) + " is not an array");
        return v->as_array();
    };
    if (const auto* arr = array_of("projects"))
        for (const auto& v : *arr)
            if (auto p = rd.project(v)) s.projects.push_back(std::move(*p));
    if (const auto* arr = array_of("workspaces"))
        for (const auto& v : *arr) {
            auto w = rd.workspace(v);
            if (rd.structural) break;
            if (w) s.workspaces.push_back(std::move(*w));
        }
    if (const Value* q = doc.get("quick_terminal"); q && q->is_object() && !rd.structural) {
        if (const Value* w = q->get("workspace"); w && w->is_object()) {
            if (auto qw = rd.workspace(*w)) {
                s.quick.workspace = std::move(*qw);
                s.quick.workspace.project = kQuickProject;
            }
        }
        if (const Value* f = q->get("frames"); f && f->is_object())
            for (const auto& [k, v] : f->members()) {
                const RectDip r = Reader::rect(&v);
                s.quick.frames[k] = RectFraction{r.x, r.y, r.w, r.h};
            }
        s.quick.extra = Reader::extra_of(*q, {"workspace", "frames"});
    }
    if (const auto* arr = array_of("recent_folders"))
        for (const auto& v : *arr) {
            if (!v.is_object()) continue;
            RecentFolder r;
            r.path = Reader::string_or(v, "path");
            if (r.path.empty()) continue;
            r.used_unix_ms = parse_iso8601(Reader::string_or(v, "used_at")).value_or(0);
            s.recent_folders.push_back(std::move(r));
        }
    if (const auto* arr = array_of("windows"))
        for (const auto& v : *arr)
            if (auto w = rd.window(v)) s.windows.push_back(std::move(*w));
    if (rd.structural) return ws_error(WsErrc::StateCorrupt, "the state file is damaged", rd.why);
    s.extra = Reader::extra_of(doc, {"schema", "writer", "saved_at", "projects", "workspaces", "quick_terminal",
                                     "recent_folders", "windows"});
    repair_state(s, rng, out.repairs);
    return out;
}

StateSnapshot default_state(IRandom& rng, std::string_view home) {
    StateSnapshot s;
    Pane p;
    p.id = new_id<PaneId>(rng);
    p.session = new_session_id(rng);
    p.last_cwd = to_slashes(home);
    p.never_started = true;
    Tab t;
    t.id = new_id<TabId>(rng);
    t.tree = SplitTree(p.id);
    t.focused = p.id;
    t.focus_history = {p.id};
    ProjectWorkspace scratch{kScratchProject, {}, t.id, {}};
    scratch.tabs.push_back(std::move(t));
    WindowState w;
    w.id = new_id<WindowId>(rng);
    w.shown = kScratchProject;
    w.active_tab = scratch.active_tab;
    w.key = true;
    s.panes[p.id] = std::move(p);
    s.workspaces.push_back(std::move(scratch));
    s.windows.push_back(std::move(w));
    s.quick.workspace.project = kQuickProject;
    return s;
}

StateSnapshot state_from_legacy(std::span<const LegacyPane> panes, IRandom& rng, std::string_view home) {
    if (panes.empty()) return default_state(rng, home);
    std::vector<LegacyPane> sorted(panes.begin(), panes.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const LegacyPane& a, const LegacyPane& b) { return a.updated_unix_ms < b.updated_unix_ms; });
    StateSnapshot s;
    ProjectWorkspace scratch{kScratchProject, {}, {}, {}};
    std::set<session::SessionId> seen;
    for (const auto& lp : sorted) {
        if (!seen.insert(lp.session).second) continue;
        Pane p;
        p.id = new_id<PaneId>(rng);
        p.session = lp.session;
        p.last_cwd = to_slashes(lp.cwd.empty() ? std::string(home) : lp.cwd);
        p.last_cwd_source = cwd_source_from_string(lp.cwd_source).value_or(CwdSource::LastKnown);
        Tab t;
        t.id = new_id<TabId>(rng);
        t.tree = SplitTree(p.id);
        t.focused = p.id;
        t.focus_history = {p.id};
        WindowState w;
        w.id = new_id<WindowId>(rng);
        w.shown = kScratchProject;
        w.active_tab = t.id;
        scratch.active_tab = t.id;
        s.windows.push_back(std::move(w));
        s.panes[p.id] = std::move(p);
        scratch.tabs.push_back(std::move(t));
    }
    if (!s.windows.empty()) s.windows.back().key = true;
    s.workspaces.push_back(std::move(scratch));
    s.quick.workspace.project = kQuickProject;
    return s;
}

// ------------------------------------------------------------------------------ store

std::string_view to_string(LoadStatus s) {
    switch (s) {
        case LoadStatus::Loaded: return "loaded";
        case LoadStatus::Empty: return "empty";
        case LoadStatus::ImportedLegacy: return "imported_legacy";
        case LoadStatus::RestoredFromBackup: return "restored_from_backup";
        case LoadStatus::ResetAfterCorrupt: return "reset_after_corrupt";
        case LoadStatus::NewerSchema: return "newer_schema";
        case LoadStatus::ReadOnlyIo: return "read_only_io";
    }
    return "loaded";
}

StateStore::StateStore(IFileSystem& fs, IClock& clock, IRandom& rng, std::string state_dir)
    : fs_(fs), clock_(clock), rng_(rng), dir_(std::move(state_dir)) {}

std::string StateStore::timestamp() const { return file_stamp(clock_.unix_ms()); }

std::string StateStore::quarantine(const std::string& path) {
    const std::string base = path + ".corrupt-" + timestamp();
    std::string name = base;
    for (int n = 2; fs_.exists(name); ++n) name = base + "-" + std::to_string(n);
    if (!fs_.rename(path, name)) return {};
    // Keep at most 5; the timestamp makes the names sort by age.
    const std::string prefix = std::string(kStateFileName) + ".corrupt-";
    std::vector<std::string> old;
    for (const auto& f : fs_.list(dir_))
        if (f.rfind(prefix, 0) == 0) old.push_back(f);
    std::sort(old.begin(), old.end());
    while (old.size() > kMaxCorruptFiles) {
        (void)fs_.remove(path_join(dir_, old.front()));
        old.erase(old.begin());
    }
    return name;
}

void StateStore::clean_temp_files() {
    const std::string prefix = std::string(kStateFileName) + ".tmp-";
    const std::int64_t now = clock_.unix_ms();
    for (const auto& f : fs_.list(dir_)) {
        if (f.rfind(prefix, 0) != 0) continue;
        const std::string p = path_join(dir_, f);
        const auto m = fs_.mtime_unix_ms(p);
        if (m && now - *m > 60'000) (void)fs_.remove(p);
    }
}

LoadOutcome StateStore::load(std::string_view home, const std::function<std::vector<LegacyPane>()>& legacy,
                             const std::function<void(int)>& sleep_ms) {
    LoadOutcome out;
    (void)fs_.make_private_dir(dir_);
    clean_temp_files();
    ReadResult r = fs_.read(file());
    for (int i = 0; i < 3 && r.status == ReadStatus::Error; ++i) {
        if (sleep_ms) sleep_ms(50);
        r = fs_.read(file());
    }
    if (r.status == ReadStatus::Error) {
        out.state = default_state(rng_, home);
        out.status = LoadStatus::ReadOnlyIo;
        out.read_only = true;
        out.notice = "The workspace file cannot be read (" + r.error + "). Changes are not saved in this session.";
        return out;
    }
    if (r.status == ReadStatus::Missing || trim(r.data).empty()) {
        std::vector<LegacyPane> panes;
        if (r.status == ReadStatus::Missing && legacy) panes = legacy();
        if (!panes.empty()) {
            out.state = state_from_legacy(panes, rng_, home);
            out.status = LoadStatus::ImportedLegacy;
        } else {
            out.state = default_state(rng_, home);
            out.status = LoadStatus::Empty;
        }
        return out;
    }
    auto parsed = parse_state(r.data, rng_);
    if (parsed) {
        out.state = std::move(parsed->state);
        out.repairs = std::move(parsed->repairs);
        out.file_schema = parsed->file_schema;
        if (parsed->newer_schema) {
            out.status = LoadStatus::NewerSchema;
            out.read_only = true;
            out.notice = "The workspace was saved by a newer keepterm (schema " + std::to_string(parsed->file_schema) +
                         "). Changes are not saved unless you choose Save Anyway.";
        } else {
            out.status = LoadStatus::Loaded;
            (void)fs_.write_atomic(backup_file(), r.data);   // the last file known to load
        }
        return out;
    }
    // Corrupt: keep it aside, never overwrite it.
    out.corrupt_file = quarantine(file());
    const std::string shown = out.corrupt_file.empty() ? file() : out.corrupt_file;
    ReadResult b = fs_.read(backup_file());
    if (b.status == ReadStatus::Ok) {
        if (auto pb = parse_state(b.data, rng_)) {
            out.state = std::move(pb->state);
            out.repairs = std::move(pb->repairs);
            out.file_schema = pb->file_schema;
            out.status = LoadStatus::RestoredFromBackup;
            out.read_only = pb->newer_schema;
            out.notice = "The workspace file was damaged and was restored from the backup. The damaged file was kept as " +
                         shown + ".";
            return out;
        }
    }
    out.state = default_state(rng_, home);
    out.status = LoadStatus::ResetAfterCorrupt;
    out.notice = "The workspace file was damaged and no backup could be loaded. The damaged file was kept as " + shown + ".";
    return out;
}

WsResult<void> StateStore::write(std::string_view bytes) {
    (void)fs_.make_private_dir(dir_);
    return fs_.write_atomic(file(), bytes);
}

WsResult<void> StateStore::backup_newer(int file_schema) {
    ReadResult r = fs_.read(file());
    if (r.status != ReadStatus::Ok) return {};
    return fs_.write_atomic(file() + ".v" + std::to_string(file_schema) + "-backup", r.data);
}

}  // namespace kt::ws
