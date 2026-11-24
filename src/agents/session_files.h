// The agents' own session files, read through IFileSystem so that tests run on
// fixtures. Port of Macterm's AgentSessionFiles. Every read fails soft: an unknown format, a
// missing folder or an unreadable file yields "no session", never an error. Core code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agents/registry.h"

namespace kt::agents {

// ---- Platform access (the platform layer owns the real interfaces; these are the subset this module needs)

struct DirEntry {
    std::string name;
    bool is_dir = false;
    std::int64_t mtime_ns = 0;  // last write, nanoseconds since the Unix epoch
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    // The whole file when it is not larger than `max_bytes`; nullopt when missing, unreadable or
    // larger. Paths are UTF-8; '/' separates on every OS.
    virtual std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes) const = 0;
    // nullopt when the folder is missing or unreadable.
    virtual std::optional<std::vector<DirEntry>> list_dir(const std::string& path) const = 0;
    virtual std::optional<std::int64_t> mtime_ns(const std::string& path) const = 0;

    // ---- Large append-only files (the context probes, agents/context.h)
    struct FileStat {
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t id = 0;   // inode and device where the OS gives them cheaply; 0 = unknown
    };
    // The defaults go through read_file (fixtures); StdFileSystem reads only what is asked for.
    virtual std::optional<FileStat> stat(const std::string& path) const {
        auto m = mtime_ns(path);
        auto d = read_file(path, std::size_t{1} << 30);
        if (!m || !d) return std::nullopt;
        return FileStat{d->size(), *m, 0};
    }
    // Up to `max_bytes` from `offset`; empty at or past the end; nullopt when unreadable.
    virtual std::optional<std::string> read_range(const std::string& path, std::uint64_t offset,
                                                  std::size_t max_bytes) const {
        auto d = read_file(path, std::size_t{1} << 30);
        if (!d) return std::nullopt;
        if (offset >= d->size()) return std::string();
        return d->substr(static_cast<std::size_t>(offset), max_bytes);
    }
};

class IProcessProbe {
public:
    virtual ~IProcessProbe() = default;
    virtual bool is_alive(std::uint32_t pid) const = 0;
    // Creation time in nanoseconds since the Unix epoch, when the OS gives it cheaply (Windows:
    // pid reuse is fast). nullopt: unknown, no reuse check.
    virtual std::optional<std::uint64_t> start_time(std::uint32_t /*pid*/) const { return std::nullopt; }
};

// `dir` + '/' + `name` (no separator added when `dir` ends with one).
std::string join_path(std::string_view dir, std::string_view name);

// ---- Sources

// What is known about the agent process whose session is wanted.
struct SessionQuery {
    std::uint32_t pid = 0;
    std::uint64_t start_time = 0;  // nanoseconds since the Unix epoch; 0 = unknown
    std::string cwd;               // the agent's directory as far as known; empty = unknown
    std::vector<std::string> argv{};   // its command line when known (an explicit session id)
};

struct AgentSessionInfo {
    std::string session_id;
    std::optional<std::string> cwd;
    std::optional<std::string> kind;    // Claude: "interactive", "bg", ...
    std::optional<std::string> status;  // Claude: "idle", "busy", ...
    std::optional<std::string> name;    // Claude: the session's name ("web-refactor")
    bool operator==(const AgentSessionInfo&) const = default;
};

class ICodexIndex;   // agents/context.h

class ISessionSource {
public:
    virtual ~ISessionSource() = default;
    virtual std::optional<AgentSessionInfo> session_of(const SessionQuery& q) const = 0;
    // A live process has session `id` (another pane, another terminal, a background agent).
    virtual bool session_is_running(std::string_view session_id) const = 0;
    virtual bool is_ready(const SessionQuery& q) const = 0;
    // The files alone prove that process `q` runs this agent, without its command line.
    virtual bool proves(const SessionQuery& /*q*/) const { return false; }
};

// A file time may trail or lead the process start time by clock granularity.
inline constexpr std::int64_t kTimeToleranceNs = 2'000'000'000;
// Session files are small; larger ones are not read.
inline constexpr std::size_t kMaxSessionFileBytes = 64 * 1024;
// Qoder segment files examined per scan.
inline constexpr std::size_t kMaxQoderSegments = 2000;

// <configured> when set, else CLAUDE_CONFIG_DIR, else <home>/.claude.
std::string claude_config_dir(std::string_view configured, std::string_view env_config_dir, std::string_view home);
// Claude Code's project folder name for `cwd`: every character other than [A-Za-z0-9] becomes
// '-' (one per UTF-16 unit, as Claude Code's JavaScript does), "/w/my.api" -> "-w-my-api",
// "C:\work" -> "C--work".
std::string claude_project_dir_name(std::string_view cwd);
// "1763-2-p31337.jsonl" -> 31337; nullopt for anything else.
std::optional<std::uint32_t> qoder_pid(std::string_view segment_file_name);

