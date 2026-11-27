#include "commands/palette.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <system_error>
#include <utility>

#include "base/utf8_range.h"
#include "platform/paths.h"

namespace kt::commands::palette {

// ---------------------------------------------------------------- usage and frecency

std::span<const std::int64_t> MemoryUsageStore::timestamps(std::string_view key) const {
    for (const auto& e : entries_)
        if (e.key == key) return e.t;
    return {};
}

std::uint64_t MemoryUsageStore::count(std::string_view key) const {
    for (const auto& e : entries_)
        if (e.key == key) return e.n;
    return 0;
}

void MemoryUsageStore::record(std::string_view key, std::int64_t now_ms) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.key == key; });
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxKeys) {
            // Evict the least recently used key.
            auto lru = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
                const std::int64_t ta = a.t.empty() ? 0 : a.t.front();
                const std::int64_t tb = b.t.empty() ? 0 : b.t.front();
                return ta < tb;
            });
            entries_.erase(lru);
        }
        entries_.push_back(Entry{std::string(key), 0, {}});
        it = entries_.end() - 1;
    }
    ++it->n;
    it->t.insert(it->t.begin(), now_ms);
    if (it->t.size() > kMaxStamps) it->t.resize(kMaxStamps);
}

double frecency_value(std::span<const std::int64_t> timestamps, std::int64_t now_ms) {
    constexpr std::int64_t h = 3600 * 1000;
    double f = 0;
    for (const std::int64_t t : timestamps) {
        const std::int64_t age = now_ms - t;
        if (age < 4 * h) f += 100;
        else if (age < 24 * h) f += 70;
        else if (age < 72 * h) f += 50;
        else if (age < 7 * 24 * h) f += 30;
        else if (age < 28 * 24 * h) f += 10;
    }
    return f;
}

int frecency_boost(std::span<const std::int64_t> timestamps, std::int64_t now_ms) {
    const double f = frecency_value(timestamps, now_ms);
    if (f <= 0) return 0;
    return std::min(32, static_cast<int>(std::floor(6.0 * std::log(1.0 + f / 10.0))));
}

// ---------------------------------------------------------------- file system

