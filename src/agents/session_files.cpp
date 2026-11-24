#include "agents/session_files.h"

#include <algorithm>
#include <limits>

#include "agents/context.h"
#include "agents/json_lite.h"

namespace kt::agents {
namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::optional<std::uint32_t> parse_pid(std::string_view digits) {
    if (!all_digits(digits) || digits.size() > 10) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : digits) v = v * 10 + static_cast<std::uint64_t>(c - '0');
    if (v == 0 || v > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(v);
}

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool is_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !is_hex(s[i])) return false;
    }
    return true;
}

// A file older than the process (minus the tolerance) was written by an earlier process.
constexpr std::uint64_t kTolerance = static_cast<std::uint64_t>(kTimeToleranceNs);

std::uint64_t non_negative(std::int64_t v) { return v < 0 ? 0 : static_cast<std::uint64_t>(v); }

bool older_than_start(std::int64_t mtime_ns, std::uint64_t start_time) {
    return start_time != 0 && non_negative(mtime_ns) + kTolerance < start_time;
}

std::optional<AgentSessionInfo> parse_claude_session(std::string_view text) {
    auto v = json::parse(text);
    if (!v || v->kind != json::Value::Kind::Object) return std::nullopt;
    auto id = v->string_at("sessionId");
    if (!id || id->empty()) return std::nullopt;
    AgentSessionInfo info;
    info.session_id = std::move(*id);
    info.cwd = v->string_at("cwd");
    if (info.cwd && info.cwd->empty()) info.cwd.reset();
    info.kind = v->string_at("kind");
    info.status = v->string_at("status");
    info.name = v->string_at("name");
    if (info.name && info.name->empty()) info.name.reset();
    return info;
}

}  // namespace

std::string join_path(std::string_view dir, std::string_view name) {
    std::string out(dir);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out += '/';
    out += name;
    return out;
}

std::string claude_config_dir(std::string_view configured, std::string_view env_config_dir, std::string_view home) {
    if (!configured.empty()) return std::string(configured);
    if (!env_config_dir.empty()) return std::string(env_config_dir);
    return join_path(home, ".claude");
}

