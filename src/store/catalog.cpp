#include "store/catalog.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <thread>

#include "platform/paths.h"

namespace kt::store {

namespace {

Unexpected<Error> sql_error(sqlite3* db, int rc, std::string_view what) {
    const Errc code = (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) ? Errc::busy
                      : (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) ? Errc::corrupt
                                                                      : Errc::os_error;
    return make_error(code, std::string("sqlite ") + std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "?"), rc);
}

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) { rc_ = sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return rc_ == SQLITE_OK; }
    Unexpected<Error> error(std::string_view what) const { return sql_error(db_, rc_, what); }

    Stmt& bind(int i, int64_t v) {
        if (ok()) rc_ = sqlite3_bind_int64(s_, i, v);
        return *this;
    }
    Stmt& bind(int i, uint64_t v) { return bind(i, static_cast<int64_t>(v)); }
    Stmt& bind(int i, std::string_view v) {
        if (ok()) rc_ = sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind_null(int i) {
        if (ok()) rc_ = sqlite3_bind_null(s_, i);
        return *this;
    }
    template <class T>
    Stmt& bind_opt(int i, const std::optional<T>& v) {
        return v ? bind(i, static_cast<int64_t>(*v)) : bind_null(i);
    }

    // SQLITE_ROW, SQLITE_DONE or an error code.
    int step() {
        last_ = rc_ != SQLITE_OK ? rc_ : sqlite3_step(s_);
        return last_;
    }
    int last() const { return last_; }
    Unexpected<Error> step_error(std::string_view what) const { return sql_error(db_, last_, what); }

    int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    uint64_t u64(int c) const { return static_cast<uint64_t>(sqlite3_column_int64(s_, c)); }
    bool null(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }
    std::string text(int c) const {
        const auto* p = sqlite3_column_text(s_, c);
        return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s_, c)))
                 : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
    int rc_ = SQLITE_OK;
    int last_ = SQLITE_OK;
};

// Runs a statement that returns no rows.
Result<void> run(Stmt& st, std::string_view what) {
    if (!st.ok()) return st.error(what);
    if (st.step() != SQLITE_DONE) return st.step_error(what);
    return {};
}

constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE IF NOT EXISTS sessions(
    id TEXT PRIMARY KEY,
    project TEXT NOT NULL DEFAULT '',
    host TEXT NOT NULL DEFAULT '',
    agent TEXT NOT NULL DEFAULT '',
    shell TEXT NOT NULL DEFAULT '',
    cwd TEXT NOT NULL DEFAULT '',
    started_ms INTEGER NOT NULL,
    ended_ms INTEGER,
    pinned INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS segments(
    id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL,
    kind INTEGER NOT NULL,
    series INTEGER NOT NULL,
    file TEXT NOT NULL UNIQUE,
    first_seq INTEGER NOT NULL DEFAULT 0,
    seq_end INTEGER NOT NULL DEFAULT 0,
    ts_first INTEGER NOT NULL DEFAULT 0,
    ts_last INTEGER NOT NULL DEFAULT 0,
    raw_bytes INTEGER NOT NULL DEFAULT 0,
    comp_bytes INTEGER NOT NULL DEFAULT 0,
    file_bytes INTEGER NOT NULL DEFAULT 0,
    index_bytes INTEGER NOT NULL DEFAULT 0,
    blocks INTEGER NOT NULL DEFAULT 0,
    sealed INTEGER NOT NULL DEFAULT 0,
    level INTEGER NOT NULL DEFAULT 3,
    created_ms INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS segments_series ON segments(session_id, kind, series);
CREATE INDEX IF NOT EXISTS segments_time ON segments(ts_last);
CREATE TABLE IF NOT EXISTS series_next(
    session_id TEXT NOT NULL,
    kind INTEGER NOT NULL,
    next INTEGER NOT NULL,
    PRIMARY KEY(session_id, kind));
CREATE TABLE IF NOT EXISTS commands(
    id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL,
    started_ms INTEGER NOT NULL,
    ended_ms INTEGER,
    exit_code INTEGER,
    cwd TEXT NOT NULL DEFAULT '',
    command TEXT NOT NULL DEFAULT '',
    segment_id INTEGER NOT NULL DEFAULT 0,
    block INTEGER NOT NULL DEFAULT 0,
    seq INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS commands_session ON commands(session_id, started_ms);
CREATE TABLE IF NOT EXISTS pins(
    id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL DEFAULT '',
    from_ms INTEGER NOT NULL,
    to_ms INTEGER NOT NULL,
    note TEXT NOT NULL DEFAULT '');
)sql";

constexpr const char* kSegmentColumns =
    "s.id, s.session_id, s.kind, s.series, s.file, s.first_seq, s.seq_end, s.ts_first, s.ts_last, s.raw_bytes, "
    "s.comp_bytes, s.file_bytes, s.index_bytes, s.blocks, s.sealed, s.level, s.created_ms, s.codec, s.block_raw";

SegmentRow read_segment(const Stmt& st) {
    SegmentRow r;
    r.id = st.i64(0);
    r.session = st.text(1);
    r.info.kind = static_cast<Kind>(st.i64(2));
    r.info.series = static_cast<uint32_t>(st.i64(3));
    r.file = st.text(4);
    r.info.first_seq = st.u64(5);
    r.info.seq_end = st.u64(6);
    r.info.ts_first = st.u64(7);
    r.info.ts_last = st.u64(8);
    r.info.raw_bytes = st.u64(9);
    r.info.comp_bytes = st.u64(10);
    r.info.file_bytes = st.u64(11);
    r.info.index_bytes = st.u64(12);
    r.info.blocks = static_cast<uint32_t>(st.i64(13));
    r.info.sealed = st.i64(14) != 0;
    r.info.level = static_cast<int>(st.i64(15));
    r.created_ms = st.u64(16);
    const auto codec = st.i64(17);
    r.info.codec = codec >= 0 && codec < static_cast<int64_t>(kCodecCount) ? static_cast<Codec>(codec) : Codec::zstd;
    r.info.block_raw = static_cast<uint32_t>(st.i64(18));
    return r;
}

SessionRow read_session(const Stmt& st) {
    SessionRow r;
    r.id = st.text(0);
    r.project = st.text(1);
    r.host = st.text(2);
    r.agent = st.text(3);
    r.shell = st.text(4);
    r.cwd = st.text(5);
    r.started_ms = st.u64(6);
    if (!st.null(7)) r.ended_ms = st.u64(7);
    r.pinned = st.i64(8) != 0;
    return r;
}

constexpr const char* kSessionColumns = "id, project, host, agent, shell, cwd, started_ms, ended_ms, pinned";

}  // namespace

Catalog::~Catalog() {
    if (db_) sqlite3_close_v2(db_);
}

Result<void> Catalog::exec(const char* sql) {
    char* msg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        std::string what = msg ? msg : "exec";
        sqlite3_free(msg);
        return sql_error(db_, rc, what);
    }
    return {};
}

template <class Fn>
auto Catalog::transaction(Fn&& fn) -> decltype(fn()) {
    if (auto r = exec("BEGIN IMMEDIATE"); !r) return Unexpected(r.error());
    auto result = fn();
    if (result) {
        if (auto c = exec("COMMIT"); !c) {
            (void)exec("ROLLBACK");
            return Unexpected(c.error());
        }
    } else {
        (void)exec("ROLLBACK");
    }
    return result;
}

