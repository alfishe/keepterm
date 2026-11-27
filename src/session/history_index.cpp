#include "session/history_index.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>

#include "platform/paths.h"
#include "session/archive.h"
#include "session/history_catchup.h"
#include "store/catalog.h"
#include "store/file_io.h"
#include "store/retention.h"
#include "store/search.h"

namespace kt::session {

namespace fs = std::filesystem;

std::string_view to_string(HistoryState s) {
    switch (s) {
        case HistoryState::running: return "running";
        case HistoryState::closed: return "closed";
    }
    return "?";
}

std::string HistoryEntry::project_name() const {
    return project.kind() == json::Value::Kind::Object ? project.str_or("name") : std::string();
}

namespace {

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 64 && id.find_first_of("/\\.:") == std::string_view::npos;
}

std::string history_root(const std::string& state) { return os::join(state, "history"); }
std::string series_dir(const std::string& state, const std::string& id) { return os::join(history_root(state), id); }

bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(platform::paths::from_utf8(path), ec);
}

// try_acquire, retried while another process holds it, at most `wait`.
Result<std::unique_ptr<store::LockFile>> lock_within(const std::string& path, std::chrono::milliseconds wait) {
    const auto end = std::chrono::steady_clock::now() + wait;
    for (;;) {
        auto l = store::LockFile::try_acquire(platform::paths::from_utf8(path));
        if (!l || *l || std::chrono::steady_clock::now() >= end) return l;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// True when a process holds the lock file (a host of the folder, a writer of the series).
bool held(const std::string& lock_path) {
    if (!exists(lock_path)) return false;
    auto l = store::LockFile::try_acquire(platform::paths::from_utf8(lock_path));
    return l && !*l;
}

std::vector<std::string> strings_of(const json::Value* a) {
    std::vector<std::string> out;
    if (!a || !a->as_array()) return out;
    for (const auto& x : *a->as_array()) {
        if (const auto* s = x.as_string()) out.push_back(*s);
    }
    return out;
}

json::Value strings_value(const std::vector<std::string>& v) {
    auto a = json::Value::array();
    for (const auto& s : v) a.push(json::Value::string(s));
    return a;
}

// A copy of `m` with `key` set (members keep their order).
json::Value with(const json::Value& m, const std::string& key, json::Value v) {
    json::Value out = m.kind() == json::Value::Kind::Object ? m : json::Value::object();
    out.set(key, std::move(v));
    return out;
}

// A series' figures from the catalog: stored and raw bytes per series.
struct Sizes {
    uint64_t stored = 0, raw = 0;
};
std::map<std::string, Sizes> sizes_by_series(store::Catalog& cat) {
    std::map<std::string, Sizes> out;
    if (auto rows = cat.segments()) {
        for (const auto& r : *rows) {
            auto& s = out[r.session];
            s.stored += r.info.file_bytes;
            if (r.info.kind == store::Kind::raw) s.raw += r.info.raw_bytes;
        }
    }
    return out;
}

}  // namespace

std::string expand_tilde(const std::string& path, const std::string& home) {
    if (path.empty() || path[0] != '~' || home.empty()) return path;
    if (path.size() == 1) return home;
    if (path[1] != '/' && path[1] != '\\') return path;
    return home + path.substr(1);
}

HistoryEntry entry_from_manifest(const json::Value& m) {
    HistoryEntry e;
    e.id = m.str_or("id");
    e.title = m.str_or("title");
    e.title_user = m.str_or("title_user");
    e.tab_title = m.str_or("tab_title");
    if (const auto* p = m.get("project"); p && p->kind() == json::Value::Kind::Object) e.project = *p;
    e.cwd_first = m.str_or("cwd_first");
    e.cwd_last = m.str_or("cwd_last");
    e.shell = m.str_or("shell");
    e.created_ms = m.uint_or("created_ms", 0);
    e.last_used_ms = m.uint_or("last_used_ms", 0);
    if (const auto* v = m.get("ended_ms"); v && v->as_uint()) e.ended_ms = *v->as_uint();
    if (const auto* v = m.get("exit"); v && v->as_int()) e.exit_code = static_cast<int>(*v->as_int());
    e.close_reason = m.str_or("close_reason");
    if (const auto* rs = m.get("restores"); rs && rs->as_array()) {
        for (const auto& r : *rs->as_array()) {
            if (const uint64_t t = r.uint_or("at_ms", 0)) e.restores.push_back(t);
        }
    }
    e.tags = strings_of(m.get("tags"));
    e.pinned = m.bool_or("pinned", false);
    if (const auto* as = m.get("agents"); as && as->as_array()) {
        for (const auto& a : *as->as_array()) {
            if (auto r = agent_run_from_json(a)) e.agents.push_back(std::move(*r));
        }
    }
    if (const auto* st = m.get("stats"); st && st->kind() == json::Value::Kind::Object) {
        e.stats.commands = st->uint_or("commands", 0);
        e.stats.failed_commands = st->uint_or("failed_commands", 0);
        e.stats.raw_bytes = st->uint_or("raw_bytes", 0);
        e.stats.text_lines = st->uint_or("text_lines", 0);
        e.stats.stored_bytes = st->uint_or("stored_bytes", 0);
    }
    return e;
}

Result<json::Value> read_manifest(const std::string& state, const std::string& id) {
    if (!valid_id(id)) return make_error(Errc::invalid_argument, "bad history id");
    const std::string path = os::join(series_dir(state, id), kManifestFile);
    if (!exists(path)) return make_error(Errc::invalid_argument, "no manifest for " + id);
    auto text = os::read_file(path, 1u << 20);
    if (!text) return Unexpected(text.error());
    auto v = json::parse(*text);
    if (!v) return Unexpected(v.error());
    if (v->kind() != json::Value::Kind::Object) return make_error(Errc::corrupt, "manifest is not an object");
    return std::move(*v);
}

Result<void> update_manifest(const std::string& state, const std::string& id,
                             const std::function<json::Value(json::Value)>& fn) {
    if (!valid_id(id)) return make_error(Errc::invalid_argument, "bad history id");
    const std::string dir = series_dir(state, id);
    if (auto r = platform::paths::make_private_dir(platform::paths::from_utf8(dir)); !r) return r;
    auto lock = lock_within(os::join(dir, kManifestLockFile), std::chrono::seconds(10));
    if (!lock) return Unexpected(lock.error());
    if (!*lock) return make_error(Errc::busy, "the manifest of " + id + " stays locked");
    json::Value cur = json::Value::object();
    if (auto m = read_manifest(state, id)) cur = std::move(*m);
    json::Value next = fn(std::move(cur));
    if (next.kind() != json::Value::Kind::Object) return make_error(Errc::invalid_argument, "manifest is not an object");
    if (!next.get("schema")) next.set("schema", json::Value::integer(1));
    if (!next.get("id")) next.set("id", json::Value::string(id));
    return os::write_file_atomic(os::join(dir, kManifestFile), next.dump(true));
}

std::vector<HistoryEntry> list_history(const os::Paths& p) {
    const std::string root = history_root(p.state);
    std::map<std::string, json::Value> manifests;
    for (const std::string& name : os::list_dir(root)) {
        if (!valid_id(name)) continue;
        const std::string dir = os::join(root, name);
        if (exists(os::join(dir, store::kManifestDeleting))) continue;  // being deleted
        if (auto m = read_manifest(p.state, name)) manifests[name] = std::move(*m);
    }

    // Session folders: running hosts and dead ones not archived yet. Several folders of one
    // series (a dead one a restore went past): the running one, else the newest.
    struct Folder {
        std::string name;
        RecoveryRecord rec;
        bool running = false;
    };
    std::map<std::string, Folder> folders;
    for (const std::string& name : os::list_dir(p.state_sessions)) {
        if (!SessionId::parse(name)) continue;
        const std::string dir = os::join(p.state_sessions, name);
        auto rec = read_recovery_record(dir);
        if (!rec || !rec->history || !rec->store) continue;
        Folder f{name, *rec, held(os::join(dir, kHostLockFile))};
        const std::string hid = rec->history_id.empty() ? name : rec->history_id;
        auto it = folders.find(hid);
        if (it == folders.end() || (f.running && !it->second.running) ||
            (f.running == it->second.running && f.rec.updated_unix_ms > it->second.rec.updated_unix_ms)) {
            folders[hid] = std::move(f);
        }
    }

    std::unique_ptr<store::Catalog> cat;
    std::map<std::string, Sizes> sizes;
    if (!folders.empty()) {
        if (auto c = store::Catalog::open(platform::paths::from_utf8(root) / store::kCatalogFile)) {
            cat = std::move(*c);
            sizes = sizes_by_series(*cat);
        }
    }

    std::vector<HistoryEntry> out;
    for (auto& [id, m] : manifests) {
        if (folders.count(id)) continue;
        HistoryEntry e = entry_from_manifest(m);
        e.id = id;
        e.final_screen = exists(os::join(os::join(root, id), kFinalSnapshotFile));
        out.push_back(std::move(e));
    }
    for (auto& [id, f] : folders) {
        // The record over the earlier manifest of the series, merged as the archive will.
        ManifestFacts facts;
        facts.record = f.rec;
        facts.history_id = id;
        facts.last_used_ms = f.rec.updated_unix_ms;
        if (!f.running) facts.ended_ms = f.rec.updated_unix_ms;
        const auto mit = manifests.find(id);
        const json::Value* prev = mit != manifests.end() ? &mit->second : nullptr;
        HistoryEntry e = entry_from_manifest(build_manifest(facts, prev));
        e.id = id;
        e.session = f.name;
        e.state = f.running ? HistoryState::running : HistoryState::closed;
        if (!f.running) e.close_reason = f.rec.exit_code ? "exited" : "killed";
        if (const auto s = sizes.find(id); s != sizes.end()) {
            e.stats.stored_bytes = s->second.stored;
            e.stats.raw_bytes = s->second.raw;
        }
        if (cat) {
            if (auto cmds = cat->commands(id)) {
                e.stats.commands = cmds->size();
                e.stats.failed_commands = static_cast<uint64_t>(std::count_if(
                    cmds->begin(), cmds->end(), [](const store::CommandRow& c) { return c.exit_code && *c.exit_code != 0; }));
            }
        }
        e.final_screen = exists(os::join(os::join(root, id), kFinalSnapshotFile));
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
        if (a.last_used_ms != b.last_used_ms) return a.last_used_ms > b.last_used_ms;
        return a.id < b.id;
    });
    return out;
}