std::string claude_project_dir_name(std::string_view cwd) {
    std::string out;
    out.reserve(cwd.size());
    std::size_t i = 0;
    while (i < cwd.size()) {
        const auto c = static_cast<unsigned char>(cwd[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        // One '-' per UTF-16 unit: a 4-byte UTF-8 sequence is a surrogate pair.
        std::size_t len = 1;
        if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC2 && c < 0xE0) {
            len = 2;
        }
        bool valid = i + len <= cwd.size();
        for (std::size_t k = 1; valid && k < len; ++k) {
            valid = (static_cast<unsigned char>(cwd[i + k]) & 0xC0) == 0x80;
        }
        if (!valid) len = 1;
        out += (valid && len == 4) ? "--" : "-";
        i += len;
    }
    return out;
}

std::optional<std::uint32_t> qoder_pid(std::string_view name) {
    constexpr std::string_view ext = ".jsonl";
    if (!ends_with(name, ext)) return std::nullopt;
    const std::string_view stem = name.substr(0, name.size() - ext.size());
    const auto marker = stem.rfind("-p");
    if (marker == std::string_view::npos) return std::nullopt;
    return parse_pid(stem.substr(marker + 2));
}

// ---- Claude Code

ClaudeSessionFiles::ClaudeSessionFiles(const IFileSystem& fs, const IProcessProbe& probe, std::string config_dir)
    : fs_(fs), probe_(probe), config_dir_(std::move(config_dir)) {}

std::optional<AgentSessionInfo> ClaudeSessionFiles::pid_file(std::uint32_t pid) const {
    if (pid == 0) return std::nullopt;
    const std::string path = join_path(join_path(config_dir_, "sessions"), std::to_string(pid) + ".json");
    auto text = fs_.read_file(path, kMaxSessionFileBytes);
    if (!text) return std::nullopt;
    return parse_claude_session(*text);
}

std::optional<AgentSessionInfo> ClaudeSessionFiles::fresh_pid_file(const SessionQuery& q) const {
    auto info = pid_file(q.pid);
    if (!info) return std::nullopt;
    if (q.start_time != 0) {
        const std::string path = join_path(join_path(config_dir_, "sessions"), std::to_string(q.pid) + ".json");
        if (auto m = fs_.mtime_ns(path); m && older_than_start(*m, q.start_time)) return std::nullopt;
    }
    return info;
}

bool ClaudeSessionFiles::writes_pid_files() const {
    auto entries = fs_.list_dir(join_path(config_dir_, "sessions"));
    if (!entries) return false;
    return std::any_of(entries->begin(), entries->end(), [](const DirEntry& e) {
        return !e.is_dir && ends_with(e.name, ".json") && parse_pid(e.name.substr(0, e.name.size() - 5));
    });
}

std::optional<AgentSessionInfo> ClaudeSessionFiles::transcript_session(std::string_view cwd,
                                                                       std::uint64_t start_time) const {
    if (cwd.empty() || start_time == 0) return std::nullopt;
    const std::string dir = join_path(join_path(config_dir_, "projects"), claude_project_dir_name(cwd));
    auto entries = fs_.list_dir(dir);
    if (!entries) return std::nullopt;
    const DirEntry* best = nullptr;
    for (const DirEntry& e : *entries) {
        if (e.is_dir || !ends_with(e.name, ".jsonl")) continue;
        if (!is_uuid(std::string_view(e.name).substr(0, e.name.size() - 6))) continue;  // not agent-*.jsonl
        if (older_than_start(e.mtime_ns, start_time)) continue;
        if (best == nullptr || e.mtime_ns > best->mtime_ns) best = &e;
    }
    if (best == nullptr) return std::nullopt;
    AgentSessionInfo info;
    info.session_id = best->name.substr(0, best->name.size() - 6);
    info.cwd = std::string(cwd);
    return info;
}

std::optional<AgentSessionInfo> ClaudeSessionFiles::session_of(const SessionQuery& q) const {
    if (auto info = fresh_pid_file(q)) return info;
    // The fallback cannot tell two sessions in one folder apart, so it runs only when this
    // Claude Code writes no pid files at all.
    if (writes_pid_files()) return std::nullopt;
    return transcript_session(q.cwd, q.start_time);
}

bool ClaudeSessionFiles::session_is_running(std::string_view session_id) const {
    const std::string dir = join_path(config_dir_, "sessions");
    auto entries = fs_.list_dir(dir);
    if (!entries) return false;
    for (const DirEntry& e : *entries) {
        if (e.is_dir || !ends_with(e.name, ".json")) continue;
        auto pid = parse_pid(std::string_view(e.name).substr(0, e.name.size() - 5));
        if (!pid) continue;
        auto info = pid_file(*pid);
        if (!info || info->session_id != session_id || !probe_.is_alive(*pid)) continue;
        // A process younger than the file reused the pid of the writer.
        if (auto started = probe_.start_time(*pid); started && e.mtime_ns != 0) {
            if (*started > non_negative(e.mtime_ns) + kTolerance) continue;
        }
        return true;
    }
    return false;
}

bool ClaudeSessionFiles::is_ready(const SessionQuery& q) const {
    if (auto info = fresh_pid_file(q)) return info->status == "idle";
    if (writes_pid_files()) return false;
    return transcript_session(q.cwd, q.start_time).has_value();
}

bool ClaudeSessionFiles::proves(const SessionQuery& q) const { return fresh_pid_file(q).has_value(); }

// ---- Qoder CLI

QoderSessionFiles::QoderSessionFiles(const IFileSystem& fs, const IProcessProbe& probe, std::string home)
    : fs_(fs), probe_(probe), home_(std::move(home)) {}

std::vector<QoderSessionFiles::Segment> QoderSessionFiles::segments() const {
    struct SessionDir {
        std::string path;
        std::string id;
        std::int64_t mtime_ns;
    };
    const std::string root = join_path(home_, ".qoder/logs/sessions");
    std::vector<SessionDir> sessions;
    if (auto cwd_dirs = fs_.list_dir(root)) {
        for (const DirEntry& cwd_dir : *cwd_dirs) {
            if (!cwd_dir.is_dir) continue;
            const std::string cwd_path = join_path(root, cwd_dir.name);
            auto session_dirs = fs_.list_dir(cwd_path);
            if (!session_dirs) continue;
            for (const DirEntry& s : *session_dirs) {
                if (s.is_dir) sessions.push_back({join_path(cwd_path, s.name), s.name, s.mtime_ns});
            }
        }
    }
    // Beyond the cap, the newest sessions by folder time.
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionDir& a, const SessionDir& b) { return a.mtime_ns > b.mtime_ns; });
    std::vector<Segment> out;
    for (const SessionDir& s : sessions) {
        auto files = fs_.list_dir(join_path(s.path, "segments"));
        if (!files) continue;
        for (const DirEntry& f : *files) {
            if (f.is_dir) continue;
            auto pid = qoder_pid(f.name);
            if (!pid) continue;
            out.push_back({s.id, *pid, f.mtime_ns});
            if (out.size() >= kMaxQoderSegments) return out;
        }
    }
    return out;
}

