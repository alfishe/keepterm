// AI agent descriptors and the registry. Core code: data only, no OS calls.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace kt::agents {

class ISessionSource;

// How an agent records its sessions. None: recognition and icon only, no capture.
// PidJsonFile: Claude Code's sessions/<pid>.json. SegmentScan: Qoder's per-run segments.
// ThreadIndex: Codex's thread index and rollouts (cwd and start time, no pid). RunLog: the
// Antigravity CLI's per-run log, which names its pid and conversation.
enum class SourceKind : std::uint8_t { None, PidJsonFile, SegmentScan, ThreadIndex, RunLog };
// How the restore sees that a resumed agent is ready for input. Running: its process runs.
enum class ReadyKind : std::uint8_t { None, StatusField, SourceHasPid, Running };

// One agent CLI. The spans point to storage that outlives the registry (static tables for the
// built-in agents).
struct AgentDescriptor {
    std::string_view id;            // persisted in AgentResume::agent; equals Macterm's raw value
    std::string_view display_name;  // "Claude Code"
    std::string_view icon;          // icon id
    std::string_view command;       // the word typed for the resume: "claude"
    std::span<const std::string_view> names;            // invoked names / images that match
    std::span<const std::string_view> package_markers;  // script path parts (interpreter runs)
    std::span<const std::string_view> one_shot;         // options that make a run non-interactive
    std::span<const std::string_view> subcommands;      // first words that are not a session
    std::span<const std::string_view> selectors_with_value;
    std::span<const std::string_view> bare_selectors;
    std::span<const std::string_view> options_with_value;
    std::span<const std::string_view> resume_args;      // "{id}" stands for the session id
    bool requires_kind_interactive = false;             // Claude: session file "kind"
    SourceKind source = SourceKind::None;
    ReadyKind ready = ReadyKind::None;
    std::chrono::seconds ready_timeout{0};
    std::chrono::seconds post_ready_settle{0};

    bool resumable() const { return source != SourceKind::None && !resume_args.empty(); }
};

class AgentRegistry {
public:
    AgentRegistry();
    ~AgentRegistry();
    AgentRegistry(AgentRegistry&&) noexcept;
    AgentRegistry& operator=(AgentRegistry&&) noexcept;
    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    // claude, qodercli, codex, antigravity (resumable) and the icon-only agents of Macterm's icon set.
    static AgentRegistry builtin();

    // Appends a descriptor (recognition order is insertion order). Pointers returned by find()
    // and recognize() stay valid across later add() calls.
    void add(const AgentDescriptor& d);
    // The session source of agent `id`; replaces an earlier one.
    void attach_source(std::string_view id, std::unique_ptr<ISessionSource> source);

    const std::deque<AgentDescriptor>& all() const { return descriptors_; }
    const AgentDescriptor* find(std::string_view id) const;
    const ISessionSource* source(std::string_view id) const;

private:
    std::deque<AgentDescriptor> descriptors_;
    std::map<std::string, std::unique_ptr<ISessionSource>, std::less<>> sources_;
};

}  // namespace kt::agents
