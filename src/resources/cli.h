// Headless resource report for testing (in-process collector, via "direct"):
//
//   keepterm --resources [--pid N [--containment job:<name>|cgroup:<path>]] [--tree] [--json]
//                        [--no-cpu] [--cmdline] [--stats]
//
// Without --pid: the machine summary (this process, visible processes) and, when a session
// lister is given, every live session of the user with its totals; --tree adds the session trees
// and the forest of every process of the current user. With --pid: process N is treated as a
// session root and its whole subtree is attributed, summed and printed as a tree.
#pragma once

#include <functional>
#include <vector>

#include "resources/model.h"

namespace kt::resources {

// The live sessions to attribute (the app asks the session registry and every host's
// PROCESS_INFO; kt_resources does not depend on the session layer).
using SessionLister = std::function<std::vector<SessionProcessInfo>()>;

// argv[first] is the first argument after "--resources". Returns the process exit code:
// 0 success, 1 not found / collection error, 2 usage error.
int resources_cli(int argc, char** argv, int first, const SessionLister& sessions = {});

}  // namespace kt::resources
