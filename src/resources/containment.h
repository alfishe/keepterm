// Containment kinds and accuracy. Header-only: kt_pty and kt_session use them
// without linking kt_resources.
#pragma once

#include <cstdint>

namespace kt::resources {

enum class ContainmentKind : uint8_t { Job, Cgroup, CgroupNoMemory, Subreaper, Tracked, None };
enum class Accuracy : uint8_t { Exact, BestEffort };

}  // namespace kt::resources
