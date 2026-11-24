#include "agents/registry.h"

#include <array>

#include "agents/session_files.h"

namespace kt::agents {
namespace {

using namespace std::string_view_literals;

// Shared by Claude Code and Qoder CLI, as in Macterm's AgentResume.
constexpr std::array kOneShot{"-p"sv, "--print"sv, "-h"sv, "--help"sv, "-v"sv, "--version"sv,
                              "--list-sessions"sv};
constexpr std::array kSelectorsWithValue{"-r"sv, "--resume"sv, "--session-id"sv, "--from-pr"sv};
constexpr std::array kBareSelectors{"-c"sv, "--continue"sv, "--fork-session"sv};
constexpr std::array kOptionsWithValue{
    "--model"sv, "--permission-mode"sv, "--add-dir"sv, "--allowedTools"sv, "--allowed-tools"sv,
    "--disallowedTools"sv, "--disallowed-tools"sv, "--mcp-config"sv, "--settings"sv, "--agent"sv,
    "--append-system-prompt"sv, "--system-prompt"sv, "--fallback-model"sv, "--output-format"sv,
    "--input-format"sv, "--setting-sources"sv, "--plugin-dir"sv, "--betas"sv, "--agents"sv,
    "--max-turns"sv, "--workspace"sv, "-w"sv};
constexpr std::array kResumeArgs{"--resume"sv, "{id}"sv};

constexpr std::array kClaudeNames{"claude"sv};
constexpr std::array kClaudeMarkers{"node_modules/@anthropic-ai/claude-code/"sv};
constexpr std::array kClaudeSubcommands{"agents"sv, "auth"sv, "config"sv, "doctor"sv, "install"sv,
                                        "mcp"sv, "migrate-installer"sv, "plugin"sv,
                                        "setup-token"sv, "update"sv, "upgrade"sv};

constexpr std::array kQoderNames{"qodercli"sv};
constexpr std::array kQoderSubcommands{"config"sv, "login"sv, "logout"sv, "mcp"sv, "update"sv,
                                       "version"sv};

// Codex CLI: `codex resume <id>` (a subcommand; options may come before it).
constexpr std::array kCodexNames{"codex"sv};
constexpr std::array kCodexMarkers{"node_modules/@openai/codex/"sv};
constexpr std::array kCodexOneShot{"-h"sv, "--help"sv, "-V"sv, "--version"sv};
constexpr std::array kCodexSubcommands{
    "agents"sv, "exec"sv, "e"sv, "review"sv, "login"sv, "logout"sv, "mcp"sv, "mcp-server"sv, "plugin"sv,
    "app-server"sv, "remote-control"sv, "app"sv, "completion"sv, "update"sv, "doctor"sv, "sandbox"sv,
    "debug"sv, "apply"sv, "a"sv, "queue"sv, "archive"sv, "delete"sv, "migrate-rollouts"sv, "unarchive"sv,
    "cloud"sv, "exec-server"sv, "features"sv, "help"sv};
constexpr std::array kCodexBareSelectors{"--last"sv, "--all"sv};
constexpr std::array kCodexOptionsWithValue{
    "-c"sv, "--config"sv, "--enable"sv, "--disable"sv, "--remote"sv, "--remote-auth-token-env"sv, "-i"sv,
    "--image"sv, "-m"sv, "--model"sv, "--local-provider"sv, "-p"sv, "--profile"sv, "-s"sv, "--sandbox"sv,
    "-C"sv, "--cd"sv, "--add-dir"sv, "-a"sv, "--ask-for-approval"sv};
constexpr std::array kCodexResumeArgs{"resume"sv, "{id}"sv};

// Antigravity CLI: `agy --conversation <id>`.
constexpr std::array kAntigravityNames{"agy"sv};
constexpr std::array kAgyOneShot{"-p"sv, "--print"sv, "--prompt"sv, "-h"sv, "--help"sv, "--version"sv};
constexpr std::array kAgySubcommands{"agent"sv, "agents"sv, "changelog"sv, "help"sv, "install"sv, "mcp"sv,
                                     "mic-serve"sv, "models"sv, "plugin"sv, "plugins"sv, "remote-control"sv,
                                     "update"sv};
// -i/--prompt-interactive carries a prompt: a resume must not send it again.
constexpr std::array kAgySelectorsWithValue{"--conversation"sv, "-i"sv, "--prompt-interactive"sv};
constexpr std::array kAgyBareSelectors{"-c"sv, "--continue"sv, "--new-project"sv};
constexpr std::array kAgyOptionsWithValue{"--add-dir"sv, "--agent"sv, "--effort"sv, "--input-format"sv,
                                          "--json-schema"sv, "--log-file"sv, "--mode"sv, "--model"sv,
                                          "--output-format"sv, "--print-timeout"sv, "--project"sv};
constexpr std::array kAgyResumeArgs{"--conversation"sv, "{id}"sv};

// Icon-only agents (Macterm's AgentIcon set). Names as Macterm matches them.
constexpr std::array kGeminiNames{"gemini"sv};
constexpr std::array kGeminiMarkers{"node_modules/@google/gemini-cli/"sv};
constexpr std::array kCopilotNames{"copilot"sv};
constexpr std::array kCopilotMarkers{"node_modules/@github/copilot/"sv};
constexpr std::array kCursorNames{"cursor-agent"sv};
constexpr std::array kOpenCodeNames{"opencode"sv};
constexpr std::array kGrokNames{"grok"sv};
constexpr std::array kPiNames{"pi"sv};
constexpr std::array kOmpNames{"omp"sv};

AgentDescriptor icon_only(std::string_view id, std::string_view display,
                          std::span<const std::string_view> names,
                          std::span<const std::string_view> markers = {}) {
    AgentDescriptor d;
    d.id = id;
    d.display_name = display;
    d.icon = id;
    d.command = names.front();
    d.names = names;
    d.package_markers = markers;
    return d;
}

}  // namespace

AgentRegistry::AgentRegistry() = default;
AgentRegistry::~AgentRegistry() = default;
AgentRegistry::AgentRegistry(AgentRegistry&&) noexcept = default;
AgentRegistry& AgentRegistry::operator=(AgentRegistry&&) noexcept = default;

AgentRegistry AgentRegistry::builtin() {
    AgentRegistry r;

    AgentDescriptor claude;
    claude.id = "claude";
    claude.display_name = "Claude Code";
    claude.icon = "claude";
    claude.command = "claude";
    claude.names = kClaudeNames;
    claude.package_markers = kClaudeMarkers;
    claude.one_shot = kOneShot;
    claude.subcommands = kClaudeSubcommands;
    claude.selectors_with_value = kSelectorsWithValue;
    claude.bare_selectors = kBareSelectors;
    claude.options_with_value = kOptionsWithValue;
    claude.resume_args = kResumeArgs;
    claude.requires_kind_interactive = true;
    claude.source = SourceKind::PidJsonFile;
    claude.ready = ReadyKind::StatusField;
    claude.ready_timeout = std::chrono::seconds(600);
    claude.post_ready_settle = std::chrono::seconds(0);
    r.add(claude);

    AgentDescriptor qoder = claude;
    qoder.id = "qodercli";
    qoder.display_name = "Qoder CLI";
    qoder.icon = "qodercli";
    qoder.command = "qodercli";
    qoder.names = kQoderNames;
    qoder.package_markers = {};  // npm package path: not confirmed yet
    qoder.subcommands = kQoderSubcommands;
    qoder.requires_kind_interactive = false;
    qoder.source = SourceKind::SegmentScan;
    qoder.ready = ReadyKind::SourceHasPid;
    qoder.ready_timeout = std::chrono::seconds(300);
    qoder.post_ready_settle = std::chrono::seconds(3);
    r.add(qoder);

    AgentDescriptor codex = icon_only("codex", "Codex", kCodexNames, kCodexMarkers);
    codex.one_shot = kCodexOneShot;
    codex.subcommands = kCodexSubcommands;
    codex.bare_selectors = kCodexBareSelectors;
    codex.options_with_value = kCodexOptionsWithValue;
    codex.resume_args = kCodexResumeArgs;
    codex.source = SourceKind::ThreadIndex;
    codex.ready = ReadyKind::Running;
    codex.ready_timeout = std::chrono::seconds(60);
    codex.post_ready_settle = std::chrono::seconds(3);
    r.add(codex);
    r.add(icon_only("gemini", "Gemini CLI", kGeminiNames, kGeminiMarkers));
    r.add(icon_only("copilot", "Copilot CLI", kCopilotNames, kCopilotMarkers));
    r.add(icon_only("cursor", "Cursor Agent", kCursorNames));
    r.add(icon_only("opencode", "OpenCode", kOpenCodeNames));
    r.add(icon_only("grok", "Grok", kGrokNames));
    AgentDescriptor agy = icon_only("antigravity", "Antigravity", kAntigravityNames);
    agy.one_shot = kAgyOneShot;
    agy.subcommands = kAgySubcommands;
    agy.selectors_with_value = kAgySelectorsWithValue;
    agy.bare_selectors = kAgyBareSelectors;
    agy.options_with_value = kAgyOptionsWithValue;
    agy.resume_args = kAgyResumeArgs;
    agy.source = SourceKind::RunLog;
    agy.ready = ReadyKind::SourceHasPid;
    agy.ready_timeout = std::chrono::seconds(120);
    agy.post_ready_settle = std::chrono::seconds(2);
    r.add(agy);
    r.add(icon_only("pi", "Pi", kPiNames));
    r.add(icon_only("omp", "Omp", kOmpNames));
    return r;
}

void AgentRegistry::add(const AgentDescriptor& d) { descriptors_.push_back(d); }

void AgentRegistry::attach_source(std::string_view id, std::unique_ptr<ISessionSource> source) {
    sources_[std::string(id)] = std::move(source);
}

const AgentDescriptor* AgentRegistry::find(std::string_view id) const {
    for (const auto& d : descriptors_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

const ISessionSource* AgentRegistry::source(std::string_view id) const {
    auto it = sources_.find(id);
    return it == sources_.end() ? nullptr : it->second.get();
}

}  // namespace kt::agents