Result<HistoryEntry> find_history(const os::Paths& p, std::string_view id) {
    if (id.empty()) return make_error(Errc::invalid_argument, "no history id");
    auto all = list_history(p);
    const HistoryEntry* hit = nullptr;
    for (const auto& e : all) {
        if (e.id == id) return e;
        if (e.id.compare(0, id.size(), id) == 0) {
            if (hit) return make_error(Errc::invalid_argument, "history id " + std::string(id) + " is ambiguous");
            hit = &e;
        }
    }
    if (!hit) return make_error(Errc::invalid_argument, "no session " + std::string(id) + " in the history");
    return *hit;
}

std::vector<HistoryCommand> history_commands(const os::Paths& p, const std::string& id) {
    std::vector<HistoryCommand> out;
    const fs::path db = platform::paths::from_utf8(history_root(p.state)) / store::kCatalogFile;
    if (std::error_code ec; !fs::exists(db, ec)) return out;   // the store never ran: no catalog to create
    auto cat = store::Catalog::open(db);
    if (!cat) return out;
    auto rows = (*cat)->commands(id);
    if (!rows) return out;
    for (const auto& r : *rows) out.push_back({r.started_ms, r.ended_ms, r.exit_code, r.cwd, r.command, r.seq});
    std::sort(out.begin(), out.end(), [](const HistoryCommand& a, const HistoryCommand& b) { return a.started_ms < b.started_ms; });
    return out;
}