Result<std::unique_ptr<Catalog>> Catalog::open(const std::filesystem::path& db_file) {
    std::unique_ptr<Catalog> c(new Catalog());
    const std::string name = platform::paths::to_utf8(db_file);
    const int rc = sqlite3_open_v2(name.c_str(), &c->db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) return sql_error(c->db_, rc, "open");
    sqlite3_busy_timeout(c->db_, 10'000);
    sqlite3_extended_result_codes(c->db_, 0);
    // WAL mode is persistent; switching needs an exclusive lock and may report SQLITE_BUSY
    // without calling the busy handler while another process opens the new database. Retry.
    for (int attempt = 0;; ++attempt) {
        Stmt st(c->db_, "PRAGMA journal_mode=WAL");
        if (st.ok() && st.step() == SQLITE_ROW) {
            if (st.text(0) != "wal") return make_error(Errc::not_supported, "catalog: WAL mode unavailable");
            break;
        }
        if ((st.last() != SQLITE_BUSY && st.last() != SQLITE_LOCKED) || attempt >= 500) {
            return sql_error(c->db_, st.last(), "journal_mode");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (auto r = c->exec("PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=OFF"); !r) return Unexpected(r.error());
    if (auto r = c->migrate(); !r) return Unexpected(r.error());
    return c;
}

Result<void> Catalog::migrate() {
    return transaction([&]() -> Result<void> {
        Stmt st(db_, "PRAGMA user_version");
        if (!st.ok() || st.step() != SQLITE_ROW) return sql_error(db_, st.last(), "user_version");
        version_ = static_cast<int>(st.i64(0));
        if (version_ > kCatalogSchema) {
            return make_error(Errc::not_supported, "catalog schema " + std::to_string(version_) + " is newer than this build");
        }
        if (version_ < 1) {
            if (auto r = exec(kSchemaV1); !r) return r;
            if (auto r = exec("PRAGMA user_version=1"); !r) return r;
            version_ = 1;
        }
        if (version_ < 2) {
            // v2: the codec and block size target of each segment (hybrid tiers).
            if (auto r = exec("ALTER TABLE segments ADD COLUMN codec INTEGER NOT NULL DEFAULT 1;"
                              "ALTER TABLE segments ADD COLUMN block_raw INTEGER NOT NULL DEFAULT 65536;"
                              "PRAGMA user_version=2");
                !r) {
                return r;
            }
            version_ = 2;
        }
        return {};
    });
}

Result<void> Catalog::upsert_session(const SessionRow& s) {
    std::lock_guard lk(mu_);
    return transaction([&]() -> Result<void> {
        Stmt st(db_,
                "INSERT INTO sessions(id, project, host, agent, shell, cwd, started_ms) VALUES(?,?,?,?,?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET project=excluded.project, host=excluded.host, agent=excluded.agent, "
                "shell=excluded.shell, cwd=excluded.cwd, ended_ms=NULL");
        st.bind(1, s.id).bind(2, s.project).bind(3, s.host).bind(4, s.agent).bind(5, s.shell).bind(6, s.cwd).bind(7,
                                                                                                             s.started_ms);
        return run(st, "upsert session");
    });
}

Result<void> Catalog::end_session(const std::string& id, uint64_t ended_ms) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "UPDATE sessions SET ended_ms=? WHERE id=?");
    st.bind(1, ended_ms).bind(2, id);
    return run(st, "end session");
}

Result<void> Catalog::set_pinned(const std::string& id, bool pinned) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "UPDATE sessions SET pinned=? WHERE id=?");
    st.bind(1, int64_t{pinned ? 1 : 0}).bind(2, id);
    if (auto r = run(st, "pin"); !r) return r;
    if (sqlite3_changes(db_) == 0) return make_error(Errc::invalid_argument, "no such session");
    return {};
}

Result<std::vector<SessionRow>> Catalog::sessions() {
    std::lock_guard lk(mu_);
    Stmt st(db_, (std::string("SELECT ") + kSessionColumns + " FROM sessions ORDER BY started_ms").c_str());
    if (!st.ok()) return st.error("sessions");
    std::vector<SessionRow> out;
    while (st.step() == SQLITE_ROW) out.push_back(read_session(st));
    if (st.last() != SQLITE_DONE) return st.step_error("sessions");
    return out;
}

Result<std::optional<SessionRow>> Catalog::session(const std::string& id) {
    std::lock_guard lk(mu_);
    Stmt st(db_, (std::string("SELECT ") + kSessionColumns + " FROM sessions WHERE id=?").c_str());
    st.bind(1, id);
    if (!st.ok()) return st.error("session");
    std::optional<SessionRow> out;
    if (st.step() == SQLITE_ROW) out = read_session(st);
    else if (st.last() != SQLITE_DONE) return st.step_error("session");
    return out;
}