std::optional<AgentSessionInfo> QoderSessionFiles::session_of(const SessionQuery& q) const {
    if (q.pid == 0) return std::nullopt;
    std::optional<Segment> best;
    for (Segment& s : segments()) {
        if (s.pid != q.pid || older_than_start(s.mtime_ns, q.start_time)) continue;
        // A /resume inside a run opens a second segment: the newest wins.
        if (!best || s.mtime_ns > best->mtime_ns) best = std::move(s);
    }
    if (!best) return std::nullopt;
    AgentSessionInfo info;
    info.session_id = std::move(best->session_id);
    return info;
}

bool QoderSessionFiles::session_is_running(std::string_view session_id) const {
    for (const Segment& s : segments()) {
        if (s.session_id == session_id && probe_.is_alive(s.pid)) return true;
    }
    return false;
}

bool QoderSessionFiles::is_ready(const SessionQuery& q) const { return session_of(q).has_value(); }

// ---- Antigravity CLI

namespace {

constexpr std::size_t kMaxAgyLogs = 32;
constexpr std::size_t kAgyLogTail = 4u << 20;   // run logs reach a few MiB

// The id after the last `marker` that is followed by one; empty when none.
std::string last_id_after(std::string_view text, std::string_view marker) {
    std::size_t pos = text.size();
    while (pos > 0) {
        const std::size_t at = text.rfind(marker, pos - 1);
        if (at == std::string_view::npos) break;
        const std::string_view id = text.substr(at + marker.size(), 36);
        if (is_uuid(id)) return std::string(id);
        if (at == 0) break;
        pos = at;
    }
    return {};
}

bool same_folder(std::string_view a, std::string_view b) {
    while (a.size() > 1 && (a.back() == '/' || a.back() == '\\')) a.remove_suffix(1);
    while (b.size() > 1 && (b.back() == '/' || b.back() == '\\')) b.remove_suffix(1);
    if (a.size() != b.size()) return false;
    // A drive letter path compares without case (Windows).
    const bool fold = a.size() >= 2 && a[1] == ':';
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i] == '\\' ? '/' : a[i];
        char y = b[i] == '\\' ? '/' : b[i];
        if (fold) {
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) return false;
    }
    return true;
}

// The id given on the command line: --conversation <id> / --conversation=<id>.
std::string agy_argv_conversation(const std::vector<std::string>& argv) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& w = argv[i];
        if (w == "--conversation" && i + 1 < argv.size() && is_uuid(argv[i + 1])) return argv[i + 1];
        if (w.starts_with("--conversation=") && is_uuid(std::string_view(w).substr(15))) return w.substr(15);
    }
    return {};
}

}  // namespace

AgySessionFiles::AgySessionFiles(const IFileSystem& fs, const IProcessProbe& probe, std::string home)
    : fs_(fs), probe_(probe), dir_(join_path(join_path(home, ".gemini"), "antigravity-cli")) {}

std::optional<std::uint32_t> AgySessionFiles::log_pid(std::string_view first_line) {
    constexpr std::string_view kMarker = "Starting language server process with pid ";
    const std::size_t at = first_line.find(kMarker);
    if (at == std::string_view::npos) return std::nullopt;
    std::uint64_t pid = 0;
    std::size_t i = at + kMarker.size(), digits = 0;
    for (; i < first_line.size() && first_line[i] >= '0' && first_line[i] <= '9' && digits < 10; ++i, ++digits)
        pid = pid * 10 + static_cast<std::uint64_t>(first_line[i] - '0');
    if (digits == 0 || pid == 0 || pid > 0xFFFFFFFFu) return std::nullopt;
    if (i < first_line.size() && first_line[i] >= '0' && first_line[i] <= '9') return std::nullopt;
    return static_cast<std::uint32_t>(pid);
}

std::string AgySessionFiles::log_conversation(std::string_view text) {
    std::string id = last_id_after(text, "Streaming conversation ");
    if (id.empty()) id = last_id_after(text, "Created conversation ");
    return id;
}

