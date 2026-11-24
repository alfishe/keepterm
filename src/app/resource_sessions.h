// The live sessions of the user for `keepterm --resources`: the session registry's
// Live hosts, each asked for PROCESS_INFO.
#pragma once

#include <string>
#include <vector>

#include "resources/model.h"
#include "session/protocol.h"

namespace kt::app {

// One host's PROCESS_INFO as the collector's session description.
inline resources::SessionProcessInfo session_process_info(const std::string& id, const session::proto::ProcessInfo& pi) {
    resources::SessionProcessInfo s;
    s.id = id;
    if (pi.host_pid) s.host = resources::ProcessKey{pi.host_pid, pi.host_start};
    s.root = resources::ProcessKey{pi.root_pid, pi.root_start};
    if (pi.agent_pid) s.agent = resources::ProcessKey{*pi.agent_pid, 0};
    if (pi.containment_kind <= static_cast<uint8_t>(resources::ContainmentKind::None)) {
        s.containment.kind = static_cast<resources::ContainmentKind>(pi.containment_kind);
    }
    s.containment.handle = pi.containment_handle;
    s.containment.accuracy = (s.containment.kind == resources::ContainmentKind::Job ||
                              s.containment.kind == resources::ContainmentKind::Cgroup ||
                              s.containment.kind == resources::ContainmentKind::CgroupNoMemory)
                                 ? resources::Accuracy::Exact
                                 : resources::Accuracy::BestEffort;
    for (uint32_t c : pi.children) s.tracked.push_back(resources::ProcessKey{c, 0});
    if (pi.posix_sid) s.posix_sid = static_cast<int32_t>(*pi.posix_sid);
    s.tty_dev = pi.tty_dev;
    return s;
}

// Every Live session of this user and profile; sessions that do not answer are left out.
std::vector<resources::SessionProcessInfo> live_session_processes();

}  // namespace kt::app