// Claude Code. Each running interactive process writes <config_dir>/sessions/<pid>.json with
// sessionId, cwd, kind and status. When that folder holds no pid files (an agent version or OS
// that does not write them), the session is the newest transcript
// <config_dir>/projects/<project_dir_name(cwd)>/<uuid>.jsonl written since the process started.
class ClaudeSessionFiles final : public ISessionSource {
public:
    ClaudeSessionFiles(const IFileSystem& fs, const IProcessProbe& probe, std::string config_dir);

    std::optional<AgentSessionInfo> session_of(const SessionQuery& q) const override;
    bool session_is_running(std::string_view session_id) const override;
    bool is_ready(const SessionQuery& q) const override;
    bool proves(const SessionQuery& q) const override;

    // The pid file as written, without the start time check. nullopt when missing or invalid.
    std::optional<AgentSessionInfo> pid_file(std::uint32_t pid) const;
    // The transcript fallback alone.
    std::optional<AgentSessionInfo> transcript_session(std::string_view cwd, std::uint64_t start_time) const;

private:
    std::optional<AgentSessionInfo> fresh_pid_file(const SessionQuery& q) const;
    bool writes_pid_files() const;

    const IFileSystem& fs_;
    const IProcessProbe& probe_;
    std::string config_dir_;
};

// Qoder CLI keeps <home>/.qoder/logs/sessions/<cwd-dir>/<session-id>/segments/<run>-p<pid>.jsonl
// for each run. The folder name is the session id.
class QoderSessionFiles final : public ISessionSource {
public:
    QoderSessionFiles(const IFileSystem& fs, const IProcessProbe& probe, std::string home);

    std::optional<AgentSessionInfo> session_of(const SessionQuery& q) const override;
    bool session_is_running(std::string_view session_id) const override;
    bool is_ready(const SessionQuery& q) const override;

private:
    struct Segment {
        std::string session_id;
        std::uint32_t pid = 0;
        std::int64_t mtime_ns = 0;
    };
    std::vector<Segment> segments() const;

    const IFileSystem& fs_;
    const IProcessProbe& probe_;
    std::string home_;
};

// The Antigravity CLI (`agy`) writes <home>/.gemini/antigravity-cli/log/cli-<local time>.log per
// run. Its first line names the process ("Starting language server process with pid N"); a new
// conversation logs "Created conversation <id>", an opened one "Streaming conversation <id>"
// (the last one is the current). Without the log: cache/last_conversations.json maps a folder to
// its last conversation (what `agy --continue` opens), taken when conversations/<id>.db was
// written since the process started.
class AgySessionFiles final : public ISessionSource {
public:
    AgySessionFiles(const IFileSystem& fs, const IProcessProbe& probe, std::string home);

    std::optional<AgentSessionInfo> session_of(const SessionQuery& q) const override;
    bool session_is_running(std::string_view session_id) const override;
    bool is_ready(const SessionQuery& q) const override;

    // The pid in a log's first line; nullopt for anything else.
    static std::optional<std::uint32_t> log_pid(std::string_view first_line);
    // The current conversation of a log's text: the last "Streaming conversation <id>", else the
    // last "Created conversation <id>"; empty when none.
    static std::string log_conversation(std::string_view text);

private:
    struct RunLog {
        std::string path;
        std::uint32_t pid = 0;
        std::int64_t mtime_ns = 0;
    };
    // The newest run logs (at most a few dozen), newest first.
    std::vector<RunLog> logs() const;
    std::optional<RunLog> log_of(const SessionQuery& q) const;
    std::string conversation_in(const RunLog& l) const;

    const IFileSystem& fs_;
    const IProcessProbe& probe_;
    std::string dir_;   // <home>/.gemini/antigravity-cli
};

struct SourcePaths {
    std::string home;
    std::string claude_config_dir;  // claude_config_dir(...) result
    std::string codex_home{};       // agents.codex.home, else CODEX_HOME, else <home>/.codex
};

// Attaches the session sources of Claude Code, Qoder CLI, Codex and the Antigravity CLI to the
// built-in descriptors of `registry`. `fs`, `probe` and `codex_index` (may be null: rollouts
// only) must outlive the registry.
void attach_builtin_sources(AgentRegistry& registry, const IFileSystem& fs, const IProcessProbe& probe,
                            const SourcePaths& paths, const ICodexIndex* codex_index = nullptr);

}  // namespace kt::agents
