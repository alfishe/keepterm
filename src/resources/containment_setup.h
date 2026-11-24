// What the session host creates for its child before the spawn. Library
// kt_containment: the session host links it without the rest of kt_resources.
//
//   Windows  the job is created by the PTY backend; nothing to do here (None).
//   Linux    tier A/B: a session cgroup `keepterm-<profile>-<id>/{host,work}` inside a delegated
//            cgroup v2 subtree; the host moves itself into host/ and the child
//            joins work/ before exec. Tier C: Subreaper when the host is one. The D-Bus
//            StartTransientUnit step (step 2) is not implemented.
//   macOS    None: the PTY backend reports Tracked.
#pragma once

#include <string>
#include <string_view>

#include "resources/containment.h"

namespace kt::resources {

struct HostContainment {
    ContainmentKind kind = ContainmentKind::None;
    std::string handle;   // cgroup path relative to the cgroup2 mount
    int procs_fd = -1;    // work/cgroup.procs, open for writing (CLOEXEC)
    std::string note;     // what happened, for the host log
};

HostContainment setup_host_containment(std::string_view session_id, std::string_view profile, bool subreaper);
// Closes procs_fd after the spawn.
void release_host_containment(HostContainment&);

#ifdef __linux__
// The cgroup steps over an explicit environment, for tests on a fake tree.
struct CgroupEnv {
    std::string mount;          // cgroup2 mount point, "/sys/fs/cgroup"
    std::string self_cgroup;    // this process's cgroup path ("0::<path>" of /proc/self/cgroup)
    bool systemd = false;       // /run/systemd/system exists
    bool move_self = true;      // write our pid into host/cgroup.procs (false in tests)
};
// Whether `dir` (absolute) may be written into: owned by us, cgroup.procs and
// cgroup.subtree_control writable, and either marked delegated by systemd (user.delegate or
// trusted.delegate xattr) or no systemd at all.
bool cgroup_delegated(const std::string& dir, bool systemd);
HostContainment setup_cgroup(const CgroupEnv&, std::string_view session_id, std::string_view profile);
// Removes empty session cgroups of earlier hosts of this profile below `parent`.
void sweep_session_cgroups(const std::string& parent, std::string_view profile);
// The "0::<path>" entry of /proc/self/cgroup text; empty for cgroup v1 only.
std::string unified_cgroup_path(std::string_view proc_self_cgroup);
#endif

}  // namespace kt::resources
