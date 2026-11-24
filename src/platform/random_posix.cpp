#include "platform/random.h"

#include <algorithm>

#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>  // getentropy (macOS 10.12+)
#endif

namespace kt::platform {

// getentropy: glibc 2.25+, macOS 10.12+. Both are below our baselines (glibc 2.28, macOS 13).
Result<void> random_bytes(std::span<std::byte> out) {
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t n = std::min<std::size_t>(256, out.size() - done);
        if (::getentropy(out.data() + done, n) != 0) return last_os_error("getentropy");
        done += n;
    }
    return {};
}

}  // namespace kt::platform