std::vector<HistoryOutputHit> search_history_output(const os::Paths& p, const std::string& pattern, std::size_t limit,
                                                    const std::atomic<bool>* cancel) {
    std::vector<HistoryOutputHit> out;
    const fs::path root = platform::paths::from_utf8(history_root(p.state));
    if (pattern.empty() || limit == 0) return out;
    if (std::error_code ec; !fs::exists(root / store::kCatalogFile, ec)) return out;
    auto cat = store::Catalog::open(root / store::kCatalogFile);
    if (!cat) return out;
    store::SearchQuery q;
    q.pattern = pattern;
    q.limit = limit;
    q.max_text = 240;
    std::map<std::string, std::pair<std::string, std::string>> names;   // id -> title, project
    (void)store::search(**cat, root, q, [&](const store::Hit& h) {
        auto it = names.find(h.session);
        if (it == names.end()) {
            std::pair<std::string, std::string> n{h.cwd, h.project};
            if (auto m = read_manifest(p.state, h.session)) {
                const HistoryEntry e = entry_from_manifest(*m);
                std::string title = e.display_title();
                if (title.empty()) title = e.cwd_last;   // as the History scope names it
                n = {std::move(title), e.project_name().empty() ? h.project : e.project_name()};
            }
            it = names.emplace(h.session, std::move(n)).first;
        }
        out.push_back({h.session, it->second.first, it->second.second, h.text, h.ts_ms, h.match_offset, h.match_len, h.seq});
        return true;
    }, cancel);
    return out;
}

Result<void> set_history_title(const os::Paths& p, const std::string& id, std::string title) {
    return update_manifest(p.state, id, [&](json::Value m) { return with(m, "title_user", json::Value::string(title)); });
}