namespace {

std::filesystem::path fs_path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

class StdFileSystem final : public IFileSystem {
public:
    std::string home() const override {
        const auto u = platform::paths::home().u8string();
        return std::string(u.begin(), u.end());
    }
    bool is_dir(std::string_view path) const override {
        std::error_code ec;
        return std::filesystem::is_directory(fs_path(path), ec);
    }
    std::vector<std::string> list_dirs(std::string_view dir) const override {
        std::vector<std::string> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(fs_path(dir), ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            // symlink_status: a linked folder is not followed into another tree.
            if (!std::filesystem::is_directory(it->symlink_status(e2))) continue;
            const auto name = it->path().filename().u8string();
            out.emplace_back(name.begin(), name.end());
            if (out.size() >= 5000) break;
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<IFileSystem> make_std_file_system() { return std::make_unique<StdFileSystem>(); }

// ---------------------------------------------------------------- Query

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool looks_like_path(std::string_view s) {
    if (s.empty()) return false;
    if (s.front() == '~' || s.front() == '/') return true;
    if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\') return true;
    if (s.size() >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' &&
        (s[2] == '\\' || s[2] == '/'))
        return true;
    return false;
}

}  // namespace

Query Query::parse(std::string_view raw, PaletteScope scope) {
    Query q;
    q.raw = std::string(raw);
    q.scope = scope;
    std::string_view s = raw;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    if (scope == PaletteScope::Root && !s.empty()) {
        if (looks_like_path(s)) {
            q.mode = QueryMode::Path;
            q.text = std::string(s);   // path text is taken as typed, spaces included
            return q;
        }
        switch (s.front()) {
            case '>': q.mode = QueryMode::Commands; break;
            case '@': q.mode = QueryMode::Projects; break;
            case '#': q.mode = QueryMode::Tabs; break;
            case '!': q.mode = QueryMode::Sessions; break;
            default: break;
        }
        if (q.mode != QueryMode::All) s.remove_prefix(1);
    } else if (scope == PaletteScope::Folders && looks_like_path(s)) {
        q.mode = QueryMode::Path;
        q.text = std::string(s);
        return q;
    }
    q.text = std::string(trim(s));
    q.terms = SearchQuery::parse(q.text);
    return q;
}

// ---------------------------------------------------------------- sources

namespace {

ICommandTarget& fallback_target() {
    static ICommandTarget t;
    return t;
}

std::string tilde(std::string_view path, std::string_view home) {
    if (!home.empty() && path.size() >= home.size() && path.substr(0, home.size()) == home &&
        (path.size() == home.size() || path[home.size()] == '/' || path[home.size()] == '\\'))
        return "~" + std::string(path.substr(home.size()));
    return std::string(path);
}

std::string home_of(const SourceContext& c) { return c.fs ? c.fs->home() : std::string(); }

class CommandSource final : public Source {
public:
    std::string_view id() const override { return "commands"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        return s == PaletteScope::Root && (m == QueryMode::All || m == QueryMode::Commands);
    }
    void collect(const Query&, const SourceContext& c, std::vector<Item>& out) override { items(c, out); }
    void collect_empty(const SourceContext& c, std::vector<Item>& out) override { items(c, out); }

private:
    static void items(const SourceContext& c, std::vector<Item>& out) {
        if (!c.registry) return;
        ICommandTarget& target = c.target ? *c.target : fallback_target();
        CommandContext cctx{target, c.config, c.keys, InvokeSource::Palette, c.os, false};
        for (const Command& cmd : c.registry->all()) {
            if (!cmd.palette_visible) continue;
            Item it;
            it.key = "cmd:" + std::string(cmd.id);
            it.title = c.registry->title(cmd);
            if (it.title != cmd.title) it.aliases.emplace_back(cmd.title);
            it.category = std::string(category_title(cmd.category));
            it.icon = cmd.icon;
            if (c.keymap) it.keys = c.keymap->key_caps(cmd.id);
            const Availability av = c.registry->availability(cmd, cctx);
            it.enabled = av.enabled;
            it.disabled_reason = av.reason;
            it.subtitle = av.enabled ? it.category : av.reason;
            if (cmd.id == "pane.toggle_private") it.checked = c.state.pane_private;
            else if (cmd.id == "pane.read_only") it.checked = c.state.pane_read_only;
            else if (cmd.id == "view.toggle_sidebar") it.checked = c.state.sidebar_visible;
            else if (cmd.id == "view.toggle_fullscreen") it.checked = c.state.fullscreen;
            it.opens_scope = cmd.opens_scope;
            const CommandRegistry* reg = c.registry;
            ICommandTarget* tp = &target;
            const config::Config* cfg = c.config;
            const KeyContext keys = c.keys;
            const Os os = c.os;
            const CommandId id = cmd.id;
            // Scope commands are entered by the engine; the rest run through the registry.
            if (!cmd.opens_scope) {
                it.run = [reg, tp, cfg, keys, os, id] {
                    CommandContext x{*tp, cfg, keys, InvokeSource::Palette, os, false};
                    (void)reg->invoke(id, x);
                };
            }
            out.push_back(std::move(it));
        }
    }
};

class ProjectSource final : public Source {
public:
    std::string_view id() const override { return "projects"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        if (s == PaletteScope::MoveTabToProject) return m == QueryMode::All;
        return s == PaletteScope::Root && (m == QueryMode::All || m == QueryMode::Projects);
    }
    int boost() const override { return 8; }
    void collect(const Query& q, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data) return;
        for (const auto& p : c.data->projects) out.push_back(item(p, c, q.scope));
    }
    void collect_empty(const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data) return;
        std::vector<const ProjectInfo*> v;
        for (const auto& p : c.data->projects) v.push_back(&p);
        std::stable_sort(v.begin(), v.end(),
                         [](const ProjectInfo* a, const ProjectInfo* b) { return a->last_used_ms > b->last_used_ms; });
        for (const auto* p : v) out.push_back(item(*p, c, PaletteScope::Root));
    }

private:
    static Item item(const ProjectInfo& p, const SourceContext& c, PaletteScope scope) {
        Item it;
        it.key = "prj:" + p.id;
        it.title = p.name;
        it.subtitle = tilde(p.path, home_of(c));
        it.aliases.push_back(p.path);
        it.category = "Projects";
        it.icon = "project";
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        const std::string id = p.id;
        if (scope == PaletteScope::MoveTabToProject) {
            it.key = "mv:" + p.id;
            if (p.id == c.data->current_project_id) {
                it.enabled = false;
                it.disabled_reason = "The tab is already in this project";
                it.subtitle = it.disabled_reason;
            }
            it.run = [t, id] { (void)t->move_tab_to_project(id); };
            return it;
        }
        it.run = [t, id] { (void)t->open_project(id, false); };
        it.alt = AltAction{"Open in New Window", [t, id] { (void)t->open_project(id, true); }};
        return it;
    }
};

class TabSource final : public Source {
public:
    std::string_view id() const override { return "tabs"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        if (s == PaletteScope::Tabs) return m == QueryMode::All;
        return s == PaletteScope::Root && (m == QueryMode::All || m == QueryMode::Tabs);
    }
    int boost() const override { return 4; }
    void collect(const Query&, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        for (const auto& tab : c.data->tabs) {
            Item it;
            it.key = "tab:" + tab.id;
            it.title = tab.project_name.empty() ? tab.title : tab.project_name + " › " + tab.title;
            it.aliases.push_back(tab.title);
            std::string sub = tab.program;
            if (!tab.status.empty()) sub += (sub.empty() ? "" : " · ") + tab.status;
            if (tab.pinned) sub += (sub.empty() ? "" : " · ") + std::string("pinned");
            if (tab.scratch) sub += (sub.empty() ? "" : " · ") + std::string("scratch");
            it.subtitle = sub;
            it.category = "Tabs";
            it.icon = "tab";
            it.checked = tab.current;
            const std::string id = tab.id;
            it.run = [t, id] { (void)t->activate_tab(id); };
            if (!c.data->current_project_id.empty() && tab.project_id != c.data->current_project_id)
                it.alt = AltAction{"Move to Current Project", [t, id] { (void)t->move_tab_here(id); }};
            out.push_back(std::move(it));
        }
    }
};

class FolderSource final : public Source {
public:
    std::string_view id() const override { return "folders"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        return (s == PaletteScope::Root || s == PaletteScope::Folders) && m == QueryMode::All;
    }
    void collect(const Query&, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        const std::string home = home_of(c);
        std::vector<std::string_view> seen;
        for (const auto& f : c.data->folders) {
            if (f.path.empty() || std::find(seen.begin(), seen.end(), f.path) != seen.end()) continue;
            seen.push_back(f.path);
            if (seen.size() > 200) break;
            Item it;
            it.key = "dir:" + f.path;
            it.title = tilde(f.path, home);
            it.aliases.push_back(f.path);
            it.subtitle = "Recent Folder";
            it.category = "Recent Folders";
            it.icon = "folder";
            const std::string path = f.path;
            it.run = [t, path] { (void)t->open_folder(path, FolderAction::OpenAsProject); };
            it.alt = AltAction{"Open as Tab Here", [t, path] { (void)t->open_folder(path, FolderAction::OpenAsTab); }};
            out.push_back(std::move(it));
        }
    }
};

class SessionSource final : public Source {
public:
    std::string_view id() const override { return "sessions"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        if (s == PaletteScope::Sessions) return m == QueryMode::All;
        return s == PaletteScope::Root && (m == QueryMode::All || m == QueryMode::Sessions);
    }
    void collect(const Query&, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        for (const auto& s : c.data->sessions) {
            Item it;
            it.key = "ses:" + s.id;
            it.title = s.title.empty() ? s.program : s.title;
            it.aliases.push_back(s.program);
            std::string sub = s.program;
            switch (s.state) {
                case SessionInfo::State::Attached: break;
                case SessionInfo::State::Orphaned: sub += sub.empty() ? "orphaned" : " · orphaned"; break;
                case SessionInfo::State::Outdated: sub += sub.empty() ? "outdated host" : " · outdated host"; break;
            }
            if (s.memory_bytes > 0) {
                const double mb = static_cast<double>(s.memory_bytes) / (1024.0 * 1024.0);
                sub += (sub.empty() ? "" : " · ") + std::to_string(static_cast<long long>(std::lround(mb))) + " MB";
            }
            it.subtitle = sub;
            it.category = "Sessions";
            it.icon = "session";
            const std::string id = s.id;
            it.run = [t, id] { (void)t->attach_session(id); };
            it.alt = AltAction{"Kill Session", [t, id] { (void)t->kill_session(id); }};
            out.push_back(std::move(it));
        }
    }
};

// Named workspaces: Enter opens one (the dialog asks replace or merge).
class WorkspaceSource final : public Source {
public:
    std::string_view id() const override { return "workspaces"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        return (s == PaletteScope::Root || s == PaletteScope::Workspaces) && m == QueryMode::All;
    }
    void collect(const Query&, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        for (const auto& w : c.data->workspaces) {
            Item it;
            it.key = "wsp:" + w.name;
            it.title = w.name;
            it.subtitle = w.is_default ? (w.summary.empty() ? std::string("default") : w.summary + " \xC2\xB7 default") : w.summary;
            it.category = "Workspaces";
            it.icon = "workspace";
            const std::string name = w.name;
            it.run = [t, name] { (void)t->open_workspace_named(name); };
            out.push_back(std::move(it));
        }
    }
};

// Path mode (Macterm): directory completion of what was typed.
class PathSource final : public Source {
public:
    std::string_view id() const override { return "path"; }
    bool serves(PaletteScope s, QueryMode m) const override {
        return (s == PaletteScope::Root || s == PaletteScope::Folders) && m == QueryMode::Path;
    }
    bool query_dependent() const override { return true; }
    void collect(const Query& q, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.fs) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        const std::string home = c.fs->home();
        std::string typed = q.text;
        std::string full = typed;
        if (!typed.empty() && typed.front() == '~') full = home + typed.substr(1);
        const bool windows = c.os == Os::Windows;
        auto is_sep = [&](char ch) { return ch == '/' || (windows && ch == '\\'); };
        const char sep = windows && full.find('\\') != std::string::npos ? '\\' : '/';

