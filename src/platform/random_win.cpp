#include "platform/random.h"

#include <windows.h>
#include <bcrypt.h>

namespace kt::platform {

// BCryptGenRandom with the system-preferred RNG: Windows Vista and later, documented.
Result<void> random_bytes(std::span<std::byte> out) {
    const NTSTATUS st = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                                          static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) return make_error(Errc::os_error, "BCryptGenRandom", static_cast<int64_t>(st));
    return {};
}

}  // namespace kt::platform
