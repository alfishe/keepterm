// The common error value of keepterm modules.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "base/expected.h"

namespace kt {

enum class Errc : uint16_t {
    ok,
    unavailable,
    not_supported,
    os_error,
    timeout,
    busy,
    invalid_argument,
    lost,
    corrupt,
};

std::string_view to_string(Errc);

struct Error {
    Errc code = Errc::ok;
    int64_t os_code = 0;  // GetLastError(), HRESULT, errno
    std::string what;     // short, never terminal text
};

template <class T>
using Result = Expected<T, Error>;

inline Unexpected<Error> make_error(Errc code, std::string what, int64_t os_code = 0) {
    return Unexpected<Error>(Error{code, os_code, std::move(what)});
}

// Last OS error (errno or GetLastError()) as an Error with code os_error.
Unexpected<Error> last_os_error(std::string what);

}  // namespace kt