Result<SegmentRow> Catalog::begin_segment(const std::string& session, Kind kind, uint64_t created_ms) {
    std::lock_guard lk(mu_);
    return transaction([&]() -> Result<SegmentRow> {
        int64_t next = 0;
        {
            Stmt st(db_, "SELECT next FROM series_next WHERE session_id=? AND kind=?");
            st.bind(1, session).bind(2, int64_t{static_cast<uint8_t>(kind)});
            if (!st.ok()) return st.error("series");
            if (st.step() == SQLITE_ROW) next = st.i64(0);
            else if (st.last() != SQLITE_DONE) return st.step_error("series");
        }
        {
            // Rows may exist from before the counter (or a forgotten counter): stay above them.
            Stmt st(db_, "SELECT COALESCE(MAX(series), -1) + 1 FROM segments WHERE session_id=? AND kind=?");
            st.bind(1, session).bind(2, int64_t{static_cast<uint8_t>(kind)});
            if (!st.ok() || st.step() != SQLITE_ROW) return st.step_error("series max");
            next = std::max(next, st.i64(0));
        }
        SegmentRow row;
        row.session = session;
        row.info.kind = kind;
        row.info.series = static_cast<uint32_t>(next);
        row.file = session + "/" + segment_file_name(kind, row.info.series);
        row.created_ms = created_ms;
        {
            Stmt st(db_,
                    "INSERT INTO series_next(session_id, kind, next) VALUES(?,?,?) "
                    "ON CONFLICT(session_id, kind) DO UPDATE SET next=excluded.next");
            st.bind(1, session).bind(2, int64_t{static_cast<uint8_t>(kind)}).bind(3, next + 1);
            if (auto r = run(st, "series next"); !r) return Unexpected(r.error());
        }
        {
            Stmt st(db_,
                    "INSERT INTO segments(session_id, kind, series, file, created_ms, level, codec, block_raw) "
                    "VALUES(?,?,?,?,?,?,?,?)");
            st.bind(1, session)
                .bind(2, int64_t{static_cast<uint8_t>(kind)})
                .bind(3, next)
                .bind(4, row.file)
                .bind(5, created_ms)
                .bind(6, int64_t{row.info.level})
                .bind(7, int64_t{static_cast<uint8_t>(row.info.codec)})
                .bind(8, int64_t{row.info.block_raw});
            if (auto r = run(st, "begin segment"); !r) return Unexpected(r.error());
        }
        row.id = sqlite3_last_insert_rowid(db_);
        return row;
    });
}

Result<void> Catalog::update_segment(int64_t id, const SegmentInfo& i) {
    std::lock_guard lk(mu_);
    Stmt st(db_,
            "UPDATE segments SET first_seq=?, seq_end=?, ts_first=?, ts_last=?, raw_bytes=?, comp_bytes=?, "
            "file_bytes=?, index_bytes=?, blocks=?, sealed=?, level=?, codec=?, block_raw=? WHERE id=?");
    st.bind(1, i.first_seq)
        .bind(2, i.seq_end)
        .bind(3, i.ts_first)
        .bind(4, i.ts_last)
        .bind(5, i.raw_bytes)
        .bind(6, i.comp_bytes)
        .bind(7, i.file_bytes)
        .bind(8, i.index_bytes)
        .bind(9, int64_t{i.blocks})
        .bind(10, int64_t{i.sealed ? 1 : 0})
        .bind(11, int64_t{i.level})
        .bind(12, int64_t{static_cast<uint8_t>(i.codec)})
        .bind(13, int64_t{i.block_raw})
        .bind(14, id);
    return run(st, "update segment");
}

Result<std::optional<SegmentRow>> Catalog::open_segment(const std::string& session, Kind kind) {
    std::lock_guard lk(mu_);
    Stmt st(db_, (std::string("SELECT ") + kSegmentColumns +
                  " FROM segments s WHERE s.session_id=? AND s.kind=? AND s.sealed=0 ORDER BY s.series DESC LIMIT 1")
                     .c_str());
    st.bind(1, session).bind(2, int64_t{static_cast<uint8_t>(kind)});
    if (!st.ok()) return st.error("open segment");
    std::optional<SegmentRow> out;
    if (st.step() == SQLITE_ROW) out = read_segment(st);
    else if (st.last() != SQLITE_DONE) return st.step_error("open segment");
    return out;
}

