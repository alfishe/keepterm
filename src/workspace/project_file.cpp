#include "workspace/project_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "workspace/workspace.h"
#include "workspace/yaml.h"

namespace kt::ws {

namespace {

constexpr int kMaxDepthDecl = 32;
constexpr std::size_t kMaxFileBytes = 1u << 20;
constexpr std::size_t kMaxPanesPerTab = 64;

std::string lower_ascii(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return o;
}

WsError schema_error(const yaml::Node& n, std::string msg) {
    char b[64];
    std::snprintf(b, sizeof b, "line %d, column %d: ", n.line, n.col);
    return WsError{WsErrc::LayoutParse, b + msg, {}};
}

// A string field: absent or null -> nullopt; a mapping or sequence -> error.
Expected<std::optional<std::string>, WsError> str_field(const yaml::Node& m, std::string_view key) {
    const yaml::Node* v = m.get(key);
    if (!v || v->is_null()) return std::optional<std::string>{};
    if (v->kind != yaml::Node::Kind::Scalar) return Unexpected<WsError>(schema_error(*v, "\"" + std::string(key) + "\" must be a string"));
    return std::optional<std::string>{v->text};
}

Expected<PresetNode, WsError> node_of(const yaml::Node& n, int depth, bool tab_level, std::optional<std::string>* name) {
    if (depth > kMaxDepthDecl) return Unexpected<WsError>(schema_error(n, "the layout is nested too deep"));
    PresetNode out;
    if (n.is_null()) {   // "second:" with nothing: a plain shell, like {}
        out.pane = PresetPane{};
        return out;
    }
    if (n.kind != yaml::Node::Kind::Map) return Unexpected<WsError>(schema_error(n, tab_level ? "a tab must be a mapping" : "a node must be a mapping"));
    if (tab_level && name) {
        auto nm = str_field(n, "name");
        if (!nm) return Unexpected<WsError>(nm.error());
        *name = std::move(*nm);
    }
    if (const yaml::Node* s = n.get("split"); s && !s->is_null()) {
        if (s->kind != yaml::Node::Kind::Map) return Unexpected<WsError>(schema_error(*s, "\"split\" must be a mapping"));
        auto dir = str_field(*s, "direction");
        if (!dir) return Unexpected<WsError>(dir.error());
        if (!*dir) return Unexpected<WsError>(schema_error(*s, "a split needs \"direction\" (horizontal or vertical)"));
        if (**dir == "horizontal") out.axis = Axis::Horizontal;
        else if (**dir == "vertical") out.axis = Axis::Vertical;
        else return Unexpected<WsError>(schema_error(*s->get("direction"), "\"direction\" must be horizontal or vertical"));
        out.ratio = 0.5;
        if (const yaml::Node* r = s->get("ratio"); r && !r->is_null()) {
            char* end = nullptr;
            const std::string t = r->text;
            const double v = r->kind == yaml::Node::Kind::Scalar ? std::strtod(t.c_str(), &end) : -1.0;
            if (r->kind != yaml::Node::Kind::Scalar || t.empty() || !end || *end != '\0' || !(v >= 0.0 && v <= 1.0))
                return Unexpected<WsError>(schema_error(*r, "\"ratio\" must be a number from 0 to 1"));
            out.ratio = v;
        }
        const yaml::Node* f = s->get("first");
        const yaml::Node* sc = s->get("second");
        if (!f || !sc) return Unexpected<WsError>(schema_error(*s, "a split needs both \"first\" and \"second\""));
        auto a = node_of(*f, depth + 1, false, nullptr);
        if (!a) return a;
        auto b = node_of(*sc, depth + 1, false, nullptr);
        if (!b) return b;
        out.first = std::make_unique<PresetNode>(std::move(*a));
        out.second = std::make_unique<PresetNode>(std::move(*b));
        return out;
    }
    PresetPane p;
    auto cwd = str_field(n, "cwd");
    if (!cwd) return Unexpected<WsError>(cwd.error());
    auto run = str_field(n, "run");
    if (!run) return Unexpected<WsError>(run.error());
    auto shell = str_field(n, "shell");
    if (!shell) return Unexpected<WsError>(shell.error());
    if (*cwd) p.cwd = **cwd;
    if (*run && !(*run)->empty()) p.run = **run;
    if (*shell && !(*shell)->empty()) p.shell = ShellSpec{**shell, {}};
    out.pane = std::move(p);
    return out;
}

void indent(std::string& o, int n) { o.append(static_cast<std::size_t>(n), ' '); }

std::string ratio_text(double r) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", std::clamp(r, 0.0, 1.0));
    std::string s = b;
    while (s.size() > 3 && s.back() == '0') s.pop_back();   // "0.50" -> "0.5"; "1.00" -> "1.0"
    return s;
}

bool pane_empty(const PresetPane& p) { return p.cwd.empty() && !p.run && !p.shell; }

// The fields of a node at `ind` (the first line already indented by the caller when `first_inline`).
void write_node(std::string& o, const PresetNode& n, int ind, bool first_inline, const std::optional<std::string>& name) {
    bool first = first_inline;
    auto line = [&](std::string_view key, const std::string& value) {
        if (!first) indent(o, ind);
        first = false;
        o += key;
        o += ": ";
        o += value;
        o += '\n';
    };
    if (name) line("name", yaml::scalar(*name));
    if (n.pane || !n.first || !n.second) {
        const PresetPane p = n.pane ? *n.pane : PresetPane{};
        if (!p.cwd.empty()) line("cwd", yaml::scalar(p.cwd));
        if (p.run) line("run", yaml::scalar(*p.run));
        if (p.shell) line("shell", yaml::scalar(p.shell->program));
        if (first) o += "{}\n";   // a bare plain shell (no name): "- {}"
        return;
    }
    if (!first) indent(o, ind);
    o += "split:\n";
    indent(o, ind + 2);
    o += std::string("direction: ") + (n.axis == Axis::Horizontal ? "horizontal" : "vertical") + "\n";
    indent(o, ind + 2);
    o += "ratio: " + ratio_text(n.ratio) + "\n";
    for (int side = 0; side < 2; ++side) {
        const PresetNode& c = side == 0 ? *n.first : *n.second;
        indent(o, ind + 2);
        o += side == 0 ? "first:" : "second:";
        if (c.pane && pane_empty(*c.pane)) {
            o += " {}\n";
            continue;
        }
        o += '\n';
        write_node(o, c, ind + 4, false, std::nullopt);
    }
}

bool is_drive(std::string_view p, PathStyle style) {
    return style == PathStyle::Windows && p.size() >= 2 && p[1] == ':' &&
           ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
}

bool is_absolute(std::string_view p, PathStyle style) {
    if (p.starts_with("/")) return true;
    if (style == PathStyle::Windows) return is_drive(p, style) || p.starts_with("\\");
    return false;
}

std::string expand_home(std::string_view p, std::string_view home) {
    if (p == "~") return std::string(home);
    if (p.starts_with("~/") || p.starts_with("~\\")) return path_join(home, p.substr(2));
    return std::string(p);
}

// `folder` inside `root` (canonical comparison): the rest ("" for the root itself), else nullopt.
std::optional<std::string> inside(std::string_view folder, std::string_view root, PathStyle style) {
    const std::string f = canonical_path(folder, style), r = canonical_path(root, style);
    if (r.empty()) return std::nullopt;
    if (f == r) return std::string();
    const std::string pre = r.ends_with("/") ? r : r + "/";
    if (!f.starts_with(pre)) return std::nullopt;
    // The rest in the folder's own spelling (canonical_path lowercases on Windows).
    const std::string fs = to_slashes(folder, style);
    std::string_view rest(fs);
    const auto n = static_cast<std::size_t>(std::count(pre.begin(), pre.end(), '/'));
    std::size_t seen = 0, i = 0;
    for (; i < rest.size() && seen < n; ++i) {
        if (rest[i] == '/') ++seen;
    }
    if (seen < n) return f.substr(pre.size());
    std::string out(rest.substr(i));
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

}  // namespace

// ------------------------------------------------------------------------------ the file

WsResult<ProjectFile> parse_project_file(std::string_view text) {
    auto doc = yaml::parse(text);
    if (!doc) {
        char b[64];
        std::snprintf(b, sizeof b, "line %d, column %d: ", doc.error().line, doc.error().col);
        return ws_error(WsErrc::LayoutParse, b + doc.error().message);
    }
    const yaml::Node& root = *doc;
    if (root.kind != yaml::Node::Kind::Map) return ws_error(WsErrc::LayoutParse, "the file must be a mapping with \"path\"");
    ProjectFile f;
    auto name = str_field(root, "name");
    if (!name) return Unexpected<WsError>(name.error());
    f.name = std::move(*name);
    auto path = str_field(root, "path");
    if (!path) return Unexpected<WsError>(path.error());
    if (!*path || (*path)->empty()) return ws_error(WsErrc::LayoutParse, "\"path\" is required");
    f.path = **path;
    if (const yaml::Node* t = root.get("tabs"); t && !t->is_null()) {
        if (t->kind != yaml::Node::Kind::Seq) return Unexpected<WsError>(schema_error(*t, "\"tabs\" must be a list"));
        std::vector<PresetTab> tabs;
        for (const yaml::Node& tn : t->seq) {
            PresetTab tab;
            auto node = node_of(tn, 0, true, &tab.title);
            if (!node) return Unexpected<WsError>(node.error());
            tab.layout = std::move(*node);
            if (tab.layout.pane_count() > kMaxPanesPerTab) return Unexpected<WsError>(schema_error(tn, "a tab has too many panes"));
            tabs.push_back(std::move(tab));
        }
        f.tabs = std::move(tabs);
    }
    return f;
}

std::optional<ProjectHeader> parse_project_header(std::string_view text) {
    auto doc = yaml::parse(text);
    if (!doc) return std::nullopt;
    ProjectHeader h;
    if (doc->kind != yaml::Node::Kind::Map) return h;
    if (const yaml::Node* n = doc->get("name"); n && n->is_scalar()) h.name = n->text;
    if (const yaml::Node* p = doc->get("path"); p && p->is_scalar()) h.path = p->text;
    return h;
}

std::string write_project_file(const ProjectFile& f) {
    std::string o(kProjectSchemaModeline);
    o += '\n';
    if (f.name) o += "name: " + yaml::scalar(*f.name) + "\n";
    o += "path: " + yaml::scalar(f.path) + "\n";
    if (f.tabs && !f.tabs->empty()) {
        o += "tabs:\n";
        for (const PresetTab& t : *f.tabs) {
            o += "- ";
            write_node(o, t.layout, 2, true, t.title && !t.title->empty() ? t.title : std::nullopt);
        }
    }
    return o;
}

std::string project_slug(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') out += '_';
        else if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += static_cast<char>(c);
        else if (c >= 0x80) out += static_cast<char>(c);   // non-ASCII letters (UTF-8) are kept
    }
    return out.empty() ? std::string("project") : out;
}

