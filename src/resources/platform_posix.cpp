// POSIX helpers shared by the Linux and macOS sources.
#include <time.h>
#include <unistd.h>

#include "resources/source.h"

namespace kt::resources {

uint64_t current_thread_cpu_ns() {
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint32_t current_pid() { return static_cast<uint32_t>(::getpid()); }

}  // namespace kt::resources
