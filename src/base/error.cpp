#include "base/error.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kt {

std::string_view to_string(Errc c) {
    switch (c) {
        case Errc::ok: return "ok";
        case Errc::unavailable: return "unavailable";
        case Errc::not_supported: return "not supported";
        case Errc::os_error: return "os error";
        case Errc::timeout: return "timeout";
        case Errc::busy: return "busy";
        case Errc::invalid_argument: return "invalid argument";
        case Errc::lost: return "lost";
        case Errc::corrupt: return "corrupt";
    }
    return "unknown";
}

Unexpected<Error> last_os_error(std::string what) {
#ifdef _WIN32
    const auto code = static_cast<int64_t>(::GetLastError());
#else
    const auto code = static_cast<int64_t>(errno);
    what += ": ";
    what += std::strerror(errno);
#endif
    return make_error(Errc::os_error, std::move(what), code);
}

}  // namespace kt
