// The agent session record and the pure recognition and argument logic.
// Port of Macterm's AgentResume. Core code: no OS headers, no file system.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agents/registry.h"

namespace kt::agents {

// The record. Field names and meaning match Macterm's AgentResume.
struct AgentResume {
    std::string agent;                   // descriptor id: "claude", "qodercli"
    std::string session_id;              // the agent's own id (UUID text for both agents)
    std::string directory;               // where the agent started
    std::vector<std::string> arguments;  // original flags without session selectors and prompt words
    bool operator==(const AgentResume&) const = default;
};

// What the session host knows about one process of a pane's foreground chain.
struct ProcessFacts {
    std::vector<std::string> argv;  // empty: not known (Windows until fetched on demand)
    std::string image;              // basename of the executable; ".exe" and case are normalized
    std::string comm;               // POSIX p_comm / /proc comm; empty on Windows
    std::uint32_t pid = 0;
    std::uint64_t start_time = 0;   // creation time, nanoseconds since the Unix epoch; 0 = unknown
    std::string cwd;                // the process's own directory when known
};

enum class Match : std::uint8_t { No, Yes, NeedsCommandLine };
struct Recognition {
    const AgentDescriptor* agent = nullptr;
    Match match = Match::No;
};

// One process. NeedsCommandLine: the image is an interpreter (or a version-shaped native image)
// and argv is not known yet.
Recognition recognize(const ProcessFacts& facts, const AgentRegistry& registry);

// The name `argv` was invoked as: argv[0]'s basename without a login '-' and without a Windows
// launcher extension, or, for an interpreter, the script's basename without a script extension.
// Lowercase is not applied. nullopt for an empty argv.
std::optional<std::string> invoked_name(std::span<const std::string> argv);
// argv without argv[0], and without the interpreter's options and script when argv[0] is an
// interpreter.
std::vector<std::string> program_arguments(std::span<const std::string> argv);
// The script word of an interpreter argv (nullopt when argv[0] is not an interpreter or has none).
std::optional<std::string> script_path(std::span<const std::string> argv);

bool is_interactive(const AgentDescriptor& d, std::span<const std::string> program_args);
// program_arguments(argv) without session selectors and prompt words.
std::vector<std::string> kept_arguments(const AgentDescriptor& d, std::span<const std::string> argv);

// Helpers shared with the session layer.
bool is_interpreter_name(std::string_view name);  // node, bun, deno, python, python3, py, pythonw
bool looks_like_version(std::string_view name);   // "2.1.326": digits and dots, one dot at least
// `name` lowercased (ASCII), basename, without a Windows launcher extension.
std::string normalize_program_name(std::string_view name);
// The arguments for a log line: values of prompt-carrying options replaced with "<redacted>".
std::vector<std::string> redact_for_log(std::span<const std::string> arguments);

}  // namespace kt::agents