        // The exact directory first.
        std::string exact = full;
        while (exact.size() > 1 && is_sep(exact.back()) && !(windows && exact.size() == 3 && exact[1] == ':'))
            exact.pop_back();
        if (c.fs->is_dir(exact)) {
            const ProjectInfo* used = nullptr;
            if (c.data)
                for (const auto& p : c.data->projects)
                    if (p.path == exact) used = &p;
            Item it;
            it.key = "dir:" + exact;
            it.title = used ? "Switch to Project “" + used->name + "”" : "Open as Project";
            it.subtitle = tilde(exact, home);
            it.category = "Folder";
            it.icon = "folder";
            if (used) {
                const std::string id = used->id;
                it.run = [t, id] { (void)t->open_project(id, false); };
            } else {
                it.run = [t, exact] { (void)t->open_folder(exact, FolderAction::OpenAsProject); };
            }
            it.alt = AltAction{"Open as Tab Here", [t, exact] { (void)t->open_folder(exact, FolderAction::OpenAsTab); }};
            out.push_back(std::move(it));
            if (used) {
                Item add;
                add.key = "dir+:" + exact;
                add.title = "Add Another Project Here";
                add.subtitle = tilde(exact, home);
                add.category = "Folder";
                add.icon = "project";
                add.run = [t, exact] { (void)t->open_folder(exact, FolderAction::AddProject); };
                out.push_back(std::move(add));
            }
        }