std::vector<AgySessionFiles::RunLog> AgySessionFiles::logs() const {
    std::vector<RunLog> out;
    const std::string dir = join_path(dir_, "log");
    auto entries = fs_.list_dir(dir);
    if (!entries) return out;
    std::vector<DirEntry> files;
    for (DirEntry& e : *entries) {
        if (!e.is_dir && e.name.starts_with("cli-") && e.name.ends_with(".log")) files.push_back(std::move(e));
    }
    std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) { return a.mtime_ns > b.mtime_ns; });
    if (files.size() > kMaxAgyLogs) files.resize(kMaxAgyLogs);
    for (DirEntry& f : files) {
        RunLog l;
        l.path = join_path(dir, f.name);
        l.mtime_ns = f.mtime_ns;
        auto head = fs_.read_range(l.path, 0, 512);
        if (!head) continue;
        const std::string_view first = std::string_view(*head).substr(0, head->find('\n'));
        auto pid = log_pid(first);
        if (!pid) continue;
        l.pid = *pid;
        out.push_back(std::move(l));
    }
    return out;
}

std::optional<AgySessionFiles::RunLog> AgySessionFiles::log_of(const SessionQuery& q) const {
    if (q.pid == 0) return std::nullopt;
    for (RunLog& l : logs()) {
        // An older run with the same pid wrote its log before this process started.
        if (l.pid == q.pid && !older_than_start(l.mtime_ns, q.start_time)) return std::move(l);
    }
    return std::nullopt;
}

std::string AgySessionFiles::conversation_in(const RunLog& l) const {
    auto st = fs_.stat(l.path);
    if (!st) return {};
    const std::uint64_t from = st->size > kAgyLogTail ? st->size - kAgyLogTail : 0;
    auto text = fs_.read_range(l.path, from, kAgyLogTail);
    return text ? log_conversation(*text) : std::string();
}

std::optional<AgentSessionInfo> AgySessionFiles::session_of(const SessionQuery& q) const {
    AgentSessionInfo info;
    info.session_id = agy_argv_conversation(q.argv);
    if (info.session_id.empty()) {
        if (auto l = log_of(q)) {
            info.session_id = conversation_in(*l);
            if (info.session_id.empty()) return std::nullopt;   // no conversation yet
        }
    }
    if (info.session_id.empty() && !q.cwd.empty()) {
        // No run log (another --log-file): the folder's last conversation, if this run wrote it.
        auto text = fs_.read_file(join_path(join_path(dir_, "cache"), "last_conversations.json"), kMaxSessionFileBytes * 16);
        auto v = text ? json::parse(*text) : std::nullopt;
        if (v && v->kind == json::Value::Kind::Object) {
            for (const auto& [folder, id] : v->members) {
                if (id.kind != json::Value::Kind::String || !is_uuid(id.text) || !same_folder(folder, q.cwd)) continue;
                auto m = fs_.mtime_ns(join_path(join_path(dir_, "conversations"), id.text + ".db"));
                if (m && !older_than_start(*m, q.start_time)) info.session_id = id.text;
            }
        }
    }
    if (info.session_id.empty()) return std::nullopt;
    return info;
}

bool AgySessionFiles::session_is_running(std::string_view session_id) const {
    for (const RunLog& l : logs()) {
        if (!probe_.is_alive(l.pid)) continue;
        // pid reuse: a process that started after the log's last write is another one.
        if (auto st = probe_.start_time(l.pid); st && older_than_start(l.mtime_ns, *st)) continue;
        if (conversation_in(l) == session_id) return true;
    }
    return false;
}

bool AgySessionFiles::is_ready(const SessionQuery& q) const {
    if (auto l = log_of(q)) return !conversation_in(*l).empty();
    // No run log of its own: ready once the log folder shows none is coming (another --log-file).
    return !fs_.list_dir(join_path(dir_, "log")).has_value();
}

void attach_builtin_sources(AgentRegistry& registry, const IFileSystem& fs, const IProcessProbe& probe,
                            const SourcePaths& paths, const ICodexIndex* codex_index) {
    registry.attach_source("claude", std::make_unique<ClaudeSessionFiles>(fs, probe, paths.claude_config_dir));
    registry.attach_source("qodercli", std::make_unique<QoderSessionFiles>(fs, probe, paths.home));
    const std::string codex_home = paths.codex_home.empty() ? join_path(paths.home, ".codex") : paths.codex_home;
    registry.attach_source("codex", make_codex_session_source(fs, codex_index, codex_home));
    registry.attach_source("antigravity", std::make_unique<AgySessionFiles>(fs, probe, paths.home));
}

}  // namespace kt::agents
