// The session protocol: framing, TLV payloads and the level-1 message catalogue.
//
// Core code: no OS headers, no I/O. Every decoder takes an untrusted byte span and checks every
// length against it before reading; a malformed input gives Errc::corrupt, never a crash. The
// decoders are therefore fuzz targets as they are.
//
// Wire rules:
//   frame   = u32 length (type + payload, 2..16 MiB), u16 type, payload       (little-endian)
//   type    = bit 15 is must-understand; unknown types without it are skipped
//   payload = fixed layout for OUTPUT, INPUT and SNAPSHOT; TLV (u16 tag, u32 len, value) for the
//             rest. Unknown tags are skipped; a missing tag keeps its default.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "session/ids.h"

namespace kt::session::proto {

inline constexpr uint32_t kMagic = 0x4D544E50;  // "PNTM" little-endian
inline constexpr uint16_t kLevelMin = 1;
inline constexpr uint16_t kLevelMax = 1;
inline constexpr uint32_t kMaxFrame = 16u << 20;      // length field limit
inline constexpr uint32_t kOutputChunk = 64u << 10;   // OUTPUT, INPUT and SNAPSHOT data per frame
inline constexpr uint16_t kMustUnderstand = 0x8000;
inline constexpr std::size_t kMaxString = 64u << 10;
inline constexpr std::size_t kMaxPath = 32u << 10;
inline constexpr std::size_t kHeaderSize = 6;         // u32 length + u16 type

enum class Type : uint16_t {
    Hello = 0x0001, HelloReject = 0x0002, Ping = 0x0003, Pong = 0x0004, Error = 0x0005,
    Attach = 0x0010, Detach = 0x0011, Leader = 0x0012, ClaimLeader = 0x0013,
    SnapshotRequest = 0x0020, Snapshot = 0x0021, ResumeOk = 0x0022,
    Output = 0x0100, Input = 0x0101, Resize = 0x0102, Resized = 0x0103, Resync = 0x0104,
    Title = 0x0200, Cwd = 0x0201, Bell = 0x0202, Notify = 0x0203, PromptMark = 0x0204,
    Attention = 0x0205, Progress = 0x0206,
    ProcessInfoRequest = 0x0300, ProcessInfo = 0x0301, Kill = 0x0302, KillResult = 0x0303,
    ExitStatus = 0x0304, ExitAck = 0x0305, SetOption = 0x0306,
    // Agent context telemetry (agents/context.h). Additive: no must-understand bit, so peers
    // without them skip the frames; a host that has them says "agent-context=1" in HELLO caps.
    AgentContextRequest = 0x0307, AgentContext = 0x0308, AgentStatusline = 0x0309,
};

// True for the types of the level-1 catalogue.
bool known_type(uint16_t type);
std::string_view type_name(uint16_t type);   // for logs; "unknown" otherwise

// ------------------------------------------------------------------------------- framing

struct FrameView {
    uint16_t type = 0;
    std::span<const std::byte> payload;
};

// Decodes one frame from the front of `in`. Returns the bytes consumed, or 0 when `in` does not
// hold a whole frame yet. A length below 2 or above kMaxFrame is Errc::corrupt (the caller sends
// ERROR{frame_too_large} and closes).
Result<std::size_t> decode_frame(std::span<const std::byte> in, FrameView& out);

// Appends a frame header for a payload of `payload_len` bytes.
void encode_header(uint16_t type, std::size_t payload_len, std::vector<std::byte>& out);
void encode_frame(uint16_t type, std::span<const std::byte> payload, std::vector<std::byte>& out);

// ------------------------------------------------------------------------------------- TLV

class TlvWriter {
public:
    explicit TlvWriter(std::vector<std::byte>& out) : out_(out) {}
    void u8(uint16_t tag, uint8_t v);
    void u16(uint16_t tag, uint16_t v);
    void u32(uint16_t tag, uint32_t v);
    void u64(uint16_t tag, uint64_t v);
    void i32(uint16_t tag, int32_t v);
    void str(uint16_t tag, std::string_view v);
    void bytes(uint16_t tag, std::span<const std::byte> v);

private:
    void head(uint16_t tag, std::size_t len);
    std::vector<std::byte>& out_;
};

struct TlvField {
    uint16_t tag = 0;
    std::span<const std::byte> value;
};

// Iterates over TLV fields with bounds checks. next() returns false at the end; error() tells
// whether the end was a truncated field.
class TlvReader {
public:
    explicit TlvReader(std::span<const std::byte> in) : in_(in) {}
    bool next(TlvField& f);
    bool error() const { return error_; }

private:
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
    bool error_ = false;
};

// Value decoders: false when the value has the wrong size or is too long.
bool tlv_u8(const TlvField&, uint8_t&);
bool tlv_u16(const TlvField&, uint16_t&);
bool tlv_u32(const TlvField&, uint32_t&);
bool tlv_u64(const TlvField&, uint64_t&);
bool tlv_i32(const TlvField&, int32_t&);
bool tlv_str(const TlvField&, std::string&, std::size_t max = kMaxString);

// --------------------------------------------------------------------------------- messages

enum class Role : uint8_t { Host = 0, Gui = 1, Cli = 2 };

struct Hello {
    uint32_t magic = kMagic;
    uint16_t level_min = kLevelMin, level_max = kLevelMax;
    std::string build_key;
    std::string version;
    Role role = Role::Gui;
    std::vector<std::string> caps;
    std::optional<SessionId> session;
    uint32_t pid = 0;
    uint32_t width_table_version = 0;
    uint16_t snapshot_major_min = 1, snapshot_major_max = 1;
};

struct HelloReject {
    std::string reason;
    uint16_t level_min = kLevelMin, level_max = kLevelMax;
};

struct Ping { uint64_t nonce = 0; };   // also PONG

enum class ErrorCode : uint16_t {
    FrameTooLarge = 1, UnknownRequiredType = 2, BadMessage = 3, NotAttached = 4, Busy = 5, Internal = 6,
};

struct ErrorMsg {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
    uint16_t ref_type = 0;
};

enum class AttachMode : uint8_t { View = 0, Control = 1 };

struct Resize {
    uint16_t cols = 80, rows = 24, px_w = 0, px_h = 0;
    friend bool operator==(const Resize&, const Resize&) = default;
};

struct Attach {
    AttachMode mode = AttachMode::View;
    Resize size;
    std::string client_name;
};

struct Leader {
    bool is_leader = false;
    uint32_t leader_client_id = 0;
};

struct SnapshotRequest {
    std::optional<uint64_t> resume_from;
    bool screen_only = false, scrollback_only = false, text_only = false;
    uint32_t scrollback_lines = UINT32_MAX;
    uint16_t major_max = 1;
};

enum SnapshotFlags : uint8_t {
    kSnapLast = 1u << 0, kSnapResync = 1u << 1, kSnapScrollbackOnly = 1u << 2, kSnapVtReplay = 1u << 3,
    kSnapTrimmed = 1u << 4,
};

struct SnapshotChunk {   // fixed layout
    uint32_t snapshot_id = 0;
    uint32_t index = 0;
    uint8_t flags = 0;
    uint64_t seq = 0;
    uint16_t major = 1, minor = 0;
    std::span<const std::byte> data;
};

struct ResumeOk { uint64_t from_seq = 0; };

struct Output {   // fixed layout
    uint64_t seq = 0;
    std::span<const std::byte> bytes;
};

struct Input {   // fixed layout
    std::span<const std::byte> bytes;
};

struct Resized {
    Resize size;
    uint64_t seq = 0;
};

enum class ResyncReason : uint8_t { Lag = 0, HostReset = 1 };
struct Resync { ResyncReason reason = ResyncReason::Lag; };

enum class TitleSource : uint8_t { Osc0 = 0, Osc2 = 1, Program = 2 };
struct Title {
    std::string text;
    TitleSource source = TitleSource::Osc2;
    uint64_t seq = 0;
};

struct Cwd {
    std::string path;
    std::string host;
    std::string source;   // "integration", "os_query", "process_peb", "prompt_parse", "last_known"
    uint64_t seq = 0;
};

struct Bell { uint64_t seq = 0; };

enum class NotifyKind : uint8_t { Osc9 = 0, Osc777 = 1, Osc99 = 2 };
struct Notify {
    NotifyKind kind = NotifyKind::Osc9;
    std::string title, body;
    uint64_t seq = 0;
};

struct PromptMarkMsg {
    uint8_t kind = 'A';   // A, B, C, D of OSC 133
    std::optional<int32_t> exit_code;
    uint64_t seq = 0;
};

struct Attention { uint8_t flags = 0; };   // bit 0 unseen output, bit 1 failed, bit 2 bell

struct Progress {
    uint8_t state = 0;
    uint8_t percent = 0;
};

// The process facts the host reports so far; more tags come with the resource monitor and the agents.
// One process of the foreground chain.
struct ChainEntry {
    uint32_t pid = 0;
    uint32_t ppid = 0;
    uint64_t start = 0;           // OS-native creation time (ProcessKey units)
    uint64_t start_unix_ns = 0;   // creation time in ns since the Unix epoch; 0 = unknown
    std::string image;            // lowercase basename without ".exe"
    std::string comm;             // POSIX only
    std::vector<std::string> argv;   // Windows: the PEB's command line split like CommandLineToArgvW
    std::string cwd;              // the OS query (Windows: the PEB's current directory)
    bool operator==(const ChainEntry&) const = default;
};
inline constexpr std::size_t kMaxChain = 8;

struct ProcessInfo {
    uint32_t host_pid = 0;
    uint64_t host_start = 0;
    uint32_t root_pid = 0;
    uint64_t root_start = 0;
    uint32_t foreground_pid = 0;
    bool foreground_is_shell = false;
    std::string cwd;
    std::string cwd_source;
    std::string backend;
    uint8_t containment_kind = 5;   // kt::resources::ContainmentKind, None
    std::string containment_handle;
    std::optional<uint32_t> agent_pid;
    std::optional<uint32_t> posix_sid;
    std::optional<uint64_t> tty_dev;
    std::vector<uint32_t> children;
    uint64_t last_input_ns = 0;
    std::string shell_kind;
    // The chain from the shell's child down to the foreground process; empty = the shell.
    std::vector<ChainEntry> foreground_chain;
    std::string shell_image;      // the shell's lowercase image name (idle-shell test)
    // The agent session the host would resume after a respawn (record.json "agent"); empty =
    // none. agent_restore false: the user turned the resume off for this pane.
    std::string saved_agent, saved_agent_session, saved_agent_dir;
    bool agent_restore = true;
};

// Suspend (keepterm exit / restart): the host saves its history and agent record as at the end
// of a login session, ends the shell's tree and exits, keeping its folder for a --restore-from.
// Hosts of older builds do not decode it and do not answer: the caller then ends the process.
enum class KillMode : uint8_t { Hangup = 0, Terminate = 1, KillTree = 2, EndHost = 3, SignalPids = 4, Suspend = 5 };
struct Kill {
    KillMode mode = KillMode::Hangup;
    uint32_t grace_ms = 500;
    std::vector<uint32_t> targets;   // signal_pids only
};

struct KillResult {
    bool ok = false;
    std::string detail;
};

struct ExitStatus {
    int32_t code = 0;
    int32_t signal = 0;
    bool core = false;
    uint64_t final_seq = 0;
    uint64_t ended_unix_ms = 0;
};

struct ExitAck { bool pane_closed = false; };

struct SetOption {
    std::string key, value;
    uint64_t seq = 0;
};

// ------------------------------------------------------------------- agent context
//
// AGENT_CONTEXT (host -> client): the context fill and counters of the agent in the pane's
// foreground. Sent to every attached client when a value changes (at most once a second), once
// after ATTACH, and as the answer to AGENT_CONTEXT_REQUEST (empty payload; also allowed without
// ATTACH). TLV tags (all optional; a missing tag keeps the default):
//    1 u16 format version (kAgentContextVersion)     2 u8  status (AgentContextStatus)
//    3 str agent id ("claude", "codex", "qodercli")  4 str session id
//    5 str model id      6 str agent version          7 u64 used tokens
//    8 u64 window tokens 9 u8 window source          10 u32 percent x 100 (7123 = 71.23 %)
//   11 u64 input tokens 12 u64 output tokens         13 u64 cache read tokens
//   14 u64 cache write tokens                        15 u32 turns
//   16 u32 compactions  17 u32 API calls             18 u8 flags (AgentContextFlags)
//   19 u8 activity      20 u64 updated (ms since the Unix epoch)
//   21 bytes one rate limit (nested TLV: 1 str name, 2 u32 used percent x 100, 3 u32 window
//      minutes, 4 u64 resets at, seconds since the Unix epoch), repeated, at most 8
//   22 u8 used source   23 u8 limits source          24 u64 cost in micro-USD
//   25 str unrecognized format version               26 u8 warn percent  27 u8 alert percent
//   28 u64 change counter (increases with every new value of this host)
//   29 str session name 30 str session folder (as the agent registered them)
//   31 u64 when the limits were current (ms since the Unix epoch; the API response they came with)
// Sources are kt::agents::Source, activity is kt::agents::Activity (same numbers).
//
// AGENT_STATUSLINE (relay -> host, `keepterm --agent-statusline claude`, no ATTACH): the
// numbers and ids of one Claude Code statusLine JSON.
//    1 str session id   2 str transcript path   3 str model id   4 str model display name
//    5 str version      6 u64 window tokens     7 u64 used tokens (current_usage)
//    8 u32 used percent x 100                   9 u64 cost in micro-USD
//   10 u64 total input 11 u64 total output     12 bytes rate limit (as tag 21 above)
//   13 str pane id (KEEPTERM_PANE)
inline constexpr uint16_t kAgentContextVersion = 1;
inline constexpr std::size_t kMaxAgentLimits = 8;

enum class AgentContextStatus : uint8_t {
    None = 0,            // no recognized agent, telemetry off, or a private pane
    Ok = 1,
    NoData = 2,          // an agent runs, its file is not found or has no usage record yet
    UnknownFormat = 3,   // the agent's files have a format version this build does not know
};

enum AgentContextFlags : uint8_t {
    kCtxAfterCompaction = 1u << 0,   // used tokens are a compaction's result; no call since
    kCtxTotalsSinceAttach = 1u << 1, // the totals cover only what the host read
    kCtxPrivateHidden = 1u << 2,     // a private pane: the values are withheld
};

struct AgentLimit {
    std::string name;
    uint32_t used_percent_x100 = 0;
    uint32_t window_minutes = 0;
    uint64_t resets_at_unix = 0;
    bool operator==(const AgentLimit&) const = default;
};

struct AgentContext {
    uint16_t version = kAgentContextVersion;
    AgentContextStatus status = AgentContextStatus::None;
    std::string agent, session_id, model, agent_version, format_version;
    std::string session_name, session_cwd;   // as the agent registered them
    std::optional<uint64_t> used_tokens, window_tokens;
    uint8_t used_source = 0, window_source = 0, limits_source = 0;
    std::optional<uint32_t> percent_x100;
    uint64_t input_tokens = 0, output_tokens = 0, cache_read_tokens = 0, cache_write_tokens = 0;
    std::optional<uint64_t> cost_micro_usd;
    uint32_t turns = 0, compactions = 0, api_calls = 0;
    uint8_t flags = 0;
    uint8_t activity = 0;
    uint64_t updated_unix_ms = 0;
    std::vector<AgentLimit> limits;
    uint64_t limits_unix_ms = 0;
    uint8_t warn_percent = 70, alert_percent = 90;
    uint64_t seq = 0;
    bool operator==(const AgentContext&) const = default;
};

struct AgentStatusline {
    std::string session_id, transcript_path, model_id, model_name, version, pane;
    std::optional<uint64_t> window_tokens, used_tokens, total_input, total_output, cost_micro_usd;
    std::optional<uint32_t> used_percent_x100;
    std::vector<AgentLimit> limits;
    bool operator==(const AgentStatusline&) const = default;
};

// Encoders append one whole frame (header + payload) to `out`.
void encode(const Hello&, std::vector<std::byte>& out);
void encode(const HelloReject&, std::vector<std::byte>& out);
void encode_ping(const Ping&, bool pong, std::vector<std::byte>& out);
void encode(const ErrorMsg&, std::vector<std::byte>& out);
void encode(const Attach&, std::vector<std::byte>& out);
void encode_empty(Type, std::vector<std::byte>& out);   // DETACH, CLAIM_LEADER, PROCESS_INFO_REQUEST,
                                                         // AGENT_CONTEXT_REQUEST
void encode(const Leader&, std::vector<std::byte>& out);
void encode(const SnapshotRequest&, std::vector<std::byte>& out);
void encode(const SnapshotChunk&, std::vector<std::byte>& out);
void encode(const ResumeOk&, std::vector<std::byte>& out);
void encode(const Output&, std::vector<std::byte>& out);
void encode(const Input&, std::vector<std::byte>& out);
void encode(const Resize&, std::vector<std::byte>& out);
void encode(const Resized&, std::vector<std::byte>& out);
void encode(const Resync&, std::vector<std::byte>& out);
void encode(const Title&, std::vector<std::byte>& out);
void encode(const Cwd&, std::vector<std::byte>& out);
void encode(const Bell&, std::vector<std::byte>& out);
void encode(const Notify&, std::vector<std::byte>& out);
void encode(const PromptMarkMsg&, std::vector<std::byte>& out);
void encode(const Attention&, std::vector<std::byte>& out);
void encode(const Progress&, std::vector<std::byte>& out);
void encode(const ProcessInfo&, std::vector<std::byte>& out);
void encode(const Kill&, std::vector<std::byte>& out);
void encode(const KillResult&, std::vector<std::byte>& out);
void encode(const ExitStatus&, std::vector<std::byte>& out);
void encode(const ExitAck&, std::vector<std::byte>& out);
void encode(const SetOption&, std::vector<std::byte>& out);
void encode(const AgentContext&, std::vector<std::byte>& out);
void encode(const AgentStatusline&, std::vector<std::byte>& out);

// Decoders read a payload (FrameView::payload). Spans in the result point into the payload.
Result<void> decode(std::span<const std::byte> payload, Hello&);
Result<void> decode(std::span<const std::byte> payload, HelloReject&);
Result<void> decode(std::span<const std::byte> payload, Ping&);
Result<void> decode(std::span<const std::byte> payload, ErrorMsg&);
Result<void> decode(std::span<const std::byte> payload, Attach&);
Result<void> decode(std::span<const std::byte> payload, Leader&);
Result<void> decode(std::span<const std::byte> payload, SnapshotRequest&);
Result<void> decode(std::span<const std::byte> payload, SnapshotChunk&);
Result<void> decode(std::span<const std::byte> payload, ResumeOk&);
Result<void> decode(std::span<const std::byte> payload, Output&);
Result<void> decode(std::span<const std::byte> payload, Input&);
Result<void> decode(std::span<const std::byte> payload, Resize&);
Result<void> decode(std::span<const std::byte> payload, Resized&);
Result<void> decode(std::span<const std::byte> payload, Resync&);
Result<void> decode(std::span<const std::byte> payload, Title&);
Result<void> decode(std::span<const std::byte> payload, Cwd&);
Result<void> decode(std::span<const std::byte> payload, Bell&);
Result<void> decode(std::span<const std::byte> payload, Notify&);
Result<void> decode(std::span<const std::byte> payload, PromptMarkMsg&);
Result<void> decode(std::span<const std::byte> payload, Attention&);
Result<void> decode(std::span<const std::byte> payload, Progress&);
Result<void> decode(std::span<const std::byte> payload, ProcessInfo&);
Result<void> decode(std::span<const std::byte> payload, Kill&);
Result<void> decode(std::span<const std::byte> payload, KillResult&);
Result<void> decode(std::span<const std::byte> payload, ExitStatus&);
Result<void> decode(std::span<const std::byte> payload, ExitAck&);
Result<void> decode(std::span<const std::byte> payload, SetOption&);
Result<void> decode(std::span<const std::byte> payload, AgentContext&);
Result<void> decode(std::span<const std::byte> payload, AgentStatusline&);

// ----------------------------------------------------------------------------- negotiation

// Level = min(a.max, b.max), valid if >= max(a.min, b.min).
std::optional<uint16_t> negotiate_level(uint16_t a_min, uint16_t a_max, uint16_t b_min, uint16_t b_max);

// The snapshot major the host writes for a request: the newest major <= `client_max` that the
// host writes and that is >= `client_min`. nullopt = no overlap: send the VT replay instead.
std::optional<uint16_t> pick_snapshot_major(uint16_t host_min, uint16_t host_max, uint16_t client_min,
                                            uint16_t client_max);

// Incremental frame splitter over a byte stream (both endpoints use it).
class FrameBuffer {
public:
    // Appends received bytes.
    void append(std::span<const std::byte> bytes);
    // The next whole frame, or nullopt. An error means the stream is unusable (bad length).
    Result<std::optional<FrameView>> next();
    // Drops consumed bytes; call after the FrameView from next() is no longer used.
    void compact();
    std::size_t buffered() const { return buf_.size() - pos_; }

private:
    std::vector<std::byte> buf_;
    std::size_t pos_ = 0;
};

}  // namespace kt::session::proto
