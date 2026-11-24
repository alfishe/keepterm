// Agent context telemetry, phase 1. Core code.
//
// A probe finds the file an agent writes its usage records to (locate) and makes a reducer for
// it. The reducer is fed the appended bytes of that file and keeps a ContextUsage: numbers,
// model and version ids and nothing else. No message text is copied: records are read with the
// skipping scanner of json_scan.h, which steps over every value that is not a usage field.
//
// Built-in probes: Claude Code (transcript JSONL), Codex CLI (rollout JSONL), Qoder CLI
// (transcript JSONL, window and compactions only; the fill is an estimate). Every probe fails
// soft: an unknown record, a missing field, a torn line or a newer format version gives
// "no data" or FormatStatus::UnknownVersion, never an error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/session_files.h"

namespace kt::agents {

// Where a figure comes from. Higher ranks win when two sources have it (StatusLine is the
// agent's own figure; Estimate is the weakest).
enum class Source : std::uint8_t {
    Unknown = 0,
    Data = 1,        // the agent's own files carry it (Codex model_context_window, Qoder contextWindow)
    Table = 2,       // keepterm's model table (context_windows.inc)
    Inferred = 3,    // evidence: a prompt or a compaction above 200k means a 1M window
    Config = 4,      // agents.context.windows
    StatusLine = 5,  // Claude Code's statusLine JSON through the relay (exact)
    Estimate = 6,    // a rough figure (Qoder writes no per call usage)
};
std::string_view to_string(Source);

enum class Activity : std::uint8_t { Unknown = 0, Busy = 1, Waiting = 2, Idle = 3 };
std::string_view to_string(Activity);

// Unknown format version: the records parse but carry a version this build was not written for,
// or a known record lacks every field this build reads. The values are best effort.
enum class FormatStatus : std::uint8_t { NoData = 0, Ok = 1, UnknownVersion = 2 };

struct RateLimit {
    std::string name;             // "primary", "secondary" (Codex); "five_hour", "seven_day" (Claude)
    double used_percent = 0;      // may exceed 100
    std::uint32_t window_minutes = 0;   // 0 = unknown
    std::int64_t resets_at_unix = 0;    // seconds; 0 = unknown
    bool operator==(const RateLimit&) const = default;
};

struct ContextUsage {
    std::string agent;        // descriptor id: "claude", "codex", "qodercli"
    std::string session_id;
    std::optional<std::uint64_t> used_tokens;    // prompt of the newest main-chain call
    Source used_source = Source::Unknown;        // Data, StatusLine or Estimate
    std::optional<std::uint64_t> window_tokens;  // the window the percent is of
    Source window_source = Source::Unknown;
    std::optional<double> percent;               // used, in the agent's own formula (0..100+)
    std::string model;                           // API model id
    std::string agent_version;
    std::uint64_t input_tokens = 0, output_tokens = 0, cache_read_tokens = 0, cache_write_tokens = 0;
    bool totals_since_attach = false;  // the totals cover only what keepterm read (a tail)
    std::optional<double> cost_usd;    // StatusLine only
    std::uint32_t turns = 0, compactions = 0, api_calls = 0;
    bool after_compaction = false;     // used_tokens is the compaction's result; no call since
    std::vector<RateLimit> limits;
    Source limits_source = Source::Unknown;
    // When the limits were current: the time of the API response they came with, as far as
    // keepterm can tell (0 = unknown).
    std::int64_t limits_unix_ms = 0;
    Activity activity = Activity::Unknown;
    std::string session_name, session_cwd;   // what the agent registered (Claude's pid file)
    std::int64_t updated_unix_ms = 0;  // time of the newest record applied (0 = unknown)
    FormatStatus format = FormatStatus::NoData;
    std::string format_version;        // the agent version that was not recognized
    bool operator==(const ContextUsage&) const = default;
};

// ---- Reducers

class IContextReducer {
public:
    virtual ~IContextReducer() = default;
    // Bytes appended to the file since the last call. Partial lines are kept until their newline.
    virtual void feed(std::string_view appended_bytes) = 0;
    // Attach in the middle of the file: forgets everything, then reduces `tail`, whose first
    // (partial) line is dropped. The totals are marked "since attach".
    virtual void reset_from_tail(std::string_view tail) = 0;
    // Attach at the start of the file: forgets everything; feed() follows.
    virtual void reset() = 0;
    virtual const ContextUsage& usage() const = 0;
    // A usage record (or what this agent has instead) has been applied since the last reset.
    virtual bool saw_usage() const = 0;
    // A reducer of a database rather than of an appended file (the Antigravity CLI): the tracker
    // never calls feed() but reread() with the path, at attach and whenever the file or its
    // "-wal" changes. reread() fails soft (no file, locked, another schema: no data).
    virtual bool rereads() const { return false; }
    virtual void reread(const std::string& /*path*/) {}
};

// ---- Probes

struct ContextQuery {
    SessionQuery process;                     // the agent process: pid, start time, cwd
    std::vector<std::string> argv;            // its arguments when known (not on Windows)
    std::string pane_cwd;
    std::optional<AgentSessionInfo> session;  // from the descriptor's session source, if any
    // Claude statusLine relay: the session and transcript the agent itself reported.
    std::string reported_session;
    std::string reported_transcript;
};

struct ContextTarget {
    std::string path;        // the JSONL file
    std::string session_id;
    std::string watch_dir;   // the folder to watch (the file's parent)
    bool operator==(const ContextTarget&) const = default;
};

class IContextProbe {
public:
    virtual ~IContextProbe() = default;
    virtual std::string_view agent() const = 0;
    virtual std::optional<ContextTarget> locate(const ContextQuery& q) const = 0;
    virtual std::unique_ptr<IContextReducer> reducer(const ContextTarget& t) const = 0;
    // Activity that lives outside the file (Claude: the pid file's status). Unknown: none.
    virtual Activity activity(const ContextQuery& /*q*/) const { return Activity::Unknown; }
    // The session's name and folder as the agent registered them (Claude: the pid file).
    struct SessionLabel {
        std::string name, cwd;
    };
    virtual SessionLabel session_label(const ContextQuery& /*q*/) const { return {}; }
};

// A model id glob ('*' matches any run, case-sensitive) and its window in tokens.
using WindowOverrides = std::vector<std::pair<std::string, std::uint64_t>>;

struct ContextPaths {
    std::string home;
    std::string claude_config_dir;   // claude_config_dir(...) result
    std::string codex_home;          // agents.codex.home, else CODEX_HOME, else <home>/.codex
    std::string codex_sqlite_home;   // CODEX_SQLITE_HOME, else codex_home
    std::string qoder_home;          // <home>/.qoder
};

// One Codex thread row of the state database (`threads` table).
struct CodexThread {
    std::string id;
    std::string rollout_path;
    std::string cwd;
    std::int64_t updated_at_ms = 0;
    bool archived = false;
};

// The Codex state database index (read-only). Core code sees only this interface; the SQLite
// implementation is in kt_agents_os (agents/context_os.h).
class ICodexIndex {
public:
    virtual ~ICodexIndex() = default;
    // Rows with id == `id` when `id` is set, else rows with cwd == `cwd`; newest first.
    virtual std::vector<CodexThread> threads(std::string_view id, std::string_view cwd) const = 0;
};

// The Antigravity CLI's conversation database <home>/.gemini/antigravity-cli/conversations/<id>.db
// (read-only). Core code sees only this interface; the SQLite implementation is in kt_agents_os.
class IAgyStore {
public:
    virtual ~IAgyStore() = default;
    // The `data` blobs of the newest `max_rows` rows of table gen_metadata, oldest first. Empty on
    // any error (no file, locked, no such table).
    virtual std::vector<std::string> generations(const std::string& db_path, std::size_t max_rows) const = 0;
};

class ContextProbes {
public:
    void add(std::unique_ptr<IContextProbe> p);
    const IContextProbe* find(std::string_view agent) const;

private:
    std::map<std::string, std::unique_ptr<IContextProbe>, std::less<>> probes_;
};

// The Claude, Codex, Qoder and Antigravity probes. `fs`, `codex_index` and `agy_store` (both may
// be null; no store means no Antigravity probe) must outlive them.
ContextProbes builtin_context_probes(const IFileSystem& fs, const IProcessProbe& proc, const ICodexIndex* codex_index,
                                     const ContextPaths& paths, WindowOverrides windows,
                                     const IAgyStore* agy_store = nullptr);

std::unique_ptr<IContextProbe> make_claude_context_probe(const IFileSystem& fs, const IProcessProbe& proc,
                                                         std::string config_dir, WindowOverrides windows);
std::unique_ptr<IContextProbe> make_codex_context_probe(const IFileSystem& fs, const ICodexIndex* index,
                                                        std::string codex_home);
// Codex's sessions for the agent restore: the thread the context probe finds (`codex resume <id>`
// from the command line, else the newest thread of the cwd updated since the process started).
// Codex keeps no per-process file, so a session running elsewhere is not seen; a resumed process
// is ready once it runs.
std::unique_ptr<ISessionSource> make_codex_session_source(const IFileSystem& fs, const ICodexIndex* index,
                                                          std::string codex_home);
std::unique_ptr<IContextProbe> make_qoder_context_probe(const IFileSystem& fs, const IProcessProbe& proc,
                                                        std::string home);
// Antigravity CLI (`agy`): the conversation of the process (--conversation, else its run log,
// else the folder's last conversation) and its database's generation records.
std::unique_ptr<IContextProbe> make_agy_context_probe(const IFileSystem& fs, const IProcessProbe& proc,
                                                      const IAgyStore* store, std::string home, WindowOverrides windows);

// One generation record of the Antigravity CLI (a protobuf blob of gen_metadata.data). The
// field numbers are not documented; read from the CLI's own databases.
struct AgyGeneration {
    std::uint64_t input = 0, cached = 0, output = 0, thinking = 0;
    bool has_usage = false;
    std::string model_id, model_name;
};
// nullopt for a blob that is not a well-formed protobuf message.
std::optional<AgyGeneration> parse_agy_generation(std::string_view blob);
std::unique_ptr<IContextReducer> make_agy_reducer(const IAgyStore* store, WindowOverrides windows);

// Reducers on their own (tests, the tracker).
struct ClaudeReducerOptions {
    std::string configured_model;   // "--model" argument or settings.json "model": "[1m]" means 1M
    WindowOverrides windows;
    bool qoder = false;             // Qoder: window from runtime-config, fill estimated
};
std::unique_ptr<IContextReducer> make_claude_reducer(ClaudeReducerOptions o = {});
std::unique_ptr<IContextReducer> make_codex_reducer();

// ---- Helpers (exposed for tests)

// The window of a model: config override, else the built-in table; nullopt for unknown ids.
std::optional<std::pair<std::uint64_t, Source>> model_window(std::string_view model, const WindowOverrides& overrides);
// '*' glob match.
bool glob_match(std::string_view pattern, std::string_view text);
// "200000", "200k", "1M", "1m" -> tokens; nullopt otherwise.
std::optional<std::uint64_t> parse_token_count(std::string_view s);
// Codex's "context left" (protocol.rs percent_of_context_window_remaining), as percent used:
// 100 - round(clamp((W - B - max(T - B, 0)) / (W - B) * 100, 0, 100)), B = 12000.
double codex_percent_used(std::uint64_t total_tokens, std::uint64_t window);
// "2026-11-21T10:20:30.123Z" (or with an offset) -> ms since the Unix epoch.
std::optional<std::int64_t> parse_iso8601_ms(std::string_view s);
// Claude pid file "status" -> activity.
Activity claude_activity(std::string_view status);

// Newest major versions the reducers were written against; newer ones are "unknown version".
inline constexpr int kClaudeKnownMajor = 2;
inline constexpr int kCodexKnownMajor = 0;
inline constexpr int kQoderKnownMajor = 1;
// The leading number of "2.1.0" / "0.63.0" / "rust-v0.63.0"; nullopt without one.
std::optional<int> version_major(std::string_view v);

}  // namespace kt::agents
