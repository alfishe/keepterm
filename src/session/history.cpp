// Session state folder and recovery.

#include "session/history.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "session/journal.h"
#include "session/os.h"

namespace kt::session {
namespace {

void quarantine(const std::string& path) {
    (void)os::rename_file(path, path + ".corrupt-" + std::to_string(os::unix_ms() / 1000));
}

std::span<const std::byte> as_bytes(const std::string& s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

}  // namespace

json::Value to_json(const RecoveryRecord& r) {
    auto v = json::Value::object();
    v.set("schema", json::Value::integer(r.schema));
    v.set("session", json::Value::string(r.id.str()));
    v.set("created_unix_ms", json::Value::integer(static_cast<int64_t>(r.created_unix_ms)));
    v.set("updated_unix_ms", json::Value::integer(static_cast<int64_t>(r.updated_unix_ms)));
    v.set("cwd", json::Value::string(r.cwd));
    v.set("cwd_source", json::Value::string(r.cwd_source));
    if (r.history) v.set("title", json::Value::string(r.title));
    v.set("shell", json::Value::string(r.shell));
    v.set("backend", json::Value::string(r.backend));
    v.set("cols", json::Value::integer(r.cols));
    v.set("rows", json::Value::integer(r.rows));
    v.set("history", json::Value::boolean(r.history));
    if (r.restored_from) v.set("restored_from", json::Value::string(r.restored_from->str()));
    if (!r.history_id.empty()) v.set("history_id", json::Value::string(r.history_id));
    if (r.exit_code) v.set("exit_code", json::Value::integer(*r.exit_code));
    v.set("agent", r.agent);
    if (!r.agent_restore) v.set("agent_restore", json::Value::boolean(false));
    auto theme = json::Value::object();
    if (!r.theme_fg.empty()) theme.set("fg", json::Value::string(r.theme_fg));
    if (!r.theme_bg.empty()) theme.set("bg", json::Value::string(r.theme_bg));
    if (!r.theme_cursor.empty()) theme.set("cursor", json::Value::string(r.theme_cursor));
    v.set("theme", std::move(theme));
    return v;
}

Result<RecoveryRecord> recovery_record_from_json(const json::Value& v) {
    if (v.kind() != json::Value::Kind::Object) return make_error(Errc::corrupt, "record.json is not an object");
    RecoveryRecord r;
    r.schema = static_cast<uint16_t>(v.int_or("schema", 0));
    if (r.schema != 1) return make_error(Errc::not_supported, "record.json schema");
    auto id = SessionId::parse(v.str_or("session"));
    if (!id) return make_error(Errc::corrupt, "record.json without a session id");
    r.id = *id;
    r.created_unix_ms = v.uint_or("created_unix_ms", 0);
    r.updated_unix_ms = v.uint_or("updated_unix_ms", 0);
    r.cwd = v.str_or("cwd");
    r.cwd_source = v.str_or("cwd_source");
    r.title = v.str_or("title");
    r.shell = v.str_or("shell");
    r.backend = v.str_or("backend");
    r.cols = static_cast<uint16_t>(std::clamp<uint64_t>(v.uint_or("cols", 80), 1, 4096));
    r.rows = static_cast<uint16_t>(std::clamp<uint64_t>(v.uint_or("rows", 24), 1, 4096));
    r.history = v.bool_or("history", true);
    if (auto rf = SessionId::parse(v.str_or("restored_from"))) r.restored_from = *rf;
    if (auto h = SessionId::parse(v.str_or("history_id"))) r.history_id = h->str();
    if (const auto* e = v.get("exit_code"); e && e->as_int()) r.exit_code = static_cast<int32_t>(*e->as_int());
    if (const auto* a = v.get("agent")) r.agent = *a;
    r.agent_restore = v.bool_or("agent_restore", true);
    if (const auto* t = v.get("theme")) {
        r.theme_fg = t->str_or("fg");
        r.theme_bg = t->str_or("bg");
        r.theme_cursor = t->str_or("cursor");
    }
    return r;
}

std::string checkpoint_name(uint64_t seq) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "checkpoint-%016" PRIx64 ".ktsn", seq);
    return buf;
}

