// Session identifiers.
#pragma once

#include <array>
#include <compare>
#include <string>
#include <string_view>

#include "base/error.h"

namespace kt::session {

// 16 chars, Crockford base32, lowercase, 80 random bits. Example: "k3v9q0t7m2c8x4za".
// After a reboot the respawned session gets a new id; the old one travels as --restore-from.
struct SessionId {
    static constexpr std::size_t kLength = 16;

    std::array<char, kLength> chars{};

    static Result<SessionId> generate();  // platform CSPRNG
    // Accepts upper case and the Crockford aliases (I, L -> 1; O -> 0); rejects U and anything else.
    static Result<SessionId> parse(std::string_view);
    static SessionId from_bytes(const std::array<std::byte, 10>& bytes);

    std::string str() const { return std::string(chars.data(), chars.size()); }

    auto operator<=>(const SessionId&) const = default;
};

}  // namespace kt::session
