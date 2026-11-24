// keepterm --command list | run | keymap.
// Titles stay English in the CLI.
#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "commands/registry.h"

namespace kt::commands {

// The "name=value" words of `command run`, typed and checked against the command's arguments.
// The CLI checks them before sending; the running app parses them again from the request.
Result<CommandArgs> parse_run_args(const Command&, std::span<const std::string> words);

// Sends a checked `command run` to the running app; returns the exit code.
struct RunRequest {
    std::string id;
    std::vector<std::string> words;   // name=value
    std::string pane;                 // --pane; empty: the CLI's KEEPTERM_PANE, else the focused pane
    bool force = false;
    bool json = false;
};
using RunForwarder = std::function<int(const RunRequest&)>;

// argv[first] is the subcommand. Returns the process exit code. Without a forwarder `run`
// only checks its arguments and reports that no app is reachable.
int command_cli(int argc, char** argv, int first, const RunForwarder& forward = {});
const char* command_cli_usage();

}  // namespace kt::commands