std::vector<std::pair<uint64_t, std::string>> list_checkpoints(const std::string& dir) {
    std::vector<std::pair<uint64_t, std::string>> out;
    for (const std::string& n : os::list_dir(dir)) {
        if (n.size() != 11 + 16 + 5 || n.rfind("checkpoint-", 0) != 0 || n.substr(27) != ".ktsn") continue;
        char* end = nullptr;
        const std::string hexpart = n.substr(11, 16);
        const uint64_t seq = std::strtoull(hexpart.c_str(), &end, 16);
        if (!end || *end != '\0') continue;
        out.emplace_back(seq, os::join(dir, n));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return out;
}

Result<RecoveryRecord> read_recovery_record(const std::string& dir) {
    const std::string path = os::join(dir, kRecordFile);
    auto text = os::read_file(path, 1 << 20);
    if (!text) return Unexpected(text.error());
    auto v = json::parse(*text);
    if (!v) {
        quarantine(path);
        return Unexpected(v.error());
    }
    return recovery_record_from_json(*v);
}

Result<void> write_recovery_record(const std::string& dir, const RecoveryRecord& r) {
    return os::write_file_atomic(os::join(dir, kRecordFile), to_json(r).dump(true) + "\n");
}

std::string restored_separator(uint64_t unix_ms) {
    const auto secs = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    return std::string("\x1b[0m\x1b[2m\xe2\x94\x80\xe2\x94\x80 restored from ") + when +
           " \xe2\x94\x80\xe2\x94\x80\x1b[0m\r\n";
}

Result<Recovered> recover_session(const std::string& dir, const vt::TerminalOptions& topts, vt::TerminalSink* sink) {
    Recovered out;
    if (auto rec = read_recovery_record(dir)) out.record = *rec;
    out.last_unix_ms = out.record ? out.record->updated_unix_ms : 0;

    journal::Contents jc;
    std::string jbytes;
    const std::string jpath = os::join(dir, kJournalFile);
    if (auto t = os::read_file(jpath, 1u << 30)) {
        jbytes = std::move(*t);
        if (auto c = journal::read(as_bytes(jbytes))) {
            jc = std::move(*c);
            out.gap = jc.gap;
            if (jc.header_valid) out.last_unix_ms = std::max(out.last_unix_ms, jc.header.last_write_unix_ms);
        } else {
            out.note += "journal unreadable; ";
        }
    }
    const auto& recs = jc.records;
    const uint64_t chain_first = recs.empty() ? UINT64_MAX : recs.front().seq;

    auto replay = [&](vt::Terminal& t, uint64_t from) {
        for (const auto& r : recs) {
            if (r.kind == journal::Kind::Output) {
                if (r.stream_end() <= from) continue;
                auto bytes = r.payload;
                if (r.seq < from) bytes = bytes.subspan(static_cast<std::size_t>(from - r.seq));
                t.feed(bytes);
                ++out.replayed_records;
                out.replayed_bytes += bytes.size();
            } else if (r.kind == journal::Kind::Resize && r.payload.size() >= 4 && r.seq >= from) {
                const auto* p = reinterpret_cast<const uint8_t*>(r.payload.data());
                vt::GridSize g;
                g.cols = static_cast<uint16_t>(p[0] | (p[1] << 8));
                g.rows = static_cast<uint16_t>(p[2] | (p[3] << 8));
                if (g.cols > 0 && g.rows > 0) t.resize(g);
            }
        }
    };

    for (const auto& [seq, path] : list_checkpoints(dir)) {
        // The checkpoint is usable when the journal continues from it (the chain starts at or
        // before its seq) or when there is no journal chain at all.
        const bool covered = recs.empty() || chain_first <= seq;
        if (!covered) {
            out.note += "checkpoint " + std::to_string(seq) + " older than the ring; ";
            continue;
        }
        auto data = os::read_file(path, 1u << 30);
        if (!data) continue;
        auto t = vt::Terminal::restore(as_bytes(*data), topts, sink, vt::RestoreOptions{});
        if (!t) {
            quarantine(path);
            out.note += "checkpoint " + std::to_string(seq) + " damaged; ";
            continue;
        }
        out.term = std::move(*t);
        out.checkpoint_seq = seq;
        replay(*out.term, seq);
        break;
    }
    if (!out.term) {
        vt::GridSize g;
        if (jc.header_valid && jc.header.cols && jc.header.rows) {
            g.cols = jc.header.cols;
            g.rows = jc.header.rows;
        } else if (out.record) {
            g.cols = out.record->cols;
            g.rows = out.record->rows;
        }
        // The newest size; Resize records in the ring correct it as the replay goes.
        auto t = vt::Terminal::create(g, topts, sink);
        if (!t) return Unexpected(t.error());
        out.term = std::move(*t);
        out.ring_only = !recs.empty();
        replay(*out.term, 0);
    }
    out.term->reset_program_modes();
    return out;
}

}  // namespace kt::session
