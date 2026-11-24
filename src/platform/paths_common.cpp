// OS-independent parts of platform::paths.
#include <system_error>

#include "platform/paths.h"

namespace kt::platform::paths {

bool valid_profile_name(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace kt::platform::paths
