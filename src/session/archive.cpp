#include "session/archive.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "base/fault.h"
#include "base/log.h"
#include "platform/paths.h"
#include "session/history_catchup.h"
#include "session/history_index.h"
#include "store/catalog.h"
#include "store/file_io.h"
#include "store/session_store.h"

namespace kt::session {

std::string_view to_string(ArchiveOutcome o) {
    switch (o) {
        case ArchiveOutcome::archived: return "archived";
        case ArchiveOutcome::removed: return "removed";
        case ArchiveOutcome::busy: return "busy";
    }
    return "?";
}

std::string tilde_path(const std::string& path, const std::string& home) {
    if (home.empty() || path.size() < home.size() || path.compare(0, home.size(), home) != 0) return path;
    if (path.size() == home.size()) return "~";
    const char next = path[home.size()];
    if (next != '/' && next != '\\') return path;
    return "~" + path.substr(home.size());
}

namespace {

json::Value u64(uint64_t v) { return json::Value::integer(static_cast<int64_t>(v)); }

}  // namespace

json::Value build_manifest(const ManifestFacts& f, const json::Value* prev) {
    const std::string home = os::home_dir();
    const RecoveryRecord& r = f.record;
    const bool have_prev = prev && prev->kind() == json::Value::Kind::Object;
    auto v = json::Value::object();
    v.set("schema", json::Value::integer(1));
    v.set("id", json::Value::string(f.history_id));
    v.set("title", json::Value::string(r.title));
    if (!r.tab_title.empty()) v.set("tab_title", json::Value::string(r.tab_title));
    else if (have_prev && prev->get("tab_title")) v.set("tab_title", *prev->get("tab_title"));
    if (have_prev && prev->get("title_user")) v.set("title_user", *prev->get("title_user"));
    v.set("project", r.project.kind() == json::Value::Kind::Object ? r.project
                     : have_prev && prev->get("project") ? *prev->get("project")
                                                         : json::Value::null());
    const std::string cwd = tilde_path(r.cwd, home);
    v.set("cwd_first", json::Value::string(have_prev && !prev->str_or("cwd_first").empty() ? prev->str_or("cwd_first") : cwd));
    v.set("cwd_last", json::Value::string(cwd));
    v.set("shell", json::Value::string(r.shell));
    v.set("host", json::Value::string("local"));
    uint64_t created = r.created_unix_ms;
    // The series began with its first session: an earlier manifest or the first restore.
    if (have_prev && prev->uint_or("created_ms", 0)) created = std::min(created, prev->uint_or("created_ms", 0));
    v.set("created_ms", u64(created));
    v.set("last_used_ms", u64(std::max(f.last_used_ms, have_prev ? prev->uint_or("last_used_ms", 0) : 0)));
    v.set("ended_ms", f.ended_ms ? u64(f.ended_ms) : json::Value::null());
    v.set("exit", r.exit_code ? json::Value::integer(*r.exit_code) : json::Value::null());
    v.set("close_reason", f.close_reason.empty() ? json::Value::null() : json::Value::string(f.close_reason));

    // Restores: the earlier manifest's and the record's, without duplicates.
    std::vector<uint64_t> restores = r.restores;
    if (have_prev) {
        if (const auto* rs = prev->get("restores"); rs && rs->as_array()) {
            for (const auto& x : *rs->as_array()) restores.push_back(x.uint_or("at_ms", 0));
        }
    }
    std::sort(restores.begin(), restores.end());
    restores.erase(std::unique(restores.begin(), restores.end()), restores.end());
    auto rs = json::Value::array();
    for (uint64_t t : restores) {
        if (!t) continue;
        auto e = json::Value::object();
        e.set("at_ms", u64(t));
        e.set("reason", json::Value::string("restart"));
        rs.push(std::move(e));
    }
    v.set("restores", std::move(rs));
    v.set("tags", have_prev && prev->get("tags") ? *prev->get("tags") : json::Value::array());
    v.set("pinned", json::Value::boolean(have_prev && prev->bool_or("pinned", false)));

    // Agent runs: the earlier manifest's, then the record's that it lacks; open runs end with
    // the session.
    std::vector<AgentRun> runs;
    if (have_prev) {
        if (const auto* as = prev->get("agents"); as && as->as_array()) {
            for (const auto& x : *as->as_array()) {
                if (auto a = agent_run_from_json(x)) runs.push_back(std::move(*a));
            }
        }
    }
    for (AgentRun a : r.agent_runs) {
        a.cwd = tilde_path(a.cwd, home);
        if (!a.ended_ms && f.ended_ms) a.ended_ms = f.ended_ms;
        auto same = std::find_if(runs.begin(), runs.end(), [&](const AgentRun& b) {
            return b.agent == a.agent && b.conversation == a.conversation && b.started_ms == a.started_ms;
        });
        if (same == runs.end()) runs.push_back(std::move(a));
        else *same = std::move(a);
    }
    auto as = json::Value::array();
    for (const auto& a : runs) as.push(to_json(a));
    v.set("agents", std::move(as));

    auto st = json::Value::object();
    st.set("commands", u64(f.commands));
    st.set("failed_commands", u64(f.failed_commands));
    st.set("raw_bytes", u64(f.raw_bytes));
    st.set("text_lines", u64(f.text_lines));
    st.set("stored_bytes", u64(f.stored_bytes));
    v.set("stats", std::move(st));
    v.set("private_ranges", json::Value::integer(0));
    return v;
}

namespace {

Result<void> write_atomic(const std::string& path, std::string_view data) { return os::write_file_atomic(path, data); }

// The figures of a series for its manifest.
void series_figures(store::Catalog& cat, const std::string& root, const std::string& id, ManifestFacts& f) {
    store::SegmentFilter sf;
    sf.sessions = {id};
    if (auto rows = cat.segments(sf)) {
        for (const auto& row : *rows) {
            f.stored_bytes += row.info.file_bytes;
            if (row.info.kind == store::Kind::raw) f.raw_bytes += row.info.raw_bytes;
            if (row.info.kind == store::Kind::text) {
                auto rd = store::SegmentReader::open(platform::paths::from_utf8(root) / platform::paths::from_utf8(row.file));
                if (!rd) continue;
                for (const auto& b : (*rd)->blocks()) f.text_lines += b.records;
            }
        }
    }
    if (auto cmds = cat.commands(id)) {
        f.commands = cmds->size();
        for (const auto& c : *cmds) {
            if (c.exit_code && *c.exit_code != 0) ++f.failed_commands;
        }
    }
}

}  // namespace

namespace {

// try_acquire, retried while busy for at most `wait`.
Result<std::unique_ptr<store::LockFile>> acquire(const std::filesystem::path& p, std::chrono::milliseconds wait) {
    const auto end = std::chrono::steady_clock::now() + wait;
    for (;;) {
        auto l = store::LockFile::try_acquire(p);
        if (!l || *l || std::chrono::steady_clock::now() >= end) return l;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace

Result<ArchiveResult> archive_session(const std::string& dir, const ArchiveOptions& o) {
    ArchiveResult out;
    const std::string name = std::filesystem::path(platform::paths::from_utf8(dir)).filename().string();
    if (!os::exists(dir)) return out;  // removed already
    std::unique_ptr<store::LockFile> host_lock;
    if (!o.locks_held) {
        auto l = acquire(platform::paths::from_utf8(os::join(dir, kHostLockFile)), o.busy_wait);
        if (!l) return Unexpected(l.error());
        if (!*l) {
            out.outcome = ArchiveOutcome::busy;
            return out;
        }
        host_lock = std::move(*l);
    }
    auto remove_folder = [&] {
        if (o.before_remove) o.before_remove();
        host_lock.reset();  // Windows removes no folder with an open file
        if (KT_FAULT_POINT("archive.remove")) return Result<void>(make_error(Errc::os_error, "injected fault"));
        return os::remove_tree(dir);
    };
    if (!has_session_files(dir)) {
        // A crash during an earlier removal, or a folder of a host that died at once.
        if (auto r = remove_folder(); !r) return Unexpected(r.error());
        return out;
    }
    auto rec = read_recovery_record(dir);
    RecoveryRecord record;
    if (rec) {
        record = *rec;
    } else if (auto id = SessionId::parse(name)) {
        // The host died before its first record: the journal is all there is.
        record.id = *id;
        record.store = true;
    }
    if (!record.history || !record.store) {
        if (auto r = remove_folder(); !r) return Unexpected(r.error());
        return out;
    }
    out.history_id = record.history_id.empty() ? record.id.str() : record.history_id;

    const std::string root_s = os::join(o.state, "history");
    const auto root = platform::paths::from_utf8(root_s);
    const auto series_dir = root / platform::paths::from_utf8(out.history_id);
    if (auto r = platform::paths::make_private_dir(series_dir); !r) return Unexpected(r.error());
    std::unique_ptr<store::LockFile> series_lock;
    if (!o.locks_held) {
        auto l = acquire(series_dir / kSeriesLockFile, o.busy_wait);
        if (!l) return Unexpected(l.error());
        if (!*l) {
            out.outcome = ArchiveOutcome::busy;
            return out;
        }
        series_lock = std::move(*l);
    }
    if (KT_FAULT_POINT("archive.locked")) return make_error(Errc::os_error, "injected fault");

    auto cat = store::Catalog::open(root / store::kCatalogFile);
    if (!cat) return Unexpected(cat.error());
    store::SessionRow row;
    row.id = out.history_id;
    row.cwd = record.cwd;
    row.shell = record.shell;
    row.started_ms = record.created_unix_ms;
    auto st = store::SessionStore::open(root, **cat, row);
    if (!st) return Unexpected(st.error());
    CatchUpOptions co;
    co.term = o.term;
    co.finish_screen = true;
    co.cwd = record.cwd;
    auto stored_end = (*st)->end_seq(store::Kind::raw);
    if (!stored_end) return Unexpected(stored_end.error());
    HistoryCatchUp cu(**st, **cat, co);
    auto r = cu.run(dir);
    if (!r) return Unexpected(r.error());
    out.raw_bytes = cu.stats().raw_bytes;
    out.lines = cu.stats().lines;
    out.commands = cu.stats().commands;
    out.text_misaligned = cu.stats().text_misaligned;
    out.raw_gap = cu.stats().raw_gap;
    if (KT_FAULT_POINT("archive.caught_up")) return make_error(Errc::os_error, "injected fault");
    const uint64_t ended = std::max<uint64_t>({r->last_unix_ms, record.updated_unix_ms, record.created_unix_ms});
    // A folder the series went past (a restore that died before it removed its source, or one
    // whose successor took over): the store had all of it, and its screen is not the series' last.
    if (*stored_end > r->term->stream_offset()) {
        if (auto c = (*st)->flush(true); !c) return Unexpected(c.error());
        st->reset();
        if (auto rm = remove_folder(); !rm) return Unexpected(rm.error());
        out.outcome = ArchiveOutcome::archived;
        log::info("history", "archived " + name + ": superseded in " + out.history_id + ", " +
                                 std::to_string(out.raw_bytes) + " raw bytes caught up");
        return out;
    }
    if (auto c = (*st)->close(ended); !c) return Unexpected(c.error());
    st->reset();
    if (KT_FAULT_POINT("archive.closed")) return make_error(Errc::os_error, "injected fault");

    // The final screen and scrollback.
    const auto snap = r->term->snapshot();
    if (auto w = write_atomic(platform::paths::to_utf8(series_dir / kFinalSnapshotFile),
                              std::string_view(reinterpret_cast<const char*>(snap.data()), snap.size()));
        !w) {
        return Unexpected(w.error());
    }

    if (KT_FAULT_POINT("archive.final_screen")) return make_error(Errc::os_error, "injected fault");

    // The manifest, merged into an earlier one of the series.
    ManifestFacts f;
    f.record = record;
    f.history_id = out.history_id;
    f.last_used_ms = ended;
    f.ended_ms = ended;
    f.close_reason = o.close_reason;
    series_figures(**cat, root_s, out.history_id, f);
    // Under the manifest lock: the user's edits (title, tags, pin) may write it meanwhile.
    if (auto w = update_manifest(o.state, out.history_id,
                                 [&](json::Value prev) { return build_manifest(f, prev.members().empty() ? nullptr : &prev); });
        !w) {
        return Unexpected(w.error());
    }
    if (KT_FAULT_POINT("archive.manifest")) return make_error(Errc::os_error, "injected fault");

    if (auto rm = remove_folder(); !rm) return Unexpected(rm.error());
    out.outcome = ArchiveOutcome::archived;
    log::info("history", "archived " + name + " into " + out.history_id + ": " + std::to_string(out.raw_bytes) +
                             " raw bytes, " + std::to_string(out.lines) + " lines, " + std::to_string(out.commands) +
                             " commands caught up" + (out.raw_gap ? ", raw gap " + std::to_string(out.raw_gap) : std::string()) +
                             (out.text_misaligned ? ", text misaligned" : ""));
    return out;
}

}  // namespace kt::session