        // Children of the parent that start with the last component.
        std::size_t cut = full.size();
        while (cut > 0 && !is_sep(full[cut - 1])) --cut;
        std::string dir = full.substr(0, cut);
        const std::string prefix = full.substr(cut);
        std::string shown_dir = typed.substr(0, typed.size() - prefix.size());
        if (dir.empty()) return;
        std::string list_dir = dir;
        if (list_dir.size() > 1 && is_sep(list_dir.back()) && !(windows && list_dir.size() == 3 && list_dir[1] == ':'))
            list_dir.pop_back();
        std::vector<std::string> names = c.fs->list_dirs(list_dir);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            return fold(a) < fold(b);
        });
        std::size_t n = 0;
        for (const auto& name : names) {
            if (name.empty() || (name.front() == '.' && (prefix.empty() || prefix.front() != '.'))) continue;
            if (!folded_has_prefix(name, prefix)) continue;
            if (name.size() == prefix.size() && !exact.empty() && dir + name == exact) continue;   // already the top row
            const std::string path = dir + name;
            Item it;
            it.key = "dir:" + path;
            it.title = shown_dir + name + sep;
            it.subtitle = "Folder";
            it.category = "Folder";
            it.icon = "folder";
            it.run = [t, path] { (void)t->open_folder(path, FolderAction::OpenAsProject); };
            it.alt = AltAction{"Open as Tab Here", [t, path] { (void)t->open_folder(path, FolderAction::OpenAsTab); }};
            out.push_back(std::move(it));
            if (++n >= 50) break;
        }
    }
};

// The History scope: every session of the history, last used first. Enter is the main action
// (Show, Attach, Restore); Alt shows the session in the Session History window.
std::string ago(std::int64_t ms, std::int64_t now) {
    const std::int64_t sec = now > ms ? (now - ms) / 1000 : 0;
    if (sec < 60) return "now";
    if (sec < 3600) return std::to_string(sec / 60) + "m ago";
    if (sec < 48 * 3600) return std::to_string(sec / 3600) + "h ago";
    const std::int64_t d = sec / 86400;
    if (d < 14) return std::to_string(d) + "d ago";
    if (d < 60) return std::to_string(d / 7) + "w ago";
    return std::to_string(d / 30) + "mo ago";
}