Result<std::vector<SegmentRow>> Catalog::segments(const SegmentFilter& f) {
    std::lock_guard lk(mu_);
    std::string sql = std::string("SELECT ") + kSegmentColumns +
                      " FROM segments s LEFT JOIN sessions ss ON ss.id = s.session_id WHERE 1";
    if (f.since_ms) sql += " AND s.ts_last >= ?";
    if (f.until_ms) sql += " AND s.ts_first <= ?";
    if (f.project) sql += " AND ss.project = ?";
    if (f.sealed_only) sql += " AND s.sealed = 1";
    if (!f.sessions.empty()) {
        sql += " AND s.session_id IN (";
        for (std::size_t i = 0; i < f.sessions.size(); ++i) sql += i ? ",?" : "?";
        sql += ")";
    }
    if (!f.kinds.empty()) {
        sql += " AND s.kind IN (";
        for (std::size_t i = 0; i < f.kinds.size(); ++i) sql += i ? ",?" : "?";
        sql += ")";
    }
    sql += " ORDER BY s.ts_last DESC, s.series DESC";
    Stmt st(db_, sql.c_str());
    int n = 1;
    if (f.since_ms) st.bind(n++, *f.since_ms);
    if (f.until_ms) st.bind(n++, *f.until_ms);
    if (f.project) st.bind(n++, *f.project);
    for (const auto& s : f.sessions) st.bind(n++, s);
    for (const Kind k : f.kinds) st.bind(n++, int64_t{static_cast<uint8_t>(k)});
    if (!st.ok()) return st.error("segments");
    std::vector<SegmentRow> out;
    while (st.step() == SQLITE_ROW) out.push_back(read_segment(st));
    if (st.last() != SQLITE_DONE) return st.step_error("segments");
    return out;
}

Result<void> Catalog::delete_segments(const std::vector<int64_t>& ids) {
    if (ids.empty()) return {};
    std::lock_guard lk(mu_);
    return transaction([&]() -> Result<void> {
        for (const int64_t id : ids) {
            Stmt st(db_, "DELETE FROM segments WHERE id=?");
            st.bind(1, id);
            if (auto r = run(st, "delete segment"); !r) return r;
        }
        return {};
    });
}

Result<int64_t> Catalog::replace_segments(const std::vector<int64_t>& old, const SegmentRow& n) {
    std::lock_guard lk(mu_);
    return transaction([&]() -> Result<int64_t> {
        for (const int64_t id : old) {
            Stmt st(db_, "DELETE FROM segments WHERE id=?");
            st.bind(1, id);
            if (auto r = run(st, "replace: delete"); !r) return Unexpected(r.error());
            if (sqlite3_changes(db_) == 0) return make_error(Errc::busy, "replace: a segment went away");
        }
        const auto& i = n.info;
        Stmt st(db_,
                "INSERT INTO segments(session_id, kind, series, file, first_seq, seq_end, ts_first, ts_last, raw_bytes, "
                "comp_bytes, file_bytes, index_bytes, blocks, sealed, level, created_ms, codec, block_raw) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        st.bind(1, n.session)
            .bind(2, int64_t{static_cast<uint8_t>(i.kind)})
            .bind(3, int64_t{i.series})
            .bind(4, n.file)
            .bind(5, i.first_seq)
            .bind(6, i.seq_end)
            .bind(7, i.ts_first)
            .bind(8, i.ts_last)
            .bind(9, i.raw_bytes)
            .bind(10, i.comp_bytes)
            .bind(11, i.file_bytes)
            .bind(12, i.index_bytes)
            .bind(13, int64_t{i.blocks})
            .bind(14, int64_t{i.sealed ? 1 : 0})
            .bind(15, int64_t{i.level})
            .bind(16, n.created_ms)
            .bind(17, int64_t{static_cast<uint8_t>(i.codec)})
            .bind(18, int64_t{i.block_raw});
        if (auto r = run(st, "replace: insert"); !r) return Unexpected(r.error());
        return sqlite3_last_insert_rowid(db_);
    });
}

Result<std::vector<std::string>> Catalog::forget_session(const std::string& id) {
    std::lock_guard lk(mu_);
    return transaction([&]() -> Result<std::vector<std::string>> {
        std::vector<std::string> files;
        {
            Stmt st(db_, "SELECT file FROM segments WHERE session_id=?");
            st.bind(1, id);
            if (!st.ok()) return st.error("forget");
            while (st.step() == SQLITE_ROW) files.push_back(st.text(0));
            if (st.last() != SQLITE_DONE) return st.step_error("forget");
        }
        for (const char* sql : {"DELETE FROM segments WHERE session_id=?", "DELETE FROM commands WHERE session_id=?",
                                "DELETE FROM pins WHERE session_id=?", "DELETE FROM sessions WHERE id=?"}) {
            Stmt st(db_, sql);
            st.bind(1, id);
            if (auto r = run(st, "forget"); !r) return Unexpected(r.error());
        }
        return files;
    });
}

Result<int64_t> Catalog::add_pin(const PinRow& p) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "INSERT INTO pins(session_id, from_ms, to_ms, note) VALUES(?,?,?,?)");
    st.bind(1, p.session).bind(2, p.from_ms).bind(3, p.to_ms).bind(4, p.note);
    if (auto r = run(st, "add pin"); !r) return Unexpected(r.error());
    return sqlite3_last_insert_rowid(db_);
}