std::string slug_filename(std::string_view slug, int attempt) {
    return attempt <= 1 ? std::string(slug) + ".yaml" : std::string(slug) + "_" + std::to_string(attempt) + ".yaml";
}

bool slug_owns(std::string_view filename, std::string_view slug) {
    std::string base = lower_ascii(filename);
    if (const std::size_t dot = base.rfind('.'); dot != std::string::npos && dot > 0) base.resize(dot);
    const std::string s = lower_ascii(slug);
    if (base == s) return true;
    if (!base.starts_with(s + "_")) return false;
    const std::string_view n = std::string_view(base).substr(s.size() + 1);
    return !n.empty() && std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool is_remote_spec(std::string_view p, PathStyle style) {
    if (p.empty() || p.starts_with("/") || p.starts_with("~") || p.starts_with(".")) return false;
    if (is_drive(p, style) && (p.size() == 2 || p[2] == '/' || p[2] == '\\')) return false;
    const std::size_t colon = p.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::size_t slash = p.find_first_of("/\\");
    return slash == std::string_view::npos || slash > colon;
}

std::optional<std::string> project_path_key(std::string_view path, std::string_view home, PathStyle style) {
    if (path.empty() || is_remote_spec(path, style)) return std::nullopt;
    return canonical_path(expand_home(path, home), style);
}

std::string resolve_layout_cwd(std::string_view cwd, std::string_view root, std::string_view home, PathStyle style) {
    if (cwd.empty() || cwd == ".") return std::string(root);
    if (cwd == "~" || cwd.starts_with("~/") || cwd.starts_with("~\\")) return expand_home(cwd, home);
    if (is_absolute(cwd, style)) return std::string(cwd);
    return path_join(root, cwd);
}

std::string layout_cwd_of(std::string_view folder, std::string_view root, std::string_view home, PathStyle style) {
    if (folder.empty()) return {};
    if (auto rel = inside(folder, root, style)) return *rel;
    if (style == PathStyle::Posix && !home.empty()) {
        if (auto rel = inside(folder, home, style)) return rel->empty() ? std::string("~") : "~/" + *rel;
    }
    return std::string(folder);
}

std::string layout_path_of(std::string_view path, std::string_view home, PathStyle style) {
    if (style == PathStyle::Posix && !home.empty()) {
        if (auto rel = inside(path, home, style)) return rel->empty() ? std::string("~") : "~/" + *rel;
    }
    return std::string(path);
}

// ------------------------------------------------------------------------------ the store

std::vector<ProjectFileStore::Entry> ProjectFileStore::list() const {
    std::vector<Entry> out;
    std::vector<std::pair<std::string, bool>> dirs{{dir_, false}};
    for (const auto& d : ro_) dirs.emplace_back(d, true);
    for (const auto& [d, ro] : dirs) {
        if (d.empty()) continue;
        std::vector<std::string> names = fs_.list(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            const std::string low = lower_ascii(n);
            if (!low.ends_with(".yaml") && !low.ends_with(".yml")) continue;
            Entry e;
            e.file = path_join(d, n);
            e.name = n;
            e.read_only = ro;
            const ReadResult r = fs_.read(e.file);
            if (r.status != ReadStatus::Ok || r.data.size() > kMaxFileBytes) {
                e.error = "cannot read the file";
            } else {
                e.header = parse_project_header(r.data);
                if (!e.header) {
                    auto p = yaml::parse(r.data);
                    e.error = p ? std::string() : p.error().message;
                }
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::optional<ProjectFileStore::Found> ProjectFileStore::find(std::string_view path, std::string_view project_name) const {
    const auto key = project_path_key(path, home_);
    if (!key) return std::nullopt;
    const std::string slug = project_slug(project_name);
    std::vector<Entry> matches;
    for (Entry& e : list()) {
        if (!e.header || !e.header->path) continue;
        const auto k = project_path_key(*e.header->path, home_);
        if (k && *k == *key) matches.push_back(std::move(e));
    }
    if (matches.empty()) return std::nullopt;
    const Entry* pick = &matches.front();
    for (const Entry& e : matches) {
        if (slug_owns(e.name, slug)) {
            pick = &e;
            break;
        }
    }
    const ReadResult r = fs_.read(pick->file);
    if (r.status != ReadStatus::Ok) return std::nullopt;
    Found f;
    f.file = pick->file;
    f.text = r.data;
    f.read_only = pick->read_only;
    f.duplicates = matches.size() - 1;
    return f;
}

WsResult<std::string> ProjectFileStore::save(const ProjectFile& f, std::string_view project_name) {
    if (dir_.empty()) return ws_error(WsErrc::Io, "no folder for project files");
    const std::string slug = project_slug(project_name);
    std::string target;
    if (auto key = project_path_key(f.path, home_)) {
        // The project's own file in keepterm's folder: one the slug owns, else the only match.
        std::vector<Entry> own;
        for (Entry& e : list()) {
            if (e.read_only || !e.header || !e.header->path) continue;
            const auto k = project_path_key(*e.header->path, home_);
            if (k && *k == *key) own.push_back(std::move(e));
        }
        for (const Entry& e : own) {
            if (slug_owns(e.name, slug)) {
                target = e.file;
                break;
            }
        }
        if (target.empty() && own.size() == 1) target = own.front().file;
    }
    if (target.empty()) {
        for (int attempt = 1; attempt < 1000 && target.empty(); ++attempt) {
            const std::string candidate = path_join(dir_, slug_filename(slug, attempt));
            if (!fs_.exists(candidate)) target = candidate;
        }
        if (target.empty()) return ws_error(WsErrc::Io, "no free file name for the project");
    }
    if (auto r = fs_.make_private_dir(dir_); !r) return Unexpected<WsError>(r.error());
    if (auto r = fs_.write_atomic(target, write_project_file(f)); !r) return Unexpected<WsError>(r.error());
    return target;
}

// ------------------------------------------------------------------------------ the plan

std::size_t LayoutPlan::kept() const {
    std::size_t n = 0;
    for (const auto& t : tabs) {
        for (const auto& k : t.keep) n += k.has_value();
    }
    return n;
}

namespace {

std::string shell_base(std::string_view program) {
    std::string s = lower_ascii(program);
    if (const std::size_t sl = s.find_last_of("/\\"); sl != std::string::npos) s.erase(0, sl + 1);
    if (s.ends_with(".exe")) s.resize(s.size() - 4);
    if (s.starts_with("-")) s.erase(0, 1);
    return s;
}

struct Pool {
    std::map<std::pair<std::string, std::string>, std::vector<PaneId>> by_identity;   // (command, cwd key)
    // (pane, shells): the live shell image and the pane's configured shell (on macOS /bin/sh
    // runs as bash, so either name matches)
    std::vector<std::pair<PaneId, std::vector<std::string>>> idle;

    std::optional<PaneId> take(const std::optional<std::string>& command, const std::string& cwd_key, const std::optional<ShellSpec>& shell) {
        if (!command) {
            const std::string want = shell ? shell_base(shell->program) : std::string();
            for (auto it = idle.begin(); it != idle.end(); ++it) {
                if (want.empty() || std::find(it->second.begin(), it->second.end(), want) != it->second.end()) {
                    const PaneId p = it->first;
                    idle.erase(it);
                    return p;
                }
            }
            return std::nullopt;
        }
        auto it = by_identity.find({*command, cwd_key});
        if (it == by_identity.end() || it->second.empty()) return std::nullopt;
        const PaneId p = it->second.front();
        it->second.erase(it->second.begin());
        return p;
    }
};

void plan_tree(PresetNode& n, std::string_view root, std::string_view home, Pool& pool, std::vector<std::optional<PaneId>>& keep,
               std::vector<PaneId>& reused) {
    if (n.pane || !n.first || !n.second) {
        if (!n.pane) n.pane = PresetPane{};
        PresetPane& p = *n.pane;
        p.cwd = resolve_layout_cwd(p.cwd, root, home);
        const auto got = pool.take(p.run, canonical_path(p.cwd), p.shell);
        keep.push_back(got);
        if (got) reused.push_back(*got);
        return;
    }
    plan_tree(*n.first, root, home, pool, keep, reused);
    plan_tree(*n.second, root, home, pool, keep, reused);
}

}  // namespace

LayoutPlan plan_layout(const Workspace& ws, ProjectId project, const std::vector<PresetTab>& tabs, std::string_view root,
                       std::string_view home, const LivePaneFactsFn& facts) {
    LayoutPlan plan;
    const ProjectWorkspace* w = ws.workspace_of(project);
    std::vector<const Tab*> live;
    if (w)
        for (const Tab& t : w->tabs) live.push_back(&t);
    std::vector<bool> used(live.size(), false);
    std::vector<PaneId> reused;
    std::map<PaneId, LivePaneFacts> fact_of;
    auto fact = [&](PaneId id) -> const LivePaneFacts& {
        auto it = fact_of.find(id);
        if (it != fact_of.end()) return it->second;
        LivePaneFacts f;
        if (const Pane* p = ws.pane(id); p && facts) f = facts(*p);
        else if (p) f.cwd = p->last_cwd;
        return fact_of.emplace(id, std::move(f)).first->second;
    };
    // Names first, over all declared tabs (a new tab declared ahead of the others must not take
    // a named live tab by position), then the rest by position among the live tabs left.
    std::vector<std::optional<std::size_t>> matched(tabs.size());
    for (std::size_t d = 0; d < tabs.size(); ++d) {
        if (!tabs[d].title) continue;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (!used[i] && live[i]->title_override && *live[i]->title_override == *tabs[d].title) {
                matched[d] = i;
                used[i] = true;
                break;
            }
        }
    }
    for (std::size_t d = 0; d < tabs.size(); ++d) {
        if (matched[d]) continue;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (used[i]) continue;
            matched[d] = i;
            used[i] = true;
            break;
        }
    }
    for (std::size_t d = 0; d < tabs.size(); ++d) {
        const PresetTab& decl = tabs[d];
        const std::optional<std::size_t> match = matched[d];
        Pool pool;
        LayoutPlanTab pt;
        pt.title = decl.title;
        if (match) {
            pt.existing = live[*match]->id;
            for (PaneId id : live[*match]->tree.panes()) {
                const LivePaneFacts& f = fact(id);
                if (f.command) pool.by_identity[{*f.command, canonical_path(f.cwd)}].push_back(id);
                else {
                    std::vector<std::string> names{shell_base(f.shell)};
                    if (const Pane* lp = ws.pane(id); lp && lp->shell) names.push_back(shell_base(lp->shell->program));
                    pool.idle.emplace_back(id, std::move(names));
                }
            }
        }
        pt.tree = decl.layout;
        plan_tree(pt.tree, root, home, pool, pt.keep, reused);
        plan.tabs.push_back(std::move(pt));
    }
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (!used[i]) plan.close.push_back(live[i]->id);
        for (PaneId id : live[i]->tree.panes()) {
            if (std::find(reused.begin(), reused.end(), id) != reused.end()) continue;
            plan.destroy.push_back(id);
            if (fact(id).command) plan.destroy_busy.push_back(id);
        }
    }
    return plan;
}

namespace {

struct PlanBuilder {
    Workspace& ws;
    LayoutApplyResult& res;

    NewPaneSpec spec(const PresetPane& p, const std::optional<PaneId>& keep) const {
        NewPaneSpec s;
        if (keep) {
            if (const Pane* old = ws.pane(*keep)) {
                s.adopt = old->session;
                if (!old->last_cwd.empty()) s.cwd = old->last_cwd;
                s.shell = old->shell;
                s.is_private = old->is_private;
                return s;
            }
        }
        if (!p.cwd.empty()) s.cwd = p.cwd;
        s.shell = p.shell;
        s.run = p.run;
        return s;
    }

    void finish(PaneId id, const std::optional<PaneId>& keep) {
        if (keep) {
            if (const Pane* old = ws.pane(*keep)) {
                if (old->read_only) (void)ws.set_pane_read_only(id, true);
                if (old->agent_resume) ws.set_agent_resume(id, old->agent_resume);
                res.adopted.emplace_back(*keep, id);
                ++res.kept;
                return;
            }
        }
        ++res.spawned;
    }

    // Leaves of `n` start at keep[next].
    static std::size_t leftmost(const PresetNode& n, std::size_t next, const PresetPane** pane) {
        const PresetNode* x = &n;
        while (!x->pane && x->first) x = x->first.get();
        static const PresetPane empty;
        *pane = x->pane ? &*x->pane : &empty;
        return next;
    }

    WsResult<void> build(const PresetNode& n, PaneId at, const std::vector<std::optional<PaneId>>& keep, std::size_t& next,
                         std::vector<PaneId>& order, int depth) {
        if (depth > kMaxDepthDecl) return ws_error(WsErrc::InvalidArgument, "the layout is nested too deep");
        if (n.pane || !n.first || !n.second) {
            finish(at, next < keep.size() ? keep[next] : std::nullopt);
            ++next;
            order.push_back(at);
            return {};
        }
        // The second side's first leaf index: after all leaves of the first side.
        const std::size_t second_first = next + n.first->pane_count();
        const PresetPane* p = nullptr;
        (void)leftmost(*n.second, second_first, &p);
        const Direction dir = n.axis == Axis::Horizontal ? Direction::Right : Direction::Down;
        auto second = ws.split_pane(at, dir, spec(*p, second_first < keep.size() ? keep[second_first] : std::nullopt), false);
        if (!second) return Unexpected<WsError>(second.error());
        (void)ws.set_split_ratio(at, n.axis, n.ratio);
        if (auto r = build(*n.first, at, keep, next, order, depth + 1); !r) return r;
        return build(*n.second, *second, keep, next, order, depth + 1);
    }
};

}  // namespace

WsResult<LayoutApplyResult> apply_layout_plan(Workspace& ws, ProjectId project, const LayoutPlan& plan, bool keep_sessions) {
    if (project == kQuickProject || !ws.workspace_of(project)) return ws_error(WsErrc::NotFound, "no such project");
    if (plan.tabs.empty()) return ws_error(WsErrc::LayoutNoTabs, "the layout declares no tabs");
    LayoutApplyResult res;
    PlanBuilder b{ws, res};
    std::vector<TabId> old;
    for (const Tab& t : ws.workspace_of(project)->tabs) old.push_back(t.id);

    Transaction tx = ws.begin();
    std::vector<TabId> made;
    for (const LayoutPlanTab& t : plan.tabs) {
        NewTabOptions o;
        o.focus = false;
        o.at = ws.workspace_of(project)->tabs.size();
        const PresetPane* first = nullptr;
        (void)PlanBuilder::leftmost(t.tree, 0, &first);
        auto id = ws.new_tab(project, b.spec(*first, t.keep.empty() ? std::nullopt : t.keep.front()), o);
        if (!id) return Unexpected<WsError>(id.error());
        const Tab* tb = ws.tab(*id);
        if (!tb) return ws_error(WsErrc::NotFound, "the new tab is gone");
        std::vector<PaneId> order;
        std::size_t next = 0;
        if (auto r = b.build(t.tree, tb->focused, t.keep, next, order, 0); !r) return Unexpected<WsError>(r.error());
        if (t.title && !t.title->empty()) (void)ws.rename_tab(*id, t.title);
        if (!order.empty()) (void)ws.focus_pane(order.front());
        made.push_back(*id);
        ++res.tabs;
    }
    // The old tabs go: unmatched panes end (or stay detached), kept ones leave without a kill
    // (their sessions now belong to the new panes).
    if (!keep_sessions) {
        for (PaneId p : plan.destroy) {
            if (ws.pane(p) && ws.close_pane(p, true)) ++res.closed;
        }
    } else {
        res.detached = plan.destroy.size();
    }
    for (TabId t : old) {
        if (ws.tab(t)) (void)ws.detach_tab(t);
    }
    if (!made.empty()) {
        (void)ws.set_active_tab(made.front());
        for (const auto& win : ws.windows()) {
            if (win.shown == project) (void)ws.select_tab(win.id, made.front());
        }
    }
    return res;
}

std::optional<ProjectFile> capture_project_file(const Workspace& ws, ProjectId project, std::string_view home,
                                                const LivePaneFactsFn& facts) {
    const Project* pr = ws.project(project);
    const ProjectWorkspace* w = ws.workspace_of(project);
    if (!pr || !w) return std::nullopt;
    auto pp = capture_project(ws, project, false, nullptr);
    if (!pp) return std::nullopt;
    ProjectFile f;
    f.name = pr->name;
    f.path = layout_path_of(pr->path, home);
    std::vector<PresetTab> tabs;
    for (std::size_t i = 0; i < pp->tabs.size() && i < w->tabs.size(); ++i) {
        PresetTab t = pp->tabs[i];
        const std::vector<PaneId> ids = w->tabs[i].tree.panes();
        std::size_t k = 0;
        std::function<void(PresetNode&)> walk = [&](PresetNode& n) {
            if (n.pane || !n.first || !n.second) {
                if (!n.pane) n.pane = PresetPane{};
                PresetPane& p = *n.pane;
                const Pane* live = k < ids.size() ? ws.pane(ids[k]) : nullptr;
                ++k;
                LivePaneFacts lf;
                if (live && facts) lf = facts(*live);
                const std::string folder = !lf.cwd.empty() ? lf.cwd : (live ? live->last_cwd : p.cwd);
                p.cwd = layout_cwd_of(folder, pr->path, home);
                p.run = lf.command;
                if (p.shell && pr->shell && p.shell->program == pr->shell->program) p.shell.reset();
                if (p.shell) p.shell->args.clear();
                p.agent.reset();
                return;
            }
            n.ratio = std::round(n.ratio * 100.0) / 100.0;
            walk(*n.first);
            walk(*n.second);
        };
        walk(t.layout);
        t.focused = 0;
        tabs.push_back(std::move(t));
    }
    f.tabs = std::move(tabs);
    return f;
}

}  // namespace kt::ws