// "webapp · Claude Code, Qoder CLI · #flaky · closed 2d ago": what a history row needs to be told
// apart from the others.
std::string history_context(const HistoryInfo& h, std::int64_t now) {
    std::vector<std::string> parts;
    if (!h.project.empty()) parts.push_back(h.project);
    const auto& agents = h.agent_names.empty() ? h.agents : h.agent_names;
    std::vector<std::string> seen;
    for (const auto& a : agents)
        if (std::find(seen.begin(), seen.end(), a) == seen.end()) seen.push_back(a);
    if (!seen.empty()) {
        std::string s;
        for (const auto& a : seen) s += (s.empty() ? "" : ", ") + a;
        parts.push_back(s);
    }
    if (!h.tags.empty()) {
        std::string s;
        for (const auto& t : h.tags) s += (s.empty() ? "#" : " #") + t;
        parts.push_back(s);
    }
    std::string state;
    switch (h.state) {
        case HistoryInfo::State::Live: state = "live"; break;
        case HistoryInfo::State::Background: state = "running, no tab"; break;
        case HistoryInfo::State::Closed: state = "closed " + ago(h.ended_ms ? h.ended_ms : h.last_used_ms, now); break;
    }
    if (h.state != HistoryInfo::State::Closed && h.last_used_ms) state += ", used " + ago(h.last_used_ms, now);
    if (h.pinned) state += ", pinned";
    parts.push_back(state);
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " \xC2\xB7 ") + p;
    return out;
}

std::string history_name(const HistoryInfo& h) { return h.title.empty() ? (h.cwd.empty() ? h.id : h.cwd) : h.title; }

// "14:05" in local time.
std::string clock_of(std::int64_t ms) {
    const auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

char fold_ascii(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

class HistorySource final : public Source {
public:
    std::string_view id() const override { return "history"; }
    bool serves(PaletteScope s, QueryMode m) const override { return s == PaletteScope::History && m == QueryMode::All; }
    void collect(const Query&, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data || !c.data->history) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        auto all = c.data->history();
        std::stable_sort(all.begin(), all.end(), [](const HistoryInfo& a, const HistoryInfo& b) { return a.last_used_ms > b.last_used_ms; });
        for (const auto& h : all) {
            Item it;
            it.key = "his:" + h.id;
            it.title = history_name(h);
            it.subtitle = history_context(h, c.now_ms);
            it.aliases.push_back(h.project);
            it.aliases.push_back(h.cwd);
            for (const auto& a : h.agents) it.aliases.push_back(a);
            for (const auto& a : h.agent_names) it.aliases.push_back(a);
            for (const auto& g : h.tags) it.aliases.push_back(g);
            it.category = "History";
            it.icon = h.agents.empty() ? "terminal" : "agent";
            const std::string id = h.id;
            it.run = [t, id] { (void)t->open_history_session(id); };
            it.alt = AltAction{"Show in Session History", [t, id] { (void)t->reveal_in_history(id); }};
            out.push_back(std::move(it));
        }
    }
};

// The History scope's "In output" section: lines of stored output with the typed text, grouped by
// session (its newest lines first, at most kPerSession, then a "+N more" row). Each row names its
// session as the History rows do; the line is a second line with the match marked. The search runs
// off the UI thread; until it answers, one disabled row says so.
class HistoryOutputSource final : public Source {
public:
    static constexpr std::size_t kMinQuery = 3, kPerSession = 3;
    std::string_view id() const override { return "history_output"; }
    bool serves(PaletteScope s, QueryMode m) const override { return s == PaletteScope::History && m == QueryMode::All; }
    bool query_dependent() const override { return true; }
    void collect(const Query& q, const SourceContext& c, std::vector<Item>& out) override {
        if (!c.data || !c.data->history_search || q.text.size() < kMinQuery) return;
        ICommandTarget* t = c.target ? c.target : &fallback_target();
        const std::vector<OutputHit>* hits = c.data->history_search->hits(q.text);
        if (!hits) {
            Item it;
            it.key = "out:searching";
            it.category = "In output";
            it.title = "Searching the stored output\xE2\x80\xA6";
            it.enabled = false;
            it.icon = "search";
            out.push_back(std::move(it));
            return;
        }
        std::map<std::string, HistoryInfo> infos;
        if (c.data->history)
            for (auto& h : c.data->history()) infos.emplace(h.id, std::move(h));
        // Sessions in the order of their newest hit.
        std::vector<std::string> order;
        std::map<std::string, std::vector<const OutputHit*>> by;
        for (const auto& h : *hits) {
            auto& v = by[h.history_id];
            if (v.empty()) order.push_back(h.history_id);
            v.push_back(&h);
        }
        std::string needle;
        for (char ch : q.text) needle += fold_ascii(ch);
        while (!needle.empty() && needle.back() == ' ') needle.pop_back();
        for (const auto& id : order) {
            const auto& lines = by[id];
            HistoryInfo info;
            if (auto f = infos.find(id); f != infos.end()) info = f->second;
            else {
                info.id = id;
                info.title = lines.front()->title;
                info.project = lines.front()->project;
            }
            const std::string name = history_name(info), context = history_context(info, c.now_ms);
            const std::string_view icon = info.agents.empty() ? "terminal" : "agent";
            for (std::size_t i = 0; i < lines.size() && i < kPerSession; ++i) {
                const OutputHit& h = *lines[i];
                Item it;
                it.key = "out:" + id + ":" + std::to_string(h.ts_ms) + ":" + std::to_string(i);
                it.title = name;
                it.subtitle = context;
                // The line: its time, then the text without its indent, the match marked.
                std::string text = h.text;
                std::size_t cut = text.find_first_not_of(" \t");
                if (cut == std::string::npos) cut = text.size();
                text = text.substr(cut);
                const std::string prefix = clock_of(h.ts_ms) + "  ";
                it.detail = prefix + text;
                std::string folded;
                for (char ch : text) folded += fold_ascii(ch);
                for (std::size_t at = needle.empty() ? std::string::npos : folded.find(needle); at != std::string::npos;
                     at = folded.find(needle, at + needle.size()))
                    it.detail_marks.emplace_back(prefix.size() + at, needle.size());
                if (it.detail_marks.empty() && h.match_offset >= cut)
                    if (auto r = utf8_whole_range(text, h.match_offset - cut, h.match_len))
                        it.detail_marks.emplace_back(prefix.size() + r->first, r->second);
                it.category = "In output";
                it.icon = icon;
                const std::string query = q.text;
                it.run = [t, id, query, line = HistoryLine{h.seq, h.text}] { (void)t->open_history_output(id, query, line); };
                it.alt = AltAction{"Show in Session History", [t, id] { (void)t->reveal_in_history(id); }};
                out.push_back(std::move(it));
            }
            if (lines.size() > kPerSession) {
                Item more;
                more.key = "out:" + id + ":more";
                const std::size_t n = lines.size() - kPerSession;
                more.title = "+" + std::to_string(n) + (n == 1 ? " more line in " : " more lines in ") + name;
                more.subtitle = "Session History";
                more.category = "In output";
                more.icon = "history";
                more.run = [t, id] { (void)t->reveal_in_history(id); };
                out.push_back(std::move(more));
            }
        }
    }
};

}  // namespace

