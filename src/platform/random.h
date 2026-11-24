// Cryptographically secure random bytes from the OS (platform_core).
#pragma once

#include <cstddef>
#include <span>

#include "base/error.h"

namespace kt::platform {

// Fills `out` from the OS CSPRNG: BCryptGenRandom on Windows, getentropy on POSIX.
Result<void> random_bytes(std::span<std::byte> out);

}  // namespace kt::platform
