// The GUI's per-window pane state (the old format before the workspace file).
//
// In this format every window holds one pane, and each window keeps one small
// file <state>/windows/<session-id>.json with what a respawn needs: the session id, the last cwd
// and its source, and the title. `keepterm` without arguments reads them at start.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "session/ids.h"
#include "session/os.h"

namespace kt::session {

struct PaneState {
    SessionId session;
    std::string cwd;
    std::string cwd_source;
    std::string title;
    uint64_t updated_unix_ms = 0;
};

std::string windows_dir(const os::Paths&);
Result<void> write_pane_state(const os::Paths&, const PaneState&);
void remove_pane_state(const os::Paths&, const SessionId&);
// All readable pane files, oldest first; unreadable ones are skipped.
std::vector<PaneState> read_pane_states(const os::Paths&);

// What `keepterm` does with a saved pane at start.
enum class PaneAction : uint8_t {
    Attach,    // live host without a GUI client: reattach
    Skip,      // live host that another window already shows
    Restore,   // dead host with history or a record: respawn with --restore-from
    Drop,      // the shell ended (record has an exit code) or nothing is left: forget the pane
};

struct PaneFacts {
    bool host_live = false;
    bool host_unresponsive = false;   // process alive, no HELLO: leave it alone this time
    int gui_clients = 0;
    bool state_folder = false;        // <state>/sessions/<id>/ exists
    bool ended = false;               // record.json has an exit code
};

PaneAction decide_pane(const PaneFacts&);

struct StartupItem {
    PaneState pane;
    PaneAction action = PaneAction::Drop;
};

// Probes every saved pane (`facts` is injectable for tests) and returns the decisions in the
// order of the files.
std::vector<StartupItem> plan_startup(const std::vector<PaneState>& panes,
                                      const std::function<PaneFacts(const PaneState&)>& facts);
// The real facts: registry probe and the state folder.
PaneFacts real_pane_facts(const os::Paths&, const PaneState&);

}  // namespace kt::session