std::unique_ptr<Source> make_command_source() { return std::make_unique<CommandSource>(); }
std::unique_ptr<Source> make_project_source() { return std::make_unique<ProjectSource>(); }
std::unique_ptr<Source> make_tab_source() { return std::make_unique<TabSource>(); }
std::unique_ptr<Source> make_folder_source() { return std::make_unique<FolderSource>(); }
std::unique_ptr<Source> make_session_source() { return std::make_unique<SessionSource>(); }
std::unique_ptr<Source> make_workspace_source() { return std::make_unique<WorkspaceSource>(); }
std::unique_ptr<Source> make_path_source() { return std::make_unique<PathSource>(); }
std::unique_ptr<Source> make_history_source() { return std::make_unique<HistorySource>(); }
std::unique_ptr<Source> make_history_output_source() { return std::make_unique<HistoryOutputSource>(); }

std::vector<std::unique_ptr<Source>> make_builtin_sources(std::span<const std::string> enabled) {
    std::vector<std::unique_ptr<Source>> all;
    all.push_back(make_command_source());
    all.push_back(make_project_source());
    all.push_back(make_tab_source());
    all.push_back(make_folder_source());
    all.push_back(make_session_source());
    all.push_back(make_workspace_source());
    all.push_back(make_path_source());
    all.push_back(make_history_source());
    all.push_back(make_history_output_source());
    if (enabled.empty()) return all;
    // Path mode and the History scope answer only when asked for: palette.sources does not hide them.
    std::vector<std::unique_ptr<Source>> out;
    for (auto& s : all)
        if (std::find(enabled.begin(), enabled.end(), s->id()) != enabled.end() || s->id() == "path" || s->id() == "history" ||
            s->id() == "history_output")
            out.push_back(std::move(s));
    return out;
}

// ---------------------------------------------------------------- Result

const Row* Result::first_enabled() const {
    for (const auto& s : sections)
        for (const auto& r : s.rows)
            if (r.item && r.item->enabled) return &r;
    return nullptr;
}

std::size_t Result::row_count() const {
    std::size_t n = 0;
    for (const auto& s : sections) n += s.rows.size();
    return n;
}

// ---------------------------------------------------------------- Engine

Engine::Engine(std::vector<std::unique_ptr<Source>> sources, IUsageStore* usage, EngineOptions o)
    : sources_(std::move(sources)), usage_(usage), opts_(o) {}

