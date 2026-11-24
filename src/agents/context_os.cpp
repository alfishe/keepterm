#include "agents/context_os.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>

#include "agents/os_access.h"

namespace kt::agents {
namespace {

std::string uri_escape(std::string_view path) {
    std::string out = "file:";
    for (char c : path) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '\\') {
            out += '/';
        } else if (c == '%' || c == '?' || c == '#' || u < 0x20) {
            static const char* hex = "0123456789ABCDEF";
            out += '%';
            out += hex[u >> 4];
            out += hex[u & 15];
        } else {
            out += c;
        }
    }
#ifdef _WIN32
    if (out.size() > 6 && out[6] == ':') out.insert(5, "/");   // file:/C:/...
#endif
    return out + "?mode=ro";
}

std::string column_text(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

}  // namespace

std::string CodexSqliteIndex::database() const {
    StdFileSystem fs;
    auto entries = fs.list_dir(dir_);
    if (!entries) return {};
    long best = -1;
    std::string name;
    for (const auto& e : *entries) {
        if (e.is_dir || !e.name.starts_with("state_") || !e.name.ends_with(".sqlite")) continue;
        const std::string mid = e.name.substr(6, e.name.size() - 6 - 7);
        if (mid.empty() || mid.size() > 6 || !std::all_of(mid.begin(), mid.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        const long n = std::stol(mid);
        if (n > best) {
            best = n;
            name = e.name;
        }
    }
    return name.empty() ? std::string() : join_path(dir_, name);
}

std::vector<CodexThread> CodexSqliteIndex::threads(std::string_view id, std::string_view cwd) const {
    std::vector<CodexThread> out;
    const std::string db_path = database();
    if (db_path.empty()) return out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(uri_escape(db_path).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_busy_timeout(db, 100);
    // Newer builds have updated_at_ms; older ones updated_at in seconds.
    static constexpr const char* kQueries[] = {
        "SELECT id, rollout_path, cwd, updated_at_ms, archived FROM threads WHERE (?1 <> '' AND id = ?1) OR "
        "(?1 = '' AND cwd = ?2) ORDER BY updated_at_ms DESC LIMIT 20",
        "SELECT id, rollout_path, cwd, updated_at * 1000, archived FROM threads WHERE (?1 <> '' AND id = ?1) OR "
        "(?1 = '' AND cwd = ?2) ORDER BY updated_at DESC LIMIT 20",
    };
    for (const char* sql : kQueries) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            continue;
        }
        // Never a null pointer: an empty string_view's data() may be one, which binds NULL.
        const std::string id_s(id), cwd_s(cwd);
        sqlite3_bind_text(st, 1, id_s.c_str(), static_cast<int>(id_s.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, cwd_s.c_str(), static_cast<int>(cwd_s.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            CodexThread t;
            t.id = column_text(st, 0);
            t.rollout_path = column_text(st, 1);
            t.cwd = column_text(st, 2);
            t.updated_at_ms = sqlite3_column_int64(st, 3);
            t.archived = sqlite3_column_int64(st, 4) != 0;
            out.push_back(std::move(t));
        }
        sqlite3_finalize(st);
        break;
    }
    sqlite3_close(db);
    return out;
}

std::vector<std::string> AgySqliteStore::generations(const std::string& db_path, std::size_t max_rows) const {
    std::vector<std::string> out;
    if (db_path.empty() || max_rows == 0) return out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(uri_escape(db_path).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_busy_timeout(db, 100);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT data FROM gen_metadata ORDER BY idx DESC LIMIT ?1", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(max_rows));
        while (sqlite3_step(st) == SQLITE_ROW) {
            const void* p = sqlite3_column_blob(st, 0);
            const int n = sqlite3_column_bytes(st, 0);
            // A generation record is a few KiB to a few hundred; larger is not one.
            if (p && n > 0 && n <= (8 << 20)) out.emplace_back(static_cast<const char*>(p), static_cast<std::size_t>(n));
        }
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    std::reverse(out.begin(), out.end());
    return out;
}

ContextPaths default_context_paths(std::string_view configured_claude_dir, std::string_view configured_codex_home) {
    ContextPaths p;
    p.home = home_directory();
    p.claude_config_dir = claude_config_dir(configured_claude_dir, environment_variable("CLAUDE_CONFIG_DIR"), p.home);
    if (!configured_codex_home.empty()) p.codex_home = std::string(configured_codex_home);
    else if (auto e = environment_variable("CODEX_HOME"); !e.empty()) p.codex_home = e;
    else p.codex_home = join_path(p.home, ".codex");
    const std::string sq = environment_variable("CODEX_SQLITE_HOME");
    p.codex_sqlite_home = sq.empty() ? p.codex_home : sq;
    p.qoder_home = p.home;
    return p;
}

}  // namespace kt::agents
