#include "session/ids.h"

#include "platform/random.h"

namespace kt::session {
namespace {

constexpr char kAlphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";

// Value of a Crockford base32 symbol, or -1.
int decode_symbol(char c) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    switch (c) {
        case 'o': return 0;
        case 'i':
        case 'l': return 1;
        default: break;
    }
    for (int i = 0; i < 32; ++i) {
        if (kAlphabet[i] == c) return i;
    }
    return -1;
}

}  // namespace

SessionId SessionId::from_bytes(const std::array<std::byte, 10>& bytes) {
    // 80 bits -> 16 symbols of 5 bits, most significant first.
    SessionId id;
    uint32_t acc = 0;
    int bits = 0;
    std::size_t out = 0;
    for (std::byte b : bytes) {
        acc = (acc << 8) | static_cast<uint32_t>(b);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            id.chars[out++] = kAlphabet[(acc >> bits) & 31u];
        }
        acc &= (1u << bits) - 1u;
    }
    return id;
}

Result<SessionId> SessionId::generate() {
    std::array<std::byte, 10> bytes{};
    if (auto r = platform::random_bytes(bytes); !r) return Unexpected(r.error());
    return from_bytes(bytes);
}

Result<SessionId> SessionId::parse(std::string_view text) {
    if (text.size() != kLength) return make_error(Errc::invalid_argument, "session id: wrong length");
    SessionId id;
    for (std::size_t i = 0; i < kLength; ++i) {
        const int v = decode_symbol(text[i]);
        if (v < 0) return make_error(Errc::invalid_argument, "session id: invalid character");
        id.chars[i] = kAlphabet[v];
    }
    return id;
}

}  // namespace kt::session
