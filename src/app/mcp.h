// The Model Context Protocol server of the control channel. An agent's MCP client
// starts `keepterm-mcp` (src/mcp_bridge), which forwards each JSON-RPC line from its stdin as one
// Mcp frame and prints the answer; the protocol lives here, in the app.
//
// Two tools, as unreal-ng's bridge: `search` finds commands in the catalogue by keywords and
// shows their arguments; `invoke` runs one with its `args`, exactly as the CLI's request would.
// Everything the CLI can do an agent can do this way: the wire commands go to the window, the
// commands the CLI runs without the app (sessions, history, resources, config, ...) run this
// executable as that CLI. The CLI stays the fallback when no MCP client is there.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app/control.h"

namespace kt::app::control::mcp {

struct Context {
    std::string app_version;
    std::vector<std::string> commands;   // the wire commands the app serves (Welcome's list)
    std::string default_pane;            // the bridge's KEEPTERM_PANE, for pane commands
    // Runs one control request ({id, command, needs, args}) and returns its response.
    std::function<json::Value(const json::Value& request)> handle;
    // Runs this executable as a CLI (control::run_self) for the catalogue's local commands.
    std::function<Result<SelfRun>(const std::vector<std::string>& args, int timeout_ms)> run_local;
};

// The CLI words of a local command (CommandInfo::local) with `args` (its named arguments):
// the error names a missing, unknown or mistyped argument.
Result<std::vector<std::string>> local_argv(const CommandInfo& c, const json::Value& args);

// One JSON-RPC message in, the answer out; empty for a notification (or a response the client
// sent). Never throws; malformed input gets a JSON-RPC error.
std::string handle_message(std::string_view line, const Context& ctx);

// The commands of `available` (all when empty) plus the local ones that match `query`, best first. Words of the
// query match the command's name, its CLI form, its help and its argument names; an empty
// query lists everything in catalogue order.
std::vector<const CommandInfo*> search(std::string_view query, std::size_t limit, const std::vector<std::string>& available);

// The protocol versions this server speaks, newest first.
inline constexpr std::string_view kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

}  // namespace kt::app::control::mcp