void Engine::open(const SourceContext& c) {
    ctx_ = c;
    scopes_.assign(1, PaletteScope::Root);
    invalidate();
}

void Engine::invalidate() {
    for (auto& row : caches_)
        for (auto& p : row) p = Prepared{};
    path_items_.clear();
    empty_items_.clear();
    result_ = Result{};
    last_query_.reset();
    last_matches_.clear();
}

void Engine::push_scope(PaletteScope s) {
    scopes_.push_back(s);
    last_query_.reset();
    last_matches_.clear();
}

bool Engine::pop_scope() {
    if (scopes_.size() <= 1) return false;
    scopes_.pop_back();
    last_query_.reset();
    last_matches_.clear();
    return true;
}

Engine::Prepared& Engine::cache_for(const Query& q) {
    Prepared& p = caches_[static_cast<std::size_t>(q.scope)][static_cast<std::size_t>(q.mode)];
    if (!p.ready) collect_into(p, q);
    return p;
}

void Engine::collect_into(Prepared& p, const Query& q) {
    p = Prepared{};
    for (std::size_t s = 0; s < sources_.size(); ++s) {
        Source& src = *sources_[s];
        if (src.query_dependent() || !src.serves(q.scope, q.mode)) continue;
        src.collect(q, ctx_, p.items);
        p.source.resize(p.items.size(), static_cast<std::uint16_t>(s));
    }
    p.fields.reserve(p.items.size());
    for (const auto& it : p.items) {
        std::vector<SearchText> f;
        f.reserve(it.aliases.size() + 2);
        f.push_back(prepare(it.title));
        for (const auto& a : it.aliases) f.push_back(prepare(a));
        if (!it.subtitle.empty()) f.push_back(prepare(it.subtitle));
        p.fields.push_back(std::move(f));
    }
    p.ready = true;
}

int Engine::usage_boost(const Item& it) const {
    if (!opts_.frecency || !usage_) return 0;
    return frecency_boost(usage_->timestamps(it.key), ctx_.now_ms);
}