Result<void> tag_history(const os::Paths& p, const std::string& id, const std::vector<std::string>& add,
                         const std::vector<std::string>& remove) {
    return update_manifest(p.state, id, [&](json::Value m) {
        std::vector<std::string> tags = strings_of(m.get("tags"));
        for (const auto& t : add) {
            if (!t.empty() && std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
        }
        std::erase_if(tags, [&](const std::string& t) { return std::find(remove.begin(), remove.end(), t) != remove.end(); });
        return with(m, "tags", strings_value(tags));
    });
}

Result<void> pin_history(const os::Paths& p, const std::string& id, bool pinned) {
    if (auto r = update_manifest(p.state, id, [&](json::Value m) { return with(m, "pinned", json::Value::boolean(pinned)); });
        !r) {
        return r;
    }
    // Retention reads the pin from the catalog. A series that never stored anything has no row.
    const fs::path db = platform::paths::from_utf8(history_root(p.state)) / store::kCatalogFile;
    if (std::error_code ec; !fs::exists(db, ec)) return {};
    auto cat = store::Catalog::open(db);
    if (!cat) return Unexpected(cat.error());
    if (auto row = (*cat)->session(id); row && *row) return (*cat)->set_pinned(id, pinned);
    return {};
}

Result<uint64_t> delete_history(const os::Paths& p, const std::string& id) {
    if (!valid_id(id)) return make_error(Errc::invalid_argument, "bad history id");
    const std::string dir = series_dir(p.state, id);
    // No host may write the series, and none may run in a folder of it.
    std::vector<std::pair<std::string, std::unique_ptr<store::LockFile>>> dead;
    for (const std::string& name : os::list_dir(p.state_sessions)) {
        if (!SessionId::parse(name)) continue;
        const std::string sdir = os::join(p.state_sessions, name);
        auto rec = read_recovery_record(sdir);
        if (!rec || (rec->history_id.empty() ? name : rec->history_id) != id) continue;
        auto l = store::LockFile::try_acquire(platform::paths::from_utf8(os::join(sdir, kHostLockFile)));
        if (!l) return Unexpected(l.error());
        if (!*l) return make_error(Errc::busy, "session " + id + " is running; close it first");
        dead.emplace_back(sdir, std::move(*l));
    }
    if (exists(dir)) {
        auto l = store::LockFile::try_acquire(platform::paths::from_utf8(os::join(dir, kSeriesLockFile)));
        if (!l) return Unexpected(l.error());
        if (!*l) return make_error(Errc::busy, "session " + id + " is running; close it first");
        // Marked first: a crash from here on leaves a folder the janitor finishes.
        std::error_code ec;
        const fs::path d = platform::paths::from_utf8(dir);
        if (fs::exists(d / kManifestFile, ec)) fs::rename(d / kManifestFile, d / store::kManifestDeleting, ec);
        if (ec) return make_error(Errc::os_error, "cannot mark " + id + " deleted: " + ec.message());
        l->reset();  // Windows removes no folder with an open file
    } else if (dead.empty()) {
        return make_error(Errc::invalid_argument, "no session " + id + " in the history");
    }
    uint64_t bytes = 0;
    // No catalog: nothing was ever stored (the store off), only the folders go.
    const fs::path root = platform::paths::from_utf8(history_root(p.state));
    if (std::error_code ec; fs::exists(root / store::kCatalogFile, ec)) {
        auto cat = store::Catalog::open(root / store::kCatalogFile);
        if (!cat) return Unexpected(cat.error());
        auto f = store::forget_session(**cat, root, id);
        if (!f) return Unexpected(f.error());
        bytes += *f;
    }
    for (auto& [sdir, lock] : dead) {
        lock.reset();
        (void)os::remove_tree(sdir);
    }
    return bytes;
}

Result<RestorePlan> plan_restore(const HistoryEntry& e) {
    if (e.state == HistoryState::running) {
        return make_error(Errc::busy, "session " + e.id + " is running (session " + e.session + ")");
    }
    RestorePlan p;
    if (!e.session.empty()) {
        if (auto sid = SessionId::parse(e.session)) p.restore_from = *sid;
    } else {
        p.restore_history = e.id;
    }
    p.cwd = expand_tilde(e.cwd_last, os::home_dir());
    return p;
}

std::vector<std::string> RestorePlan::host_args(std::optional<std::size_t> resume_run) const {
    std::vector<std::string> a;
    if (restore_from) a = {std::string("--restore-from"), restore_from->str()};
    else if (!restore_history.empty()) a = {std::string("--restore-history"), restore_history};
    if (resume_run) {
        a.push_back("--resume-run");
        a.push_back(std::to_string(*resume_run));
    }
    return a;
}

}  // namespace kt::session
