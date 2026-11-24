// The Model Context Protocol server of the control channel. An agent's MCP client
// starts `keepterm-mcp` (src/mcp_bridge), which forwards each JSON-RPC line from its stdin as one
// Mcp frame and prints the answer; the protocol lives here, in the app.
//
// Two tools, as unreal-ng's bridge: `search` finds commands in the catalogue by keywords and
// shows their arguments; `invoke` runs one with its `args`, exactly as the CLI's request would.
// Everything the CLI can do over the channel an agent can do this way; the CLI stays the
// fallback when no MCP client is there.
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
};

// One JSON-RPC message in, the answer out; empty for a notification (or a response the client
// sent). Never throws; malformed input gets a JSON-RPC error.
std::string handle_message(std::string_view line, const Context& ctx);

// The commands of `available` (all when empty) that match `query`, best first. Words of the
// query match the command's name, its CLI form, its help and its argument names; an empty
// query lists everything in catalogue order.
std::vector<const CommandInfo*> search(std::string_view query, std::size_t limit, const std::vector<std::string>& available);

// The protocol versions this server speaks, newest first.
inline constexpr std::string_view kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

}  // namespace kt::app::control::mcp