Result<void> Catalog::remove_pin(int64_t id) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "DELETE FROM pins WHERE id=?");
    st.bind(1, id);
    return run(st, "remove pin");
}

Result<std::vector<PinRow>> Catalog::pins() {
    std::lock_guard lk(mu_);
    Stmt st(db_, "SELECT id, session_id, from_ms, to_ms, note FROM pins ORDER BY from_ms");
    if (!st.ok()) return st.error("pins");
    std::vector<PinRow> out;
    while (st.step() == SQLITE_ROW) out.push_back({st.i64(0), st.text(1), st.u64(2), st.u64(3), st.text(4)});
    if (st.last() != SQLITE_DONE) return st.step_error("pins");
    return out;
}

Result<int64_t> Catalog::add_command(const CommandRow& c) {
    std::lock_guard lk(mu_);
    Stmt st(db_,
            "INSERT INTO commands(session_id, started_ms, ended_ms, exit_code, cwd, command, segment_id, block, seq) "
            "VALUES(?,?,?,?,?,?,?,?,?)");
    st.bind(1, c.session)
        .bind(2, c.started_ms)
        .bind_opt(3, c.ended_ms)
        .bind_opt(4, c.exit_code)
        .bind(5, c.cwd)
        .bind(6, c.command)
        .bind(7, c.segment_id)
        .bind(8, int64_t{c.block})
        .bind(9, c.seq);
    if (auto r = run(st, "add command"); !r) return Unexpected(r.error());
    return sqlite3_last_insert_rowid(db_);
}

Result<void> Catalog::finish_command(int64_t id, uint64_t ended_ms, std::optional<int> exit_code) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "UPDATE commands SET ended_ms=?, exit_code=? WHERE id=?");
    st.bind(1, ended_ms).bind_opt(2, exit_code).bind(3, id);
    return run(st, "finish command");
}

Result<std::vector<CommandRow>> Catalog::commands(const std::string& session) {
    std::lock_guard lk(mu_);
    Stmt st(db_,
            "SELECT id, session_id, started_ms, ended_ms, exit_code, cwd, command, segment_id, block, seq FROM commands "
            "WHERE session_id=? ORDER BY started_ms, id");
    st.bind(1, session);
    if (!st.ok()) return st.error("commands");
    std::vector<CommandRow> out;
    while (st.step() == SQLITE_ROW) {
        CommandRow c;
        c.id = st.i64(0);
        c.session = st.text(1);
        c.started_ms = st.u64(2);
        if (!st.null(3)) c.ended_ms = st.u64(3);
        if (!st.null(4)) c.exit_code = static_cast<int>(st.i64(4));
        c.cwd = st.text(5);
        c.command = st.text(6);
        c.segment_id = st.i64(7);
        c.block = static_cast<uint32_t>(st.i64(8));
        c.seq = st.u64(9);
        out.push_back(std::move(c));
    }
    if (st.last() != SQLITE_DONE) return st.step_error("commands");
    return out;
}

}  // namespace kt::store