void Engine::empty_state(const Query& q) {
    Prepared& all = cache_for(q);
    // Recent: the most used items across all sources.
    if (usage_ && opts_.frecency && opts_.recent_items > 0) {
        std::vector<std::pair<double, std::uint32_t>> scored;
        for (std::uint32_t i = 0; i < all.items.size(); ++i) {
            const double f = frecency_value(usage_->timestamps(all.items[i].key), ctx_.now_ms);
            if (f > 0) scored.emplace_back(f, i);
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        if (!scored.empty()) {
            Section s;
            s.header = "Recent";
            for (std::size_t k = 0; k < scored.size() && k < opts_.recent_items; ++k)
                s.rows.push_back(Row{&all.items[scored[k].second], 0, {}, all.source[scored[k].second]});
            result_.sections.push_back(std::move(s));
        }
    }
    // Projects by recency, then commands by category in registry order.
    std::vector<std::uint16_t> source_of;
    for (const bool projects_pass : {true, false}) {
        for (std::size_t s = 0; s < sources_.size(); ++s) {
            Source& src = *sources_[s];
            if (src.query_dependent() || !src.serves(q.scope, q.mode)) continue;
            if ((src.id() == "projects") != projects_pass) continue;
            const std::size_t before = empty_items_.size();
            src.collect_empty(ctx_, empty_items_);
            if (projects_pass && empty_items_.size() - before > opts_.recent_projects)
                empty_items_.resize(before + opts_.recent_projects);
            source_of.resize(empty_items_.size(), static_cast<std::uint16_t>(s));
        }
    }
    for (std::size_t i = 0; i < empty_items_.size(); ++i) {
        const Item& it = empty_items_[i];
        if (result_.sections.empty() || result_.sections.back().header != it.category)
            result_.sections.push_back(Section{it.category, {}});
        result_.sections.back().rows.push_back(Row{&it, 0, {}, source_of[i]});
    }
    for (const auto& s : result_.sections) result_.matched += s.rows.size();
}

const Result& Engine::search(std::string_view raw, std::size_t limit) {
    Query q = Query::parse(raw, scope());
    result_ = Result{};
    result_.scope = q.scope;
    result_.mode = q.mode;
    empty_items_.clear();

    if (q.mode == QueryMode::Path) {
        path_items_.clear();
        std::vector<std::uint16_t> src_of;
        for (std::size_t s = 0; s < sources_.size(); ++s) {
            if (!sources_[s]->query_dependent() || !sources_[s]->serves(q.scope, q.mode)) continue;
            sources_[s]->collect(q, ctx_, path_items_);
            src_of.resize(path_items_.size(), static_cast<std::uint16_t>(s));
        }
        Section sec;
        for (std::size_t i = 0; i < path_items_.size() && (limit == 0 || i < limit); ++i)
            sec.rows.push_back(Row{&path_items_[i], 0, {}, src_of[i]});
        result_.matched = path_items_.size();
        if (!sec.rows.empty()) result_.sections.push_back(std::move(sec));
        last_query_.reset();
        return result_;
    }

    if (q.empty()) {
        if (q.scope == PaletteScope::Root && q.mode == QueryMode::All) {
            empty_state(q);
        } else {
            Prepared& p = cache_for(q);
            Section sec;
            for (std::uint32_t i = 0; i < p.items.size() && (limit == 0 || i < limit); ++i)
                sec.rows.push_back(Row{&p.items[i], 0, {}, p.source[i]});
            result_.matched = p.items.size();
            if (!sec.rows.empty()) result_.sections.push_back(std::move(sec));
        }
        // An empty query matches everything: the next keystroke searches the whole cache.
        last_query_.reset();
        last_matches_.clear();
        return result_;
    }

    Prepared& p = cache_for(q);
    const bool narrowed = last_query_ && last_query_->scope == q.scope && last_query_->mode == q.mode &&
                          q.terms.narrows(last_query_->terms);
    std::vector<Match> matches;
    auto consider = [&](std::uint32_t i) {
        const auto s = score_fields(q.terms, std::span<const SearchText>(p.fields[i]), scratch_);
        if (!s) return;
        const int boost = sources_[p.source[i]]->boost() + usage_boost(p.items[i]);
        matches.push_back({i, *s + boost});
    };
    if (narrowed) {
        for (const std::uint32_t i : last_matches_) consider(i);
    } else {
        for (std::uint32_t i = 0; i < p.items.size(); ++i) consider(i);
    }
    last_matches_.clear();
    for (const auto& m : matches) last_matches_.push_back(m.index);
    std::sort(last_matches_.begin(), last_matches_.end());

    auto better = [&](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        const std::uint32_t la = p.fields[a.index][0].length, lb = p.fields[b.index][0].length;
        if (la != lb) return la < lb;
        if (p.source[a.index] != p.source[b.index]) return p.source[a.index] < p.source[b.index];
        return a.index < b.index;
    };
    result_.matched = matches.size();
    result_.narrowed = narrowed;
    if (limit != 0 && matches.size() > limit) {
        std::partial_sort(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(limit), matches.end(), better);
        matches.resize(limit);
    } else {
        std::sort(matches.begin(), matches.end(), better);
    }
    Section sec;
    sec.rows.reserve(matches.size());
    for (std::size_t k = 0; k < matches.size(); ++k) {
        Row r{&p.items[matches[k].index], matches[k].score, {}, p.source[matches[k].index]};
        if (k < opts_.visible) r.highlights = highlights(q.terms, p.fields[matches[k].index][0]);
        sec.rows.push_back(std::move(r));
    }
    if (!sec.rows.empty()) result_.sections.push_back(std::move(sec));
    // Sources that answer per query (the History scope's output search) follow as their own
    // section, in their own order.
    path_items_.clear();
    std::vector<std::uint16_t> src_of;
    for (std::size_t s = 0; s < sources_.size(); ++s) {
        if (!sources_[s]->query_dependent() || !sources_[s]->serves(q.scope, q.mode)) continue;
        sources_[s]->collect(q, ctx_, path_items_);
        src_of.resize(path_items_.size(), static_cast<std::uint16_t>(s));
    }
    if (!path_items_.empty()) {
        Section extra;
        extra.header = path_items_.front().category;
        for (std::size_t i = 0; i < path_items_.size(); ++i) extra.rows.push_back(Row{&path_items_[i], 0, {}, src_of[i]});
        result_.matched += path_items_.size();
        result_.sections.push_back(std::move(extra));
    }
    last_query_ = std::move(q);
    return result_;
}

bool Engine::run(const Row& r, bool alt) {
    if (!r.item || !r.item->enabled) return false;
    const Item& it = *r.item;
    if (alt && it.alt && it.alt->run) {
        if (usage_) usage_->record(it.key, ctx_.now_ms);
        it.alt->run();
        return true;
    }
    if (it.opens_scope && !it.run) {
        if (usage_) usage_->record(it.key, ctx_.now_ms);
        push_scope(*it.opens_scope);
        return true;
    }
    if (!it.run) return false;
    if (usage_) usage_->record(it.key, ctx_.now_ms);
    it.run();
    return true;
}

bool Engine::enter(const Row& r) {
    if (!r.item || !r.item->enabled || !r.item->opens_scope) return false;
    push_scope(*r.item->opens_scope);
    return true;
}

}  // namespace kt::commands::palette
