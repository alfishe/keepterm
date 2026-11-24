// Ending every session host for `keepterm exit` / `keepterm restart`.
//
// Each live host gets KILL mode Suspend: it saves its history and agent record, ends its shell's
// tree and exits, keeping its folder so the next GUI restores the pane (--restore-from) and
// resumes the agent. A host that does not answer, does not exit in time or is unresponsive is
// ended by its process (start-time checked); its folder stays as after a crash. Every step is
// bounded: the call returns within about `budget` whatever the hosts and their programs do.
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "session/os.h"

namespace kt::session {

struct SuspendReport {
    std::size_t saved = 0;    // ended through Suspend
    std::size_t killed = 0;   // ended by their process
    std::size_t failed = 0;   // still running at the end
};

// `last`: the session whose host goes last (the pane the caller runs in: KEEPTERM_SESSION), so
// the caller sees the others end first. Empty: none.
SuspendReport suspend_all_hosts(const os::Paths&, const std::string& last = {},
                                std::chrono::milliseconds budget = std::chrono::seconds(6));

}  // namespace kt::session
