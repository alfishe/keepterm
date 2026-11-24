// Starting a session host from the GUI or the CLI.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "session/ids.h"
#include "session/json.h"
#include "session/os.h"
#include "session/protocol.h"

namespace kt::session {

struct HostSpawnOptions {
    std::optional<SessionId> restore_from;
    std::string cwd;
    proto::Resize size;
    bool history = true;
    std::string pty;                 // "", "posix", "conpty", "agent"
    std::vector<std::string> extra;  // more host options (tests: --ring, --checkpoint-bytes, ...)
    std::vector<std::string> argv;   // program instead of the user's shell
    std::string exe;                 // keepterm executable; empty = this process's image
};

struct SpawnedHost {
    SessionId id;
    std::string endpoint;
    uint32_t pid = 0;
    bool may_end_with_gui = false;
};

// Generates a new id, starts `<self> --session-host <id> ...` detached, and waits for its ready
// line. The endpoint accepts clients when this returns.
Result<SpawnedHost> spawn_host(const HostSpawnOptions&, std::chrono::milliseconds timeout = std::chrono::seconds(10));

// The session commands of the CLI (`session *`); they need no GUI.
// Live sessions of the registry and dead ones whose folder is left (restorable), as JSON objects
// with session, state, pid, cwd, cwd_source, title, gui_clients, exit_code.
json::Value list_sessions(const os::Paths&);
// Ends a session: KILL kill_tree, then its process; removes its record and history folder.
Result<void> end_session(const os::Paths&, const SessionId&);
// The text of a live session's screen (plus the scrollback), rendered from a host snapshot.
Result<std::string> session_text(const os::Paths&, const SessionId&, bool scrollback);
// Types `text` into a live session (a control client, as `pane run` does for panes).
Result<void> send_session_text(const os::Paths&, const SessionId&, std::string_view text);

// `keepterm --session <command> ...`; argv[i] is "--session". Returns the exit code.
int cli_main(int argc, char** argv, int i);

}  // namespace kt::session
